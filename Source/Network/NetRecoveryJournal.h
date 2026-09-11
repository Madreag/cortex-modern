#pragma once

#include "NetLockstep.h"
#include "nlohmann/json.hpp"

namespace RTE {

	struct NetResyncState;

	class NetRecoveryJournal {
	public:
		static nlohmann::json Context(const NetLockstepConfig& config, uint64_t round);
		static std::string Blob(const char* kind, const std::vector<uint8_t>& bytes);
		static std::string NextScope();
		static nlohmann::json Command(const NetGameCommand& command, uint64_t target);
		static nlohmann::json Input(const NetLockstepConfig& config, const NetLockstepFrame& input, const char* phase, const char* source, const std::string& scope = {}, size_t index = 0);
		static std::string State(const NetLockstepConfig& config, const NetResyncState& state, const char* phase, const char* source, uint64_t round, const std::vector<NetLockstepFrame>& extras = {}, const NetResyncState* captured = nullptr);
		static std::vector<NetLockstepFrame> SplitReady(const NetLockstepConfig& config, uint64_t round, const NetLockstepReadyFrame& ready);
	};

}
