#pragma once

#include "ControllerFrame.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace RTE {

	struct ControllerLogMetadata {
		std::string scenario;
		std::string commit;
		uint64_t seed = 0;
		uint64_t maxTicks = 0;
		int numLuaStates = -1;
		std::map<std::string, std::string> simConfig;
	};

	struct ControllerLogTick {
		uint64_t tick = 0;
		std::vector<ControllerFrame> frames;
	};

	class ControllerLog {
	public:
		ControllerLogMetadata metadata;
		std::vector<ControllerLogTick> ticks;

		void Clear();
		void AddTick(uint64_t tick, std::vector<ControllerFrame> frames);
		const ControllerLogTick* FindTick(uint64_t tick) const;

		bool Write(const std::string& path, std::string* error = nullptr) const;
		bool Read(const std::string& path, std::string* error = nullptr);
	};

} // namespace RTE
