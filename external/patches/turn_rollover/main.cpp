#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static Clock::time_point started;
static HSteamNetConnection host = 0, joiner = 0;
static int connects = 0, drops = 0;
static std::atomic<int> replacements{0}, refreshes{0};
static std::mutex signalsMutex;
static std::deque<std::vector<char>> toHost, toJoiner;

static double Seconds() { return std::chrono::duration<double>(Clock::now() - started).count(); }
static void Debug(ESteamNetworkingSocketsDebugOutputType, const char* line) {
	// Keep protocol receipts without copying library text that might contain a login.
	const std::string text(line);
	if (text.find("TURN replacement route selected") != std::string::npos) ++replacements;
	if (text.find("refreshed for") != std::string::npos) ++refreshes;
}
static void Status(SteamNetConnectionStatusChangedCallback_t* event) {
	if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && event->m_info.m_hListenSocket) {
		host = event->m_hConn;
		SteamNetworkingSockets()->AcceptConnection(host);
	}
	if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
		++connects;
		std::printf("[turn-check] connected connection=%u t=%.3f\n", event->m_hConn, Seconds());
	}
	if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer || event->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
		++drops;
		std::printf("[turn-check] dropped connection=%u reason=%d t=%.3f\n", event->m_hConn, event->m_info.m_eEndReason, Seconds());
	}
}
class Signal final : public ISteamNetworkingConnectionSignaling {
	bool towardsHost;
public:
	explicit Signal(bool direction) : towardsHost(direction) {}
	bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* data, int size) override {
		std::lock_guard<std::mutex> lock(signalsMutex);
		auto& queue = towardsHost ? toHost : toJoiner;
		queue.emplace_back(static_cast<const char*>(data), static_cast<const char*>(data) + size);
		return true;
	}
	void Release() override { delete this; }
};
class Context final : public ISteamNetworkingSignalingRecvContext {
	bool receiverHost;
public:
	explicit Context(bool value) : receiverHost(value) {}
	ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection, const SteamNetworkingIdentity&, int) override { return receiverHost ? new Signal(false) : nullptr; }
	void SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) override {}
};
static void Deliver(std::deque<std::vector<char>>& queue, Context& context) {
	std::deque<std::vector<char>> pending;
	{ std::lock_guard<std::mutex> lock(signalsMutex); pending.swap(queue); }
	for (const auto& message : pending) SteamNetworkingSockets()->ReceivedP2PCustomSignal(message.data(), int(message.size()), &context);
}
static bool Login(const char* file, std::string& user, std::string& password) {
	std::ifstream stream(file);
	return bool(std::getline(stream, user) && std::getline(stream, password)) && !user.empty() && !password.empty();
}
static bool Update(HSteamNetConnection connection, const char* server, const std::string& user, const std::string& password) {
#if STEAMNETWORKINGSOCKETS_TURN_LIFETIME >= 3
	return SteamNetworkingSockets_SetTURNConfig(connection, server, user.c_str(), password.c_str());
#else
	auto* utils = SteamNetworkingUtils();
	return utils->SetConnectionConfigValueString(connection, k_ESteamNetworkingConfig_P2P_TURN_ServerList, server)
		&& utils->SetConnectionConfigValueString(connection, k_ESteamNetworkingConfig_P2P_TURN_UserList, user.c_str())
		&& utils->SetConnectionConfigValueString(connection, k_ESteamNetworkingConfig_P2P_TURN_PassList, password.c_str());
#endif
}
int main(int argc, char** argv) {
	if (argc != 4) return 64;
	std::string user, password, nextUser, nextPassword;
	if (!Login(argv[2], user, password) || !Login(argv[3], nextUser, nextPassword)) return 65;
	started = Clock::now();
	SteamNetworkingErrMsg error;
	if (!GameNetworkingSockets_Init(nullptr, error)) return 66;
	auto* utils = SteamNetworkingUtils();
	utils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Debug, Debug);
	utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, 5);
	utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
	SteamNetworkingConfigValue_t values[8];
	values[0].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay);
	values[1].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation, 1);
	values[2].SetString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, argv[1]);
	values[3].SetString(k_ESteamNetworkingConfig_P2P_TURN_UserList, user.c_str());
	values[4].SetString(k_ESteamNetworkingConfig_P2P_TURN_PassList, password.c_str());
	values[5].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(Status));
	values[6].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 5000);
	values[7].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 15000);
	auto* sockets = SteamNetworkingSockets();
	auto listener = sockets->CreateListenSocketP2P(1, 8, values);
	SteamNetworkingIdentity identity;
	sockets->GetIdentity(&identity);
	std::vector<SteamNetworkingConfigValue_t> client(values, values + 8);
	SteamNetworkingConfigValue_t localPort;
	localPort.SetInt32(k_ESteamNetworkingConfig_LocalVirtualPort, 2);
	client.push_back(localPort);
	joiner = sockets->ConnectP2PCustomSignaling(new Signal(true), &identity, 1, int(client.size()), client.data());
	Context hostContext(true), joinerContext(false);
	bool rotated = false;
	double nextSend = 0, lastHost = 0, lastJoiner = 0;
	int receivedHost = 0, receivedJoiner = 0;
	bool gap = false, relay = true;
	while (Seconds() < 85 && !drops && !gap) {
		Deliver(toHost, hostContext); Deliver(toJoiner, joinerContext); sockets->RunCallbacks();
		const double now = Seconds();
		if (connects == 2) {
			if (!lastHost) lastHost = lastJoiner = now;
			for (auto connection : {host, joiner}) {
				SteamNetConnectionInfo_t info;
				if (!sockets->GetConnectionInfo(connection, &info) || !(info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed)) relay = false;
				SteamNetworkingMessage_t* message = nullptr;
				while (sockets->ReceiveMessagesOnConnection(connection, &message, 1) > 0) {
					if (connection == host) { lastHost = now; ++receivedHost; } else { lastJoiner = now; ++receivedJoiner; }
					message->Release();
				}
			}
			if (now >= nextSend) {
				nextSend = now + .25;
				uint32_t payload = uint32_t(receivedHost + receivedJoiner);
				for (auto connection : {host, joiner}) sockets->SendMessageToConnection(connection, &payload, sizeof(payload), k_nSteamNetworkingSend_Reliable, nullptr);
			}
			if (!rotated && now >= 12) {
				rotated = Update(host, argv[1], nextUser, nextPassword) && Update(joiner, argv[1], nextUser, nextPassword);
				std::printf("[turn-check] login queued same_connections=%u,%u t=%.3f\n", host, joiner, now);
			}
			gap = now - lastHost > 5 || now - lastJoiner > 5;
		} else if (now > 20) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	bool passed = connects == 2 && drops == 0 && !gap && relay && rotated && replacements >= 2 && receivedHost >= 250 && receivedJoiner >= 250;
	std::printf("[turn-check] %s elapsed_s=%.3f connects=%d drops=%d gap=%d relay=%d replacements=%d refreshes=%d messages=%d,%d\n", passed ? "PASS" : "FAIL", Seconds(), connects, drops, gap, relay, replacements.load(), refreshes.load(), receivedHost, receivedJoiner);
	utils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
	GameNetworkingSockets_Kill();
	return passed ? 0 : 1;
}
