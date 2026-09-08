#include "NetReconnectTicketStore.h"

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
		// magic 8 + version 2 + epoch 16 + seat 2 + generation 4 + credential 32 + session 8 +
		// issuedAt 8 + configHash 32 + address length 2 = 114, then the address, then the 32 B mac.
		constexpr size_t c_FixedBytes = 114;

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
		if (record.recordVersion != c_RecordVersion || record.holderGeneration == 0 ||
		    record.hostAddress.size() > c_MaxHostAddressBytes) {
			return false;
		}
		out.clear();
		out.reserve(c_FixedBytes + record.hostAddress.size());
		out.insert(out.end(), std::begin(c_Magic), std::end(c_Magic));
		AppendU16LE(out, record.recordVersion);
		out.insert(out.end(), record.epoch.begin(), record.epoch.end());
		AppendU16LE(out, record.stableSeat);
		AppendU32LE(out, record.holderGeneration);
		out.insert(out.end(), record.credential.begin(), record.credential.end());
		AppendU64LE(out, record.hostSessionId);
		AppendU64LE(out, record.issuedAtUnixMs);
		out.insert(out.end(), record.matchConfigHash.begin(), record.matchConfigHash.end());
		AppendU16LE(out, static_cast<uint16_t>(record.hostAddress.size()));
		out.insert(out.end(), record.hostAddress.begin(), record.hostAddress.end());
		return out.size() == c_FixedBytes + record.hostAddress.size();
	}

	bool NetReconnectTicketStore::Deserialize(const std::vector<uint8_t>& bytes, NetH4TicketRecord& out) {
		if (bytes.size() < c_FixedBytes || std::memcmp(bytes.data(), c_Magic, sizeof(c_Magic)) != 0) {
			return false;
		}
		size_t offset = sizeof(c_Magic);
		NetH4TicketRecord record;
		record.recordVersion = ReadU16LE(bytes.data() + offset);
		offset += 2;
		if (record.recordVersion != c_RecordVersion) {
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
		if (addressBytes > c_MaxHostAddressBytes || bytes.size() != offset + addressBytes) {
			return false;
		}
		record.hostAddress.assign(reinterpret_cast<const char*>(bytes.data() + offset), addressBytes);
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

	NetH4TicketLoadResult NetReconnectTicketStore::Load(uint64_t nowUnixMs, NetH4TicketRecord& out, std::string* error) {
		std::error_code code;
		if (!std::filesystem::exists(m_Path, code)) {
			SetError(error, "no recovery record");
			return NetH4TicketLoadResult::Missing;
		}
		std::ifstream file(m_Path, std::ios::binary);
		if (!file) {
			++m_RefusedLoads;
			SetError(error, "the recovery record could not be read");
			return NetH4TicketLoadResult::Corrupt;
		}
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (bytes.size() < sizeof(NetAuthBytes32)) {
			++m_RefusedLoads;
			SetError(error, "the recovery record is truncated");
			return NetH4TicketLoadResult::Corrupt;
		}
		const std::vector<uint8_t> body(bytes.begin(), bytes.end() - static_cast<long>(sizeof(NetAuthBytes32)));
		NetAuthBytes32 mac{};
		std::memcpy(mac.data(), bytes.data() + body.size(), mac.size());
		NetH4TicketRecord record;
		if (!Deserialize(body, record)) {
			++m_RefusedLoads;
			SetError(error, "the recovery record is damaged");
			return NetH4TicketLoadResult::Corrupt;
		}
		if (!NetH4VerifyTicketRecord(record.credential, body, mac)) {
			++m_RefusedLoads;
			SetError(error, "the recovery record failed its integrity check");
			return NetH4TicketLoadResult::Corrupt;
		}
		if (nowUnixMs >= record.issuedAtUnixMs && nowUnixMs - record.issuedAtUnixMs > c_MaxRecordAgeMs) {
			++m_RefusedLoads;
			SetError(error, "the recovery record is too old to offer");
			return NetH4TicketLoadResult::Stale;
		}
		out = record;
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
		++m_Clears;
		return true;
	}

	bool NetReconnectTicketStore::HasRecord() const {
		std::error_code code;
		return std::filesystem::exists(m_Path, code);
	}

} // namespace RTE
