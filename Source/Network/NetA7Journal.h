#pragma once

#include "nlohmann/json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace RTE {

	class NetA7Journal {
	public:
		static bool StartE2E(std::string* error, std::function<bool()> cancelled);
		static bool Enabled();
		static void Emit(const char* event, nlohmann::json fields = nlohmann::json::object());
		static void Session(const char* event, uint64_t sessionMs, nlohmann::json fields = nlohmann::json::object(), const char* source = "NetSession::m_NowMs");
		static void Gap(const char* reason);
		static bool Seal(const std::string& reportPath, int exitCode);
		static std::string Hex(const uint8_t* bytes, size_t size);
		static std::string Sha256(const uint8_t* bytes, size_t size);
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
