#pragma once

#include "NetMatchReplay.h"
#include <chrono>
#include <thread>

namespace RTE {
	inline bool WaitForReplayCloseForTest(const NetMatchReplayWriter& writer, std::string* error) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!writer.IsCloseComplete() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (writer.IsCloseComplete()) return true;
		if (error) *error = "the replay storage worker did not finish after its fixture released it";
		return false;
	}
}
