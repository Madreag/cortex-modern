#include "NetReconnectTicketStore.h"
#include "NetA7Journal.h"
#include "NetMatchConfig.h"

#include "NetReconnectTranscript.h"
#include "System/System.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace RTE {

	namespace {
		constexpr char c_Magic[8] = {'C', 'C', 'C', 'P', 'H', '4', 'T', 'K'};
		constexpr size_t c_FixedBytes = NetReconnectTicketStore::c_FixedBytes;
		constexpr uint8_t c_PersistentWorldFlag = 1;

		void AppendU16LE(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU32LE(std::vector<uint8_t>& out, uint32_t value) {
			for (int i = 0; i < 4; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		void AppendU64LE(std::vector<uint8_t>& out, uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		uint16_t ReadU16LE(const uint8_t* data) {
			return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
		}

		uint32_t ReadU32LE(const uint8_t* data) {
			uint32_t value = 0;
			for (int i = 0; i < 4; ++i) {
				value |= static_cast<uint32_t>(data[i]) << (i * 8);
			}
			return value;
		}

		uint64_t ReadU64LE(const uint8_t* data) {
			uint64_t value = 0;
			for (int i = 0; i < 8; ++i) {
				value |= static_cast<uint64_t>(data[i]) << (i * 8);
			}
			return value;
		}

		void SetError(std::string* error, std::string text) {
			if (error) {
				*error = std::move(text);
			}
		}

		// The record is only durable once the bytes have left the C library AND the OS cache: a crash
		// between the two would leave a half-written temporary that the replace never promotes.
		bool WriteFileDurably(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string* error) {
#ifdef _WIN32
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.wstring().c_str(), L"wb") != 0 || file == nullptr) {
#else
			FILE* file = std::fopen(path.string().c_str(), "wb");
			if (file == nullptr) {
#endif
				SetError(error, "could not open the ticket store for writing");
				return false;
			}
			const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
			const bool flushed = wrote && std::fflush(file) == 0;
#ifdef _WIN32
			const bool synced = flushed && _commit(_fileno(file)) == 0;
#else
			const bool synced = flushed && fsync(fileno(file)) == 0;
#endif
			std::fclose(file);
			if (!synced) {
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
				SetError(error, "could not flush the ticket store to disk");
				return false;
			}
			return true;
		}
	} // namespace

	std::string NetReconnectTicketStore::DefaultPath() {
		return System::GetWorkingDirectory() + System::GetUserdataDirectory() + "reconnect.ticket";
	}

	void NetReconnectTicketStore::SetPath(std::string path) {
		m_Path = std::move(path);
	}

	bool NetReconnectTicketStore::Serialize(const NetH4TicketRecord& record, std::vector<uint8_t>& out) {
		const uint16_t version = record.recordVersion;
		if ((version != c_RecordVersion && version != c_DirectoryRecordVersion && version != c_LegacyRecordVersion) ||
		    record.holderGeneration == 0 || record.hostAddress.size() > c_MaxHostAddressBytes ||
		    record.directorySessionId.size() > c_MaxDirectorySessionIdBytes) {
			return false;
		}
		// A body that cannot spell the world flag must not be handed a record that carries one.
		if (record.persistentWorld && version < c_RecordVersion) {
			return false;
		}
		out.clear();
		out.reserve(c_FixedBytes + record.hostAddress.size() + 2 + record.directorySessionId.size());
		out.insert(out.end(), std::begin(c_Magic), std::end(c_Magic));
		AppendU16LE(out, version);
		out.insert(out.end(), record.epoch.begin(), record.epoch.end());
		AppendU16LE(out, record.stableSeat);
		AppendU32LE(out, record.holderGeneration);
		out.insert(out.end(), record.credential.begin(), record.credential.end());
		AppendU64LE(out, record.hostSessionId);
		AppendU64LE(out, record.issuedAtUnixMs);
		out.insert(out.end(), record.matchConfigHash.begin(), record.matchConfigHash.end());
		AppendU16LE(out, static_cast<uint16_t>(record.hostAddress.size()));
		out.insert(out.end(), record.hostAddress.begin(), record.hostAddress.end());
		if (version >= c_DirectoryRecordVersion) {
			AppendU16LE(out, static_cast<uint16_t>(record.directorySessionId.size()));
			out.insert(out.end(), record.directorySessionId.begin(), record.directorySessionId.end());
		}
		if (version >= c_RecordVersion) {
			out.push_back(record.persistentWorld ? c_PersistentWorldFlag : uint8_t{0});
		}
		const size_t expected = c_FixedBytes + record.hostAddress.size() +
		    (version >= c_DirectoryRecordVersion ? 2 + record.directorySessionId.size() : 0) +
		    (version >= c_RecordVersion ? 1 : 0);
		return out.size() == expected;
	}

	bool NetReconnectTicketStore::Deserialize(const std::vector<uint8_t>& bytes, NetH4TicketRecord& out) {
		if (bytes.size() < c_FixedBytes || std::memcmp(bytes.data(), c_Magic, sizeof(c_Magic)) != 0) {
			return false;
		}
		size_t offset = sizeof(c_Magic);
		NetH4TicketRecord record;
		record.recordVersion = ReadU16LE(bytes.data() + offset);
		offset += 2;
		if (record.recordVersion != c_RecordVersion && record.recordVersion != c_DirectoryRecordVersion &&
		    record.recordVersion != c_LegacyRecordVersion) {
			return false;
		}
		std::memcpy(record.epoch.data(), bytes.data() + offset, record.epoch.size());
		offset += record.epoch.size();
		record.stableSeat = ReadU16LE(bytes.data() + offset);
		offset += 2;
		record.holderGeneration = ReadU32LE(bytes.data() + offset);
		offset += 4;
		std::memcpy(record.credential.data(), bytes.data() + offset, record.credential.size());
		offset += record.credential.size();
		record.hostSessionId = ReadU64LE(bytes.data() + offset);
		offset += 8;
		record.issuedAtUnixMs = ReadU64LE(bytes.data() + offset);
		offset += 8;
		std::memcpy(record.matchConfigHash.data(), bytes.data() + offset, record.matchConfigHash.size());
		offset += record.matchConfigHash.size();
		const uint16_t addressBytes = ReadU16LE(bytes.data() + offset);
		offset += 2;
		if (addressBytes > c_MaxHostAddressBytes || bytes.size() < offset + addressBytes) {
			return false;
		}
		record.hostAddress.assign(reinterpret_cast<const char*>(bytes.data() + offset), addressBytes);
		offset += addressBytes;
		// v3 appends one flag byte after the v2 body, so a v2 reader's length check is unchanged.
		const size_t flagBytes = record.recordVersion >= c_RecordVersion ? 1 : 0;
		if (record.recordVersion >= c_DirectoryRecordVersion) {
			if (bytes.size() < offset + 2) {
				return false;
			}
			const uint16_t sessionBytes = ReadU16LE(bytes.data() + offset);
			offset += 2;
			if (sessionBytes > c_MaxDirectorySessionIdBytes || bytes.size() != offset + sessionBytes + flagBytes) {
				return false;
			}
			record.directorySessionId.assign(reinterpret_cast<const char*>(bytes.data() + offset), sessionBytes);
			offset += sessionBytes;
		} else if (bytes.size() != offset) {
			return false;
		}
		if (flagBytes != 0) {
			const uint8_t flags = bytes[offset];
			if ((flags & ~c_PersistentWorldFlag) != 0) {
				return false;
			}
			record.persistentWorld = (flags & c_PersistentWorldFlag) != 0;
		}
		// Generation 0 names no holder, so it can prove nothing.
		if (record.holderGeneration == 0) {
			return false;
		}
		out = record;
		return true;
	}

	bool NetReconnectTicketStore::Store(const NetH4TicketRecord& record, std::string* error) {
		std::vector<uint8_t> bytes;
		if (!Serialize(record, bytes)) {
			++m_StoreFailures;
			SetError(error, "the ticket record is not well formed");
			return false;
		}
		NetAuthBytes32 mac{};
		if (!NetH4MacTicketRecord(record.credential, bytes, mac)) {
			++m_StoreFailures;
			SetError(error, "no crypto provider to mac the ticket record");
			return false;
		}
		bytes.insert(bytes.end(), mac.begin(), mac.end());

		const std::filesystem::path path(m_Path);
		std::error_code code;
		if (path.has_parent_path() && !std::filesystem::exists(path.parent_path(), code)) {
			std::filesystem::create_directories(path.parent_path(), code);
		}
		std::filesystem::path temporary = path;
		temporary += ".tmp";
		if (!WriteFileDurably(temporary, bytes, error)) {
			++m_StoreFailures;
			return false;
		}
		// Replace, never truncate-in-place: a torn write must not be able to eat the live record.
		std::filesystem::rename(temporary, path, code);
		if (code) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			++m_StoreFailures;
			SetError(error, "could not replace the ticket store: " + code.message());
			return false;
		}
		std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, code);
		++m_Stores;
		return true;
	}

	bool NetReconnectTicketStore::DismissOffer(const NetH4TicketRecord& record, std::string* error) {
		// Bind the preference to the canonical ticket, so a new seat or credential has its own offer.
		std::vector<uint8_t> body;
		NetAuthBytes32 fingerprint{};
		if (!Serialize(record, body) || !NetH4MacTicketRecord(record.credential, body, fingerprint)) {
			SetError(error, "could not identify the rejoin offer to dismiss");
			return false;
		}
		const std::vector<uint8_t> bytes(fingerprint.begin(), fingerprint.end());
		const std::filesystem::path path(m_Path + ".dismissed"), temporary(m_Path + ".dismissed.tmp");
		if (!WriteFileDurably(temporary, bytes, error)) return false;
		std::error_code code;
		std::filesystem::rename(temporary, path, code);
		if (code) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			SetError(error, "could not save the rejoin dismissal: " + code.message());
			return false;
		}
		std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, code);
		return true;
	}

	bool NetReconnectTicketStore::IsOfferDismissed(const NetH4TicketRecord& record) const {
		std::error_code code;
		if (std::filesystem::file_size(m_Path + ".dismissed", code) != sizeof(NetAuthBytes32) || code) return false;
		std::ifstream file(m_Path + ".dismissed", std::ios::binary);
		NetAuthBytes32 fingerprint{};
		if (!file.read(reinterpret_cast<char*>(fingerprint.data()), fingerprint.size()) || file.peek() != std::char_traits<char>::eof()) return false;
		std::vector<uint8_t> body;
		return Serialize(record, body) && NetH4VerifyTicketRecord(record.credential, body, fingerprint);
	}

	NetH4TicketLoadResult NetReconnectTicketStore::Read(uint64_t nowUnixMs, NetH4TicketRecord& out, std::string* error, std::vector<uint8_t>& bytes) const {
		std::error_code code;
		if (!std::filesystem::exists(m_Path, code)) {
			SetError(error, "no recovery record");
			return NetH4TicketLoadResult::Missing;
		}
		std::ifstream file(m_Path, std::ios::binary);
		if (!file) {
			SetError(error, "the recovery record could not be read");
			return NetH4TicketLoadResult::Corrupt;
		}
		bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		if (bytes.size() < sizeof(NetAuthBytes32)) {
			SetError(error, "the recovery record is truncated");
			return NetH4TicketLoadResult::Corrupt;
		}
		const std::vector<uint8_t> body(bytes.begin(), bytes.end() - static_cast<long>(sizeof(NetAuthBytes32)));
		NetAuthBytes32 mac{};
		std::memcpy(mac.data(), bytes.data() + body.size(), mac.size());
		NetH4TicketRecord record;
		if (!Deserialize(body, record)) {
			SetError(error, "the recovery record is damaged");
			return NetH4TicketLoadResult::Corrupt;
		}
		if (!NetH4VerifyTicketRecord(record.credential, body, mac)) {
			SetError(error, "the recovery record failed its integrity check");
			return NetH4TicketLoadResult::Corrupt;
		}
		if (nowUnixMs >= record.issuedAtUnixMs && nowUnixMs - record.issuedAtUnixMs > c_MaxRecordAgeMs) {
			SetError(error, "the recovery record is too old to offer");
			return NetH4TicketLoadResult::Stale;
		}
		out = record;
		return NetH4TicketLoadResult::Loaded;
	}

	NetH4TicketLoadResult NetReconnectTicketStore::Check(uint64_t nowUnixMs) const {
		NetH4TicketRecord record;
		std::vector<uint8_t> bytes;
		return Read(nowUnixMs, record, nullptr, bytes);
	}

	NetH4TicketLoadResult NetReconnectTicketStore::Load(uint64_t nowUnixMs, NetH4TicketRecord& out, std::string* error) {
		if (NetA7Journal::Enabled()) m_A7LoadedSha256.clear();
		std::vector<uint8_t> bytes;
		const NetH4TicketLoadResult result = Read(nowUnixMs, out, error, bytes);
		if (result == NetH4TicketLoadResult::Missing) return result;
		if (result != NetH4TicketLoadResult::Loaded) {
			++m_RefusedLoads;
			return result;
		}
		if (NetA7Journal::Enabled()) {
			m_A7LoadedSha256 = NetA7Journal::Sha256(bytes.data(), bytes.size());
			if (m_A7LoadedSha256.empty()) NetA7Journal::Gap("loaded ticket SHA-256 unavailable");
		}
		++m_Loads;
		return NetH4TicketLoadResult::Loaded;
	}

	bool NetReconnectTicketStore::Clear(std::string* error) {
		std::error_code code;
		std::filesystem::remove(m_Path, code);
		if (code) {
			SetError(error, "could not delete the recovery record: " + code.message());
			return false;
		}
		std::filesystem::remove(m_Path + ".routes", code);
		if (code) { SetError(error, "could not delete the recovery routes: " + code.message()); return false; }
		std::filesystem::remove(m_Path + ".dismissed", code);
		if (code) { SetError(error, "could not delete the rejoin dismissal: " + code.message()); return false; }
		++m_Clears;
		return true;
	}

	bool NetReconnectTicketStore::StoreRoutes(const NetH4TicketRecord& record, const std::vector<NetH4TicketRoute>& routes, std::string* error) {
		if (record.holderGeneration == 0 || routes.size() > NetMatchConfigUtil::c_MaxPeerCount * NetMatchConfigUtil::c_MaxMigrationAddresses) return false;
		std::vector<uint8_t> bytes{'C', 'C', 'C', 'P', 'H', '4', 'R', 'T'};
		bytes.insert(bytes.end(), record.epoch.begin(), record.epoch.end());
		AppendU16LE(bytes, record.stableSeat); AppendU32LE(bytes, record.holderGeneration);
		AppendU64LE(bytes, record.hostSessionId); AppendU64LE(bytes, record.issuedAtUnixMs);
		AppendU16LE(bytes, static_cast<uint16_t>(routes.size()));
		for (const auto& route: routes) {
			if (route.address.empty() || route.address.size() > c_MaxHostAddressBytes || route.port == 0) return false;
			AppendU16LE(bytes, static_cast<uint16_t>(route.address.size()));
			bytes.insert(bytes.end(), route.address.begin(), route.address.end()); AppendU16LE(bytes, route.port);
		}
		NetAuthBytes32 mac{};
		if (!NetH4MacTicketRecord(record.credential, bytes, mac)) return false;
		bytes.insert(bytes.end(), mac.begin(), mac.end());
		const std::filesystem::path path(m_Path + ".routes"), temporary(m_Path + ".routes.tmp");
		if (!WriteFileDurably(temporary, bytes, error)) return false;
		std::error_code code;
		std::filesystem::rename(temporary, path, code);
		if (code) { std::error_code ignored; std::filesystem::remove(temporary, ignored); SetError(error, "could not replace the recovery routes: " + code.message()); return false; }
		std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, code);
		return true;
	}

	std::vector<NetH4TicketRoute> NetReconnectTicketStore::LoadRoutes(const NetH4TicketRecord& record) const {
		constexpr size_t header = 48;
		constexpr size_t maximum = header + NetMatchConfigUtil::c_MaxPeerCount * NetMatchConfigUtil::c_MaxMigrationAddresses * (c_MaxHostAddressBytes + 4) + 32;
		std::error_code code;
		const auto size = std::filesystem::file_size(m_Path + ".routes", code);
		if (code || size < header + 32 || size > maximum) return {};
		std::ifstream file(m_Path + ".routes", std::ios::binary);
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (bytes.size() != size || std::memcmp(bytes.data(), "CCCPH4RT", 8) != 0) return {};
		const std::vector<uint8_t> body(bytes.begin(), bytes.end() - 32);
		NetAuthBytes32 mac{}; std::memcpy(mac.data(), bytes.data() + body.size(), mac.size());
		if (!NetH4VerifyTicketRecord(record.credential, body, mac) ||
		    std::memcmp(body.data() + 8, record.epoch.data(), record.epoch.size()) != 0 ||
		    ReadU16LE(body.data() + 24) != record.stableSeat || ReadU32LE(body.data() + 26) != record.holderGeneration ||
		    ReadU64LE(body.data() + 30) != record.hostSessionId || ReadU64LE(body.data() + 38) != record.issuedAtUnixMs) return {};
		const auto count = ReadU16LE(body.data() + 46);
		if (count > NetMatchConfigUtil::c_MaxPeerCount * NetMatchConfigUtil::c_MaxMigrationAddresses) return {};
		std::vector<NetH4TicketRoute> routes;
		size_t offset = header;
		for (size_t index = 0; index < count; ++index) {
			if (offset + 2 > body.size()) return {};
			const auto length = ReadU16LE(body.data() + offset); offset += 2;
			if (length == 0 || length > c_MaxHostAddressBytes || offset + length + 2 > body.size()) return {};
			NetH4TicketRoute route;
			route.address.assign(reinterpret_cast<const char*>(body.data() + offset), length); offset += length;
			route.port = ReadU16LE(body.data() + offset); offset += 2;
			if (route.port == 0) return {};
			routes.push_back(std::move(route));
		}
		return offset == body.size() ? routes : std::vector<NetH4TicketRoute>{};
	}

	bool NetReconnectTicketStore::HasRecord() const {
		std::error_code code;
		return std::filesystem::exists(m_Path, code);
	}

} // namespace RTE
