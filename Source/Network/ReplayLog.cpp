#include "ReplayLog.h"

#include <cstring>
#include <fstream>

namespace RTE {

	ReplayLog::ReplayLog() = default;
	ReplayLog::~ReplayLog() = default;

	void ReplayLog::Destroy() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Recording = false;
		m_Scenario.clear();
		m_Seed = 0;
		m_Ticks.clear();
	}

	void ReplayLog::BeginRecording(const std::string& scenario, uint64_t seed) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Recording = true;
		m_Scenario = scenario;
		m_Seed = seed;
		m_Ticks.clear();
	}

	void ReplayLog::RecordTick(uint64_t tick, const std::vector<PlayerInput>& inputs) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Recording) {
			return;
		}
		TickRec t;
		t.tick = tick;
		t.inputs = inputs;
		m_Ticks.push_back(std::move(t));
	}

	void ReplayLog::EndRecording() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Recording = false;
	}

	static void writeLE(std::ofstream& f, uint64_t v) {
		uint8_t b[8];
		for (int i = 0; i < 8; ++i) {
			b[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
		}
		f.write(reinterpret_cast<const char*>(b), 8);
	}
	static void writeLE32(std::ofstream& f, uint32_t v) {
		uint8_t b[4];
		for (int i = 0; i < 4; ++i) {
			b[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
		}
		f.write(reinterpret_cast<const char*>(b), 4);
	}
	static void writeLE32s(std::ofstream& f, int32_t v) {
		writeLE32(f, static_cast<uint32_t>(v));
	}

	bool ReplayLog::Write(const std::string& path) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::ofstream out(path, std::ios::binary);
		if (!out.is_open()) {
			return false;
		}

		// Magic
		const char magic[4] = {'C', 'C', 'R', 'P'};
		out.write(magic, 4);
		// Version
		writeLE32(out, 1);
		// Seed
		writeLE(out, m_Seed);
		// Tick count
		writeLE(out, static_cast<uint64_t>(m_Ticks.size()));
		// Scenario (16 bytes, null-padded)
		char scenarioBuf[16] = {0};
		const size_t n = std::min<size_t>(m_Scenario.size(), 15);
		std::memcpy(scenarioBuf, m_Scenario.data(), n);
		out.write(scenarioBuf, 16);

		for (const auto& t: m_Ticks) {
			writeLE(out, t.tick);
			writeLE32(out, static_cast<uint32_t>(t.inputs.size()));
			for (const auto& p: t.inputs) {
				writeLE32s(out, p.player_id);
				writeLE32(out, static_cast<uint32_t>(p.state.size()));
				if (!p.state.empty()) {
					out.write(reinterpret_cast<const char*>(p.state.data()),
					          static_cast<std::streamsize>(p.state.size()));
				}
			}
		}

		return out.good();
	}

} // namespace RTE
