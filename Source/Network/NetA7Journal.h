#pragma once

#include "nlohmann/json.hpp"
#include "NetProtocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace RTE {
	struct NetGameCommand;
	struct NetLockstepSeatSnapshot;
	struct NetModerationSelection;

	class NetA7Journal {
	public:
		static bool StartE2E(std::string* error, std::function<bool()> cancelled);
		static bool Enabled();
		static bool B2Enabled();
		static bool RecoveryInputJournalEnabled();
		static const std::string& B2Fault();
		static const std::string& B2Name();
		static bool B2ReplayRequested();
		static bool ReadB2ReplayGate(const nlohmann::json& expected, bool& released);
		static bool ValidateB2ReplayGate(const nlohmann::json& gate, const nlohmann::json& expected);
		static void Emit(const char* event, nlohmann::json fields = nlohmann::json::object());
		static void Session(const char* event, uint64_t sessionMs, nlohmann::json fields = nlohmann::json::object(), const char* source = "NetSession::m_NowMs");
		static void Gap(const char* reason);
		static bool Seal(const std::string& reportPath, int exitCode);
		static std::string Hex(const uint8_t* bytes, size_t size);
		static std::string Sha256(const uint8_t* bytes, size_t size);
		static std::string FileSha256(const std::string& path);
		static std::string PayloadSha256(const NetPayload& payload);
		static std::string CommandSha256(const NetGameCommand& command);
		static constexpr const char* c_CommandHashAlgorithm = "command_only_recovery_v1";
		static nlohmann::json B2Selection(const NetModerationSelection& selection);
		static nlohmann::json B2PayloadFields(const NetPayload& payload);
		static void B2SeatSnapshot(const NetLockstepSeatSnapshot& snapshot, const char* source, const char* phase);
		static bool HasConnectGate();
		static bool WaitForConnectGate(const std::string& loadedTicketSha, std::string* error);
		static bool ControlledSilentClient();
		static uint32_t SilentHeartbeatTag();
		static uint64_t SilentHeartbeatMs();
		static uint64_t SilentReceiveBudgetMs();
		static void SetSeatView(nlohmann::json seats, uint64_t sessionMs, const char* source);
		static void AppliedTick(uint64_t round, uint64_t frame, uint8_t peerId, const std::string& sharedHash);
		static uint64_t AppliedFrame();
		static uint64_t BeginResync();
		static uint64_t CurrentResync();
	};

} // namespace RTE
