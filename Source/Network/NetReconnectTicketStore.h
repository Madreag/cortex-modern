#pragma once

#include "NetProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	/// The recovery record a client keeps so it can prove it held its seat. Everything a Reclaim and
	/// its proof need, plus enough context to tell one hosted session's record from another's.
	struct NetH4TicketRecord {
		uint16_t recordVersion = 1;
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		NetAuthBytes32 credential{};
		uint64_t hostSessionId = 0;
		std::string hostAddress;
		uint64_t issuedAtUnixMs = 0;
		NetHash32 matchConfigHash{};

		bool operator==(const NetH4TicketRecord&) const = default;
	};

	/// Why a load produced no record. The UX distinguishes these; the protocol does not.
	enum class NetH4TicketLoadResult : uint8_t {
		Loaded = 0,
		Missing = 1,
		Corrupt = 2,
		Stale = 3,
	};

	/// The client's durable ticket store: exactly one record at one injectable path, written by a
	/// flushed temporary plus an atomic replace so a failed write leaves the previous record intact.
	/// Deleted only on a LeaveAck, at a confirmed hosted-session end, or past the outer age bound.
	class NetReconnectTicketStore {
	public:
		static constexpr uint16_t c_RecordVersion = 1;
		// Long enough to outlast any single session, short enough that a next-day launch is not
		// offered a dead match. The record is worthless once the host's epoch is gone.
		static constexpr uint64_t c_MaxRecordAgeMs = 24ULL * 60ULL * 60ULL * 1000ULL;
		static constexpr size_t c_MaxHostAddressBytes = NetProtocol::c_MaxShortTextBytes;

		/// The default resolved path, `<working>/<Userdata>/reconnect.ticket`.
		static std::string DefaultPath();

		/// Points the store at a path. The multiprocess reconnect test shares one Userdata, so every
		/// process gets its own store path rather than racing over one file.
		void SetPath(std::string path);
		const std::string& GetPath() const { return m_Path; }

		/// Writes the record durably, replacing whatever was there.
		/// @return Whether the record is on disk; a failure leaves any previous record untouched.
		bool Store(const NetH4TicketRecord& record, std::string* error = nullptr);

		/// Reads the record back.
		/// @return Why the load produced nothing, or Loaded.
		NetH4TicketLoadResult Load(uint64_t nowUnixMs, NetH4TicketRecord& out, std::string* error = nullptr);

		/// Deletes the record. Only a LeaveAck, a confirmed session end or the age bound may call this.
		bool Clear(std::string* error = nullptr);

		bool HasRecord() const;

		uint32_t GetStores() const { return m_Stores; }
		uint32_t GetStoreFailures() const { return m_StoreFailures; }
		uint32_t GetLoads() const { return m_Loads; }
		uint32_t GetRefusedLoads() const { return m_RefusedLoads; }
		uint32_t GetClears() const { return m_Clears; }

		/// The record's canonical bytes, the same ones the store writes and macs.
		static bool Serialize(const NetH4TicketRecord& record, std::vector<uint8_t>& out);
		static bool Deserialize(const std::vector<uint8_t>& bytes, NetH4TicketRecord& out);

	private:
		std::string m_Path = DefaultPath();
		uint32_t m_Stores = 0;
		uint32_t m_StoreFailures = 0;
		uint32_t m_Loads = 0;
		uint32_t m_RefusedLoads = 0;
		uint32_t m_Clears = 0;
	};

} // namespace RTE
