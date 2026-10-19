#pragma once

#include "NetIceServers.h"
#include "NetParticipantCrypto.h"
#include "NetDirectoryCodec.h"
#include "NetMatchConfig.h"
#include "NetHttpClient.h"
#include "NetDirectoryRetry.h"
#include <deque>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace RTE {

	class NetHttpClient;
	class NetLanDiscovery;

	/// The same signed, address-free lease for both authorities. The existing H4
	/// transaction still commits the roster; this is its expiring seat credential.
	struct NetSeatLease {
		static constexpr uint16_t c_Version = 2;
		static constexpr uint64_t c_LifetimeSeconds = 300;
		static constexpr size_t c_MaxTokenBytes = 512;
		static constexpr size_t c_MaxStableSeat = NetMatchConfigUtil::c_MaxPlayers + NetMatchConfigUtil::c_MaxWorldSpectators;
		std::string token;
		NetParticipantId authority{};
		std::string directorySessionId;
		NetAuthBytes16 epoch{};
		uint64_t hostSessionId = 0;
		uint16_t seat = 0;
		uint32_t generation = 0;
		NetParticipantId participant{};
		uint64_t issuedAt = 0;
		uint64_t expiresAt = 0;
		NetAuthBytes32 credential{};

		bool Empty() const { return token.empty(); }
		bool Usable(uint64_t unixSeconds) const { return !Empty() && issuedAt <= unixSeconds + 60 && expiresAt > unixSeconds; }
		bool SameSeat(const NetSeatLease& other) const;
		/// Verifies the signature and bounded format, but leaves expiry to admission
		/// so its own key can renew an expired lease after a long outage.
		static bool Decode(const std::string& token, const NetParticipantId& authority, NetSeatLease& out);
		static bool Sign(const NetAuthBytes32& matchSigningKey, NetSeatLease& lease);
	};

	struct NetConnectionRoute {
		std::string iceIdentity;
		uint16_t iceVirtualPort = 0;
		uint16_t listenPort = 0;
		std::vector<std::string> listenAddrs;
		uint32_t generation = 0;
		std::string state = "connecting";
		bool operator==(const NetConnectionRoute&) const = default;
	};

	struct NetConnectionHost {
		NetDirectorySessionRow row;
		uint32_t generation = 0;
		NetParticipantId authority{};
		bool relayEnabled = false;
		uint64_t directoryReadyMs = 0;
		bool operator==(const NetConnectionHost&) const = default;
	};

	// These are authenticated directory facts, not a second copy of seat phase.
	struct NetHostChangeRequest {
		uint64_t generation = 0, roundId = 0, appliedFrame = 0, preparedFrame = 0;
		NetHash32 configHash{};
		// A completed unanimous fallback is reported by every signer when HTTP returns.
		uint16_t agreedHost = UINT16_MAX;
		uint64_t agreedBoundary = 0;
		std::vector<uint16_t> agreedMembers;
		bool operator==(const NetHostChangeRequest&) const = default;
	};
	struct NetHostChangeReply {
		enum class State { Unavailable, Waiting, Decided };
		State state = State::Unavailable;
		uint64_t generation = 0, boundary = 0;
		uint16_t host = UINT16_MAX, donor = UINT16_MAX;
		std::vector<uint16_t> members;
	};

	struct NetFrameTieRequest {
		uint64_t generation = 0, roundId = 0, frame = 0;
		NetHash32 configHash{};
		uint16_t host = UINT16_MAX;
		std::vector<uint16_t> owners, members;
		bool queryOnly = false;
		bool operator==(const NetFrameTieRequest&) const = default;
	};
	struct NetFrameTieReply {
		enum class State { Unavailable, Waiting, Decided };
		State state = State::Unavailable;
		std::vector<uint16_t> members;
	};
	constexpr uint64_t c_NetFrameTieDeadlineMs = 1000;
	constexpr uint64_t c_NetFrameTieHttpTimeoutMs = 250;

	/// One connection control plane, shared by admission and recovery. Every HTTP
	/// operation is asynchronous. A direct match never starts a directory request.
	/// It owns route/check-in state only; NetSeatRoster remains the sole seat phase.
	class NetConnectionAuthority {
	public:
		enum class Result { Pending, Ready, Refused };
		NetConnectionAuthority();
		~NetConnectionAuthority();
		NetConnectionAuthority(const NetConnectionAuthority&) = delete;
		NetConnectionAuthority& operator=(const NetConnectionAuthority&) = delete;

		void Configure(NetParticipantIdentityStore* player, std::string baseUrl, std::string installKey, std::string certPin);
		void Reset();
		void Suspend();
		void SetHostSigningKey(const NetAuthBytes32& key);
		void SetDirectory(std::string sessionId, std::string hostToken = {}, uint32_t hostGeneration = 0, std::string authorityKey = {});
		std::string DirectorySessionId() const;
		NetParticipantId LocalParticipant() const;
		/// A host reserves through H4, then waits for this issuer before offering the
		/// ticket. The directory signs exactly that match, seat, holder and key.
		Result Issue(NetSeatLease requested, const std::string& name, NetSeatLease& issued, std::string& error);
		void Remove(const NetParticipantId& player, const std::string& name, bool ban);
		bool AdoptLocalLease(const NetSeatLease& lease);
		/// Restores a disk record for proof at the authority; it is not admission.
		bool RestoreLease(const NetSeatLease& lease);
		std::optional<NetSeatLease> LocalLease() const;
		void SetRoute(NetConnectionRoute route);
		/// The route the next check-in carries.
		NetConnectionRoute Route() const;
		void CheckInNow();
		void RefreshRoute();
		void RequestBootstrap();
		std::optional<NetConnectionHost> Host() const;
		NetRelayConfig Relay() const;
		std::string Error() const;
		bool Refused() const;
		bool Ended() const;
		uint64_t CheckIns() const;
		bool RouteCheckedIn() const;
		bool RelayRefreshed() const;
		uint32_t NetworkRevision() const;
		std::map<uint16_t, NetConnectionRoute> PeerRoutes() const;
		std::vector<std::string> LocalAddresses() const;
		std::optional<NetConnectionRoute> DirectHost(uint64_t matchId) const;
		NetHostChangeReply QueryHostChange(const NetHostChangeRequest& request);
		std::string HostChangeToken(uint64_t generation) const;
		NetFrameTieReply QueryFrameTie(const NetFrameTieRequest& request);
		/// The match the directory listed for this session; a tie is filed under it, which the directory checks the round against.
		void SetListedMatchHash(const std::string& hex);
		void Update(uint64_t steadyMs, uint64_t unixSeconds);
		uint64_t AdmissionWaitedMs() const;
		uint64_t AdmissionRetryMs() const;
		/// This machine's clock moved onto the directory's, as its last reply measured it; the seat and relay times it issues are on that clock.
		uint64_t DirectoryNow(uint64_t localUnixSeconds) const;
		/// Test lever: replaces the machine's adapter enumeration with this source.
		void SetAddressSourceForTest(std::function<std::vector<std::string>()> source) { m_AddressSource = std::move(source); }

	private:
		friend bool TestRecoveryUsesListedMatch(std::string* error);
		friend struct NetConnectionSelfTestAccess;
		NetHash32 DirectoryMatchHash(const NetHash32& peerHash) const;
		struct Operation {
			std::string body;
			NetSeatLease wanted;
			std::optional<NetSeatLease> issued;
			std::string error;
			std::unique_ptr<NetHttpClient> request;
			uint64_t retryAt = 0;
			bool removal = false;
		};
		std::unique_ptr<NetHttpClient> StartRequest(const std::string& method, const std::string& body, int timeoutMs = NetHttpClient::c_TotalTimeoutMs);
		bool ReadLeaseReply(const std::string& body, NetSeatLease& lease, std::string& error);
		bool ReadHostReply(const std::string& body, NetConnectionHost& host, std::string& error);
		void PollOperations(uint64_t steadyMs);
		void PollCheckIn(uint64_t steadyMs, uint64_t unixSeconds);
		void PollBootstrap(uint64_t steadyMs);
		void PollHostChange(uint64_t steadyMs, uint64_t unixSeconds);
		void PollFrameTie(uint64_t steadyMs, uint64_t unixSeconds);
		std::string SignedSeatRequest(const std::string& operation, uint64_t unixSeconds, const NetHostChangeRequest* change = nullptr, const NetFrameTieRequest* tie = nullptr);

		mutable std::mutex m_Mutex;
		NetParticipantIdentityStore* m_Player = nullptr;
		std::string m_BaseUrl, m_InstallKey, m_CertPin, m_SessionId, m_HostToken;
		uint32_t m_HostGeneration = 0;
		NetParticipantId m_DirectoryKey{};
		NetAuthBytes32 m_HostSigningKey{};
		NetAuthBytes16 m_Instance{};
		std::map<std::string, Operation> m_Operations;
		std::map<std::string, bool> m_Removals; //!< Each removal sent or queued, and whether it bans.
		std::optional<NetSeatLease> m_LocalLease;
		std::optional<NetConnectionHost> m_Host;
		NetConnectionRoute m_Route;
		std::map<uint16_t, NetConnectionRoute> m_PeerRoutes;
		std::unique_ptr<NetLanDiscovery> m_LanBrowser;
		std::map<uint64_t, NetConnectionRoute> m_DirectHosts;
		std::vector<std::string> m_NetworkAddresses;
		std::function<std::vector<std::string>()> m_AddressSource; //!< The adapter enumeration; empty reads the machine's own.
		NetRelayConfig m_Relay;
		std::unique_ptr<NetHttpClient> m_CheckIn, m_Bootstrap, m_HostChange;
		std::optional<NetHostChangeRequest> m_HostChangeRequest;
		NetHostChangeReply m_HostChangeReply;
		std::string m_HostChangeToken;
		std::unique_ptr<NetHttpClient> m_FrameTie;
		std::vector<std::unique_ptr<NetHttpClient>> m_RetiredFrameTies;
		std::optional<NetFrameTieRequest> m_FrameTieRequest;
		std::optional<NetHash32> m_ListedMatchHash;
		NetFrameTieReply m_FrameTieReply;
		bool m_FrameTieCheckedIn = false;
		std::optional<uint64_t> m_FrameTieBoundStartMs;
		bool m_FrameTieFinalQuerySent = false;
		uint64_t m_NextFrameTie = 0;
		uint64_t m_FrameTieSentMs = 0; //!< When the request in flight left.
		uint64_t m_FrameTieExchangeMs = 0; //!< The longest recent tie exchange, decaying; sizes the next one's timeout.
		bool m_FrameTieSentCheckIn = false; //!< The request in flight is a check-in, not a query.
		std::deque<uint64_t> m_TieRequests; //!< Tie requests over the last minute: their own allowance, outside the routine check-ins'.
		uint64_t m_NextHostChange = 0;
		uint64_t m_NextCheckIn = 0, m_NextBootstrap = 0, m_CheckIns = 0;
		uint64_t m_NextNetworkProbe = 0, m_LastWallSeconds = 0;
		NetDirectoryRetry m_IssueRetry, m_RemoveRetry, m_CheckInRetry, m_BootstrapRetry, m_HostChangeRetry, m_FrameTieRetry;
		uint64_t m_NextSeatRequestMs = 0, m_LastUpdateMs = 0;
		std::deque<uint64_t> m_SeatRequests;
		uint64_t FailedReply(const NetHttpClient::Response& response, NetDirectoryRetry& retry, uint64_t nowMs, const char* path);
		bool CanSeatRequest(uint64_t nowMs, bool decisionRequest = false);
		bool CanTieRequest(uint64_t nowMs);
		bool CanRequest(uint64_t nowMs) const;
		void NoteDirectoryClock(const std::string& body);
		uint64_t DirectoryNowLocked(uint64_t localUnixSeconds) const;
		void ProbeLanHosts(uint64_t steadyMs, std::map<uint64_t, NetConnectionRoute>& hosts);
		struct NetworkProbe {
			std::vector<std::string> addresses;
			uint32_t nativeRevision = 0;
			bool resumed = false;
			uint64_t sequence = 0;
		};
		std::optional<NetworkProbe> ProbeNetwork(uint64_t steadyMs, uint64_t unixSeconds);
		void ApplyNetwork(NetworkProbe probe);
		int64_t m_DirectoryClockOffset = 0; //!< The directory's clock minus this machine's, in seconds.
		uint64_t m_UnixSeconds = 0; //!< This machine's clock at the last Update.
		std::mutex m_NetworkMutex; //!< The LAN browser and the adapter scan, kept apart from m_Mutex so no query waits on them.
		uint64_t m_ProbeSequence = 0, m_LanSequence = 0, m_AppliedProbe = 0, m_AppliedLan = 0;
		uint32_t m_NetworkRevision = 0;
		uint32_t m_NativeRouteRevision = 0;
		uint32_t m_CheckInGeneration = 0, m_CheckedGeneration = 0, m_RelayGeneration = 0;
		bool m_BootstrapWanted = false, m_Refused = false, m_Ended = false, m_Suspended = false;
		std::string m_Error;
	};

	/// The connection plane's rows, run by the net-directory self-test; CCCP_TEST_CONNECTION_CASE picks one.
	namespace NetConnectionSelfTest { bool Run(std::string* error); }

} // namespace RTE
