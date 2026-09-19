#include "NetHostBanStore.h"

#include "NetAuthCrypto.h"
#include "System/System.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace RTE {

	namespace {
		constexpr char c_Magic[8] = {'C', 'C', 'C', 'P', 'H', 'B', 'A', 'N'};
		constexpr char c_MacDomain[] = "CCCP-HOST-BANS-v1";

		void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		uint16_t ReadU16(const uint8_t* data) {
			return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
		}

		uint64_t ReadU64(const uint8_t* data) {
			uint64_t value = 0;
			for (int i = 0; i < 8; ++i) {
				value |= static_cast<uint64_t>(data[i]) << (i * 8);
			}
			return value;
		}

		bool WriteDurably(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
#ifdef _WIN32
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.wstring().c_str(), L"wb") != 0 || file == nullptr) {
				return false;
			}
#else
			FILE* file = std::fopen(path.string().c_str(), "wb");
			if (file == nullptr) {
				return false;
			}
#endif
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
				return false;
			}
			return true;
		}

		bool BoundedText(const std::string& text) {
			return text.size() <= NetProtocol::c_MaxShortTextBytes;
		}

		bool PlausibleSealedSize(uintmax_t size) {
			constexpr uintmax_t header = 8 + 2 + 2;
			constexpr uintmax_t mac = 32;
			constexpr uintmax_t maxRecord = 32 + 8 + 2 + NetProtocol::c_MaxShortTextBytes + 2 + NetProtocol::c_MaxShortTextBytes;
			if (size < header + mac) {
				return false;
			}
			const uintmax_t maxSealed = header + static_cast<uintmax_t>(std::numeric_limits<uint16_t>::max()) * maxRecord + mac;
			return size <= maxSealed;
		}

		NetHostBanRecord* FindRecord(std::vector<NetHostBanRecord>& records, const NetAuthBytes32& identity) {
			for (NetHostBanRecord& record : records) {
				if (record.identity == identity) {
					return &record;
				}
			}
			return nullptr;
		}
	} // namespace

	const char* NetHostBanScopeName(NetHostBanScope scope) {
		switch (scope) {
			case NetHostBanScope::Session: return "Session";
			case NetHostBanScope::UntilRemoved: return "UntilRemoved";
		}
		return "Unknown";
	}

	std::string NetHostBanStore::DefaultPath() {
		return System::GetWorkingDirectory() + System::GetUserdataDirectory() + "NetworkBans";
	}

	void NetHostBanStore::SetPath(std::string path) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Path = std::move(path);
	}

	std::string NetHostBanStore::GetPath() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Path;
	}

	bool NetHostBanStore::PersistentReady() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_PersistentReady;
	}

	std::vector<NetHostBanRecord> NetHostBanStore::List() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Records;
	}

	void NetHostBanStore::ForcePersistFailureForTest(bool fail) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_ForcePersistFail = fail;
	}

	bool NetHostBanStore::SerializePersistent(const std::vector<NetHostBanRecord>& records, std::vector<uint8_t>& out) {
		out.clear();
		out.insert(out.end(), std::begin(c_Magic), std::end(c_Magic));
		AppendU16(out, c_RecordVersion);
		uint16_t count = 0;
		for (const NetHostBanRecord& record : records) {
			if (record.scope == NetHostBanScope::UntilRemoved) {
				++count;
			}
		}
		AppendU16(out, count);
		for (const NetHostBanRecord& record : records) {
			if (record.scope != NetHostBanScope::UntilRemoved || !BoundedText(record.displayAlias) || !BoundedText(record.reason)) {
				continue;
			}
			out.insert(out.end(), record.identity.begin(), record.identity.end());
			AppendU64(out, record.createdUnixMs);
			AppendU16(out, static_cast<uint16_t>(record.displayAlias.size()));
			out.insert(out.end(), record.displayAlias.begin(), record.displayAlias.end());
			AppendU16(out, static_cast<uint16_t>(record.reason.size()));
			out.insert(out.end(), record.reason.begin(), record.reason.end());
		}
		uint8_t mac[32];
		if (!GetNetAuthCrypto().HmacSha256(reinterpret_cast<const uint8_t*>(c_MacDomain), sizeof(c_MacDomain) - 1, out.data(), out.size(), mac)) {
			return false;
		}
		out.insert(out.end(), mac, mac + 32);
		return true;
	}

	bool NetHostBanStore::DeserializePersistent(const std::vector<uint8_t>& bytes, std::vector<NetHostBanRecord>& out) {
		out.clear();
		if (bytes.size() < 8 + 2 + 2 + 32 || std::memcmp(bytes.data(), c_Magic, 8) != 0) {
			return false;
		}
		uint8_t mac[32];
		if (!GetNetAuthCrypto().HmacSha256(reinterpret_cast<const uint8_t*>(c_MacDomain), sizeof(c_MacDomain) - 1, bytes.data(), bytes.size() - 32, mac) ||
		    !NetAuthConstantTimeEquals(mac, bytes.data() + bytes.size() - 32, 32)) {
			return false;
		}
		if (ReadU16(bytes.data() + 8) != c_RecordVersion) {
			return false;
		}
		const uint16_t count = ReadU16(bytes.data() + 10);
		size_t cursor = 12;
		for (uint16_t i = 0; i < count; ++i) {
			if (cursor + 32 + 8 + 2 > bytes.size() - 32) {
				return false;
			}
			NetHostBanRecord record;
			std::memcpy(record.identity.data(), bytes.data() + cursor, 32);
			cursor += 32;
			record.createdUnixMs = ReadU64(bytes.data() + cursor);
			cursor += 8;
			const uint16_t aliasCount = ReadU16(bytes.data() + cursor);
			cursor += 2;
			if (aliasCount > NetProtocol::c_MaxShortTextBytes || cursor + aliasCount + 2 > bytes.size() - 32) {
				return false;
			}
			record.displayAlias.assign(reinterpret_cast<const char*>(bytes.data() + cursor), aliasCount);
			cursor += aliasCount;
			const uint16_t reasonCount = ReadU16(bytes.data() + cursor);
			cursor += 2;
			if (reasonCount > NetProtocol::c_MaxShortTextBytes || cursor + reasonCount > bytes.size() - 32) {
				return false;
			}
			record.reason.assign(reinterpret_cast<const char*>(bytes.data() + cursor), reasonCount);
			cursor += reasonCount;
			record.scope = NetHostBanScope::UntilRemoved;
			out.push_back(record);
		}
		return cursor == bytes.size() - 32;
	}

	bool NetHostBanStore::Load(std::string* error) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return LoadLocked(error);
	}

	bool NetHostBanStore::LoadLocked(std::string* error) {
		const std::filesystem::path path(m_Path);
		std::error_code code;
		const bool present = std::filesystem::exists(path, code);
		if (code) {
			if (error) *error = "could not stat the host ban store";
			m_PersistentReady = false;
			return false;
		}
		if (!present) {
			m_Records.erase(std::remove_if(m_Records.begin(), m_Records.end(), [](const NetHostBanRecord& record) {
				return record.scope == NetHostBanScope::UntilRemoved;
			}), m_Records.end());
			m_PersistentReady = true;
			return true;
		}
		const uintmax_t size = std::filesystem::file_size(path, code);
		if (code || !PlausibleSealedSize(size)) {
			if (error) *error = "could not size the host ban store";
			m_PersistentReady = false;
			return false;
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		FILE* file = nullptr;
#ifdef _WIN32
		if (_wfopen_s(&file, path.wstring().c_str(), L"rb") != 0 || file == nullptr) {
#else
		file = std::fopen(path.string().c_str(), "rb");
		if (file == nullptr) {
#endif
			if (error) *error = "could not read the host ban store";
			m_PersistentReady = false;
			return false;
		}
		const bool read = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
		std::fclose(file);
		std::vector<NetHostBanRecord> persistent;
		if (!read || !DeserializePersistent(bytes, persistent)) {
			if (error) *error = "the host ban store is corrupt";
			m_PersistentReady = false;
			return false;
		}
		std::vector<NetHostBanRecord> kept;
		for (const NetHostBanRecord& record : m_Records) {
			if (record.scope == NetHostBanScope::Session) {
				kept.push_back(record);
			}
		}
		kept.insert(kept.end(), persistent.begin(), persistent.end());
		m_Records.swap(kept);
		m_PersistentReady = true;
		return true;
	}

	bool NetHostBanStore::Persist(std::string* error) {
		if (m_ForcePersistFail) {
			if (error) *error = "ban store persistence forced to fail";
			return false;
		}
		std::vector<uint8_t> bytes;
		if (!SerializePersistent(m_Records, bytes)) {
			if (error) *error = "no crypto provider to seal the host ban store";
			return false;
		}
		const std::filesystem::path path(m_Path);
		std::error_code code;
		if (path.has_parent_path() && !std::filesystem::exists(path.parent_path(), code)) {
			std::filesystem::create_directories(path.parent_path(), code);
		}
		std::filesystem::path temporary = path;
		temporary += ".tmp";
		if (!WriteDurably(temporary, bytes)) {
			if (error) *error = "could not write the host ban store";
			return false;
		}
		std::filesystem::rename(temporary, path, code);
		if (code) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			if (error) *error = "could not publish the host ban store";
			return false;
		}
		return true;
	}

	bool NetHostBanStore::Ban(const NetAuthBytes32& identity, NetHostBanScope scope, const std::string& alias, const std::string& reason, uint64_t sessionId, uint64_t nowUnixMs, std::string* error) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return BanLocked(identity, scope, alias, reason, sessionId, nowUnixMs, error);
	}

	bool NetHostBanStore::BanLocked(const NetAuthBytes32& identity, NetHostBanScope scope, const std::string& alias, const std::string& reason, uint64_t sessionId, uint64_t nowUnixMs, std::string* error) {
		if (scope == NetHostBanScope::UntilRemoved && !m_PersistentReady) {
			if (error) *error = "the host ban store is not loaded";
			return false;
		}
		if (!BoundedText(alias) || !BoundedText(reason)) {
			if (error) *error = "ban alias or reason is too long";
			return false;
		}
		if (NetHostBanRecord* existing = FindRecord(m_Records, identity)) {
			if (scope == NetHostBanScope::UntilRemoved && existing->scope != NetHostBanScope::UntilRemoved) {
				existing->scope = NetHostBanScope::UntilRemoved;
				existing->displayAlias = alias;
				existing->reason = reason;
				if (!Persist(error)) {
					existing->scope = NetHostBanScope::Session;
					return false;
				}
			}
			return true;
		}
		NetHostBanRecord record;
		record.identity = identity;
		record.scope = scope;
		record.createdUnixMs = nowUnixMs;
		record.sessionId = sessionId;
		record.displayAlias = alias;
		record.reason = reason;
		m_Records.push_back(record);
		if (scope == NetHostBanScope::UntilRemoved && !Persist(error)) {
			m_Records.pop_back();
			return false;
		}
		return true;
	}

	bool NetHostBanStore::Unban(const NetAuthBytes32& identity, std::string* error) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return UnbanLocked(identity, error);
	}

	bool NetHostBanStore::UnbanLocked(const NetAuthBytes32& identity, std::string* error) {
		const auto found = std::find_if(m_Records.begin(), m_Records.end(), [&identity](const NetHostBanRecord& record) {
			return record.identity == identity;
		});
		if (found == m_Records.end()) {
			return true;
		}
		const NetHostBanRecord removed = *found;
		m_Records.erase(found);
		if (removed.scope == NetHostBanScope::UntilRemoved && !Persist(error)) {
			m_Records.push_back(removed);
			return false;
		}
		return true;
	}

	bool NetHostBanStore::IsBanned(const NetAuthBytes32& identity, uint64_t sessionId) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return IsBannedLocked(identity, sessionId);
	}

	bool NetHostBanStore::IsBannedLocked(const NetAuthBytes32& identity, uint64_t sessionId) const {
		for (const NetHostBanRecord& record : m_Records) {
			if (record.identity != identity) {
				continue;
			}
			if (record.scope == NetHostBanScope::UntilRemoved || record.sessionId == sessionId) {
				return true;
			}
		}
		return false;
	}

	void NetHostBanStore::EndSession(uint64_t sessionId) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Records.erase(std::remove_if(m_Records.begin(), m_Records.end(), [sessionId](const NetHostBanRecord& record) {
			return record.scope == NetHostBanScope::Session && record.sessionId == sessionId;
		}), m_Records.end());
	}

} // namespace RTE
