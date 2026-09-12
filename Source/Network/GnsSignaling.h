#pragma once

#ifdef CCCP_WITH_GNS

#include "GnsTransport.h"
#include "NetDirectorySignalChannel.h"

#include <steam/steamnetworkingcustomsignaling.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RTE {

	class GnsDirectorySignalDispatcher;

	enum class GnsSignalFrame : uint8_t {
		Rendezvous = 0x01,
		Refusal = 0x02, //!< Ours: GNS's own rejection blob lacks from_identity, so a joiner drops it.
	};

	// Shared, so the counts outlive the dispatcher and every signaling object it made.
	struct GnsSignalingTally {
		std::atomic<int> created{0};
		std::atomic<int> released{0};
		std::atomic<int> deleted{0};
		std::atomic<int> dropped{0}; //!< Frames that never reached the channel.
	};

	class GnsDirectorySignaling final : public ISteamNetworkingConnectionSignaling {
	public:
		struct PumpResult {
			uint64_t posted = 0;
			uint64_t duplicates = 0;
			uint64_t refused = 0;
			bool done = false;
		};
		using Observer = std::function<void(const std::string& frame, bool duplicate, bool posted)>;

		GnsDirectorySignaling(std::string peer, int copies, std::shared_ptr<GnsSignalingTally> tally);

		// Any GNS thread: only the mutex-guarded outbox is touched, never the channel.
		bool SendSignal(HSteamNetConnection connection, const SteamNetConnectionInfo_t& info, const void* blob, int size) override;
		void Release() override;

		PumpResult Pump(NetDirectorySignalChannel& channel, const Observer& observe = {});
		void Detach();

		const std::string& Peer() const { return m_Peer; }

	private:
		struct Frame {
			std::string bytes;
			bool duplicate = false;
		};

		~GnsDirectorySignaling();
		void DropHolder();

		const std::string m_Peer;
		const int m_Copies; //!< Test flag: each blob queued this many times, as an at-least-once relay may deliver it.
		const std::shared_ptr<GnsSignalingTally> m_Tally;
		std::mutex m_Mutex;
		std::vector<Frame> m_Outbox;
		bool m_Released = false;
		bool m_Detached = false;
		std::atomic<int> m_Holders{2}; //!< GNS until Release, the dispatcher until the outbox is handed over.
	};

	// GNS keeps no context past ReceivedP2PCustomSignal, so each delivery makes one naming its sender.
	class GnsDirectoryRecvContext final : public ISteamNetworkingSignalingRecvContext {
	public:
		GnsDirectoryRecvContext(GnsDirectorySignalDispatcher& dispatcher, std::string peer) : m_Dispatcher(dispatcher), m_Peer(std::move(peer)) {}

		ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection connection, const SteamNetworkingIdentity& identityPeer, int localVirtualPort) override;
		void SendRejectionSignal(const SteamNetworkingIdentity& identityPeer, const void* blob, int size) override;

	private:
		GnsDirectorySignalDispatcher& m_Dispatcher;
		const std::string m_Peer;
		std::string m_Refusal;
	};

	class GnsDirectorySignalDispatcher {
	public:
		enum class Role : uint8_t { Host, Joiner };
		struct Config {
			Role role = Role::Joiner;
			std::string baseUrl;
			std::string installKey;
			std::string certPinSha256;
			std::string sessionId;
			std::string sessionToken; //!< Host only.
		};
		struct Counters {
			uint64_t signalsOut = 0;
			uint64_t signalsIn = 0;
			uint64_t framesIgnored = 0;
			uint64_t refusals = 0; //!< Host: posted. Joiner: received.
			uint64_t connectRequests = 0;
			uint64_t duplicatesOut = 0;
			uint64_t duplicatesIn = 0; //!< Byte-identical to the previous frame from the same peer.
			uint64_t gnsRefused = 0;
			uint64_t postsRefused = 0;
			uint64_t unpostedAtStop = 0;
		};
		struct PollWindow {
			uint64_t armedMs = 0;
			uint64_t disarmedMs = 0;
			uint64_t polls = 0;
			uint64_t signalsPosted = 0;
		};
		/// Host: an empty string accepts the request, anything else refuses it with that reason.
		using Admission = std::function<std::string(const std::string& peer, const std::string& identity)>;
		using Trace = std::function<void(const std::string& line)>;

		static constexpr size_t c_MaxRefusalBytes = 127; //!< What GNS keeps of an end reason.

		GnsDirectorySignalDispatcher();
		GnsDirectorySignalDispatcher(const GnsDirectorySignalDispatcher&) = delete;
		GnsDirectorySignalDispatcher& operator=(const GnsDirectorySignalDispatcher&) = delete;
		~GnsDirectorySignalDispatcher();

		bool Start(GnsTransport& transport, const Config& config);
		void SetAdmission(Admission admission) { m_Admission = std::move(admission); }
		void SetTrace(Trace trace) { m_Trace = std::move(trace); }
		void SetCopiesForTest(int copies) { m_Copies = copies; }

		GnsDirectorySignaling* CreateJoinSignaling();
		// Every poll spends the service's per-key request budget, so polling is armed only during a rendezvous.
		void SetPolling(bool armed, uint64_t nowMs);
		void Update(uint64_t nowMs);
		void Stop();

		static std::string HostIdentity(const std::string& sessionId);
		static std::string JoinerIdentity(const std::string& joinNonce);
		std::string LocalIdentity() const;

		const NetDirectorySignalChannel& Channel() const { return m_Channel; }
		const Counters& GetCounters() const { return m_Counters; }
		const std::vector<PollWindow>& PollWindows() const { return m_PollWindows; }
		std::shared_ptr<const GnsSignalingTally> Tally() const { return m_Tally; }
		std::string BuildReportJson() const;

	private:
		friend class GnsDirectoryRecvContext;

		bool Deliver(const NetDirectorySignalChannel::Signal& signal);
		GnsDirectorySignaling* Adopt(const std::string& peer);
		void PostRefusal(const std::string& peer, const std::string& reason);
		void PumpOutboxes();
		void Note(const std::string& line) const;

		NetDirectorySignalChannel m_Channel;
		GnsTransport* m_Transport = nullptr;
		Role m_Role = Role::Joiner;
		std::string m_SessionId;
		Admission m_Admission;
		Trace m_Trace;
		int m_Copies = 1;
		bool m_Stopping = false;
		std::vector<GnsDirectorySignaling*> m_Signalings;
		std::shared_ptr<GnsSignalingTally> m_Tally;
		std::map<std::string, std::string> m_LastFrameFrom;
		Counters m_Counters;
		bool m_PollArmed = false;
		uint64_t m_PollsAtArm = 0;
		uint64_t m_PostsAtArm = 0;
		std::vector<PollWindow> m_PollWindows;
	};

} // namespace RTE

#endif
