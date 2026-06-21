#pragma once

#include "NetMatchRunner.h"
#include "Singleton.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define g_NetMatchService NetMatchService::Instance()

namespace RTE {

	class GnsTransport;

	enum class NetMatchServiceState {
		Idle,
		Starting,
		ReadyToLaunch,
		Running,
		Failed,
	};

	struct NetMatchServiceRequest {
		bool host = false;
		std::string address = "127.0.0.1";
		uint16_t port = 41010;
		std::string playerName = "Player";
		std::string activityPreset = "Skirmish Defense";
		NetActorOwnershipPolicy ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
	};

	class NetMatchService : public Singleton<NetMatchService> {
	public:
		NetMatchService() = default;
		~NetMatchService();

		bool Start(const NetMatchServiceRequest& request, std::string* error = nullptr);
		void Destroy();
		void Update();
		void SetReady();
		void RequestStart();
		void ReportRuntimeError(const std::string& error);
		void Complete(const std::string& reason);

		bool ConsumeReadyToLaunch(std::string& outActivityPreset);
		NetMatchServiceState GetState() const;
		std::string GetStatusText() const;
		std::string GetErrorText() const;
		std::string BuildReportJson() const;
		uint8_t GetLocalPeerId() const;
		int GetLocalTeam() const;

		static const char* StateName(NetMatchServiceState state);

	private:
		void WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest);
		NetSessionConfig BuildSessionConfig(const NetIdentityManifest& manifest, const NetMatchServiceRequest& request) const;
		NetMatchConfig BuildMatchConfig(const NetMatchServiceRequest& request, uint64_t sessionId) const;
		void SetState(NetMatchServiceState state, std::string status, std::string error = "");
		void JoinWorkerIfDone();

		mutable std::mutex m_Mutex;
		NetMatchServiceState m_State = NetMatchServiceState::Idle;
		std::string m_StatusText = "Idle";
		std::string m_ErrorText;
		std::string m_ActivityPreset;
		std::thread m_Worker;
		bool m_WorkerDone = false;
		bool m_IsHost = false;
		uint8_t m_LocalPeerId = 0;
		int m_LocalTeam = -1;

		std::unique_ptr<GnsTransport> m_Transport;
		std::unique_ptr<NetSession> m_Session;
		std::unique_ptr<NetLockstepCoordinator> m_Coordinator;
		std::unique_ptr<NetMatchRunner> m_Runner;
		std::atomic<bool> m_ReadyRequested{false};
		std::atomic<bool> m_StartRequested{false};
		std::atomic<bool> m_CancelRequested{false};
	};

} // namespace RTE
