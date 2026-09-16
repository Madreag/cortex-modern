#pragma once

#include "NetProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	enum class NetHostBanScope : uint8_t {
		Session = 0,
		UntilRemoved = 1,
	};

	/// Public identity the host refused. No private key, no ticket, no IP.
	struct NetHostBanRecord {
		NetAuthBytes32 identity{};
		NetHostBanScope scope = NetHostBanScope::Session;
		uint64_t createdUnixMs = 0;
		uint64_t sessionId = 0;
		std::string displayAlias;
		std::string reason;
	};

	/// Session bans die with the hosted session; Until Removed lives in Userdata/NetworkBans.
	class NetHostBanStore {
	public:
		static constexpr uint16_t c_RecordVersion = 1;
		static std::string DefaultPath();

		void SetPath(std::string path);
		const std::string& GetPath() const { return m_Path; }
		bool Load(std::string* error = nullptr);
		bool PersistentReady() const { return m_PersistentReady; }
		bool Ban(const NetAuthBytes32& identity, NetHostBanScope scope, const std::string& alias, const std::string& reason, uint64_t sessionId, uint64_t nowUnixMs, std::string* error = nullptr);
		bool Unban(const NetAuthBytes32& identity, std::string* error = nullptr);
		bool IsBanned(const NetAuthBytes32& identity, uint64_t sessionId) const;
		void EndSession(uint64_t sessionId);
		std::vector<NetHostBanRecord> List() const { return m_Records; }
		void ForcePersistFailureForTest(bool fail) { m_ForcePersistFail = fail; }

	private:
		bool Persist(std::string* error);
		static bool SerializePersistent(const std::vector<NetHostBanRecord>& records, std::vector<uint8_t>& out);
		static bool DeserializePersistent(const std::vector<uint8_t>& bytes, std::vector<NetHostBanRecord>& out);

		std::string m_Path = DefaultPath();
		std::vector<NetHostBanRecord> m_Records;
		bool m_PersistentReady = false;
		bool m_ForcePersistFail = false;
	};

	const char* NetHostBanScopeName(NetHostBanScope scope);

} // namespace RTE
