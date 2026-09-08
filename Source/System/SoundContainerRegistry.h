#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace RTE {
	class SoundContainer;
	using CheckpointSoundRegistry = std::map<uint64_t, std::vector<SoundContainer*>>;
}
