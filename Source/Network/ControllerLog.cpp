#include "ControllerLog.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <fstream>
#include <limits>

namespace RTE {

	namespace {
		using nlohmann::json;

		constexpr char c_Magic[] = {'C', 'C', 'C', 'F', 'L', 'O', 'G', '1'};
		constexpr uint16_t c_LogVersion = 1;

		void SetError(std::string* error, const std::string& message) {
			if (error) {
				*error = message;
			}
		}

		void WriteU16LE(std::ostream& out, uint16_t value) {
			char bytes[2] = {
			    static_cast<char>(value & 0xFFU),
			    static_cast<char>((value >> 8) & 0xFFU),
			};
			out.write(bytes, sizeof(bytes));
		}

		void WriteU32LE(std::ostream& out, uint32_t value) {
			char bytes[4] = {
			    static_cast<char>(value & 0xFFU),
			    static_cast<char>((value >> 8) & 0xFFU),
			    static_cast<char>((value >> 16) & 0xFFU),
			    static_cast<char>((value >> 24) & 0xFFU),
			};
			out.write(bytes, sizeof(bytes));
		}

		void WriteU64LE(std::ostream& out, uint64_t value) {
			char bytes[8];
			for (int i = 0; i < 8; ++i) {
				bytes[i] = static_cast<char>((value >> (i * 8)) & 0xFFU);
			}
			out.write(bytes, sizeof(bytes));
		}

		bool ReadExact(std::istream& in, char* data, size_t size) {
			in.read(data, static_cast<std::streamsize>(size));
			return static_cast<size_t>(in.gcount()) == size;
		}

		bool ReadU16LE(std::istream& in, uint16_t& value) {
			char bytes[2];
			if (!ReadExact(in, bytes, sizeof(bytes))) return false;
			value = static_cast<uint16_t>(static_cast<uint8_t>(bytes[0])) |
			        static_cast<uint16_t>(static_cast<uint16_t>(static_cast<uint8_t>(bytes[1])) << 8);
			return true;
		}

		bool ReadU32LE(std::istream& in, uint32_t& value) {
			char bytes[4];
			if (!ReadExact(in, bytes, sizeof(bytes))) return false;
			value = static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) |
			        (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 8) |
			        (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 16) |
			        (static_cast<uint32_t>(static_cast<uint8_t>(bytes[3])) << 24);
			return true;
		}

		bool ReadU64LE(std::istream& in, uint64_t& value) {
			char bytes[8];
			if (!ReadExact(in, bytes, sizeof(bytes))) return false;
			value = 0;
			for (int i = 0; i < 8; ++i) {
				value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i])) << (i * 8);
			}
			return true;
		}

		json MetadataToJson(const ControllerLogMetadata& metadata) {
			json j;
			j["scenario"] = metadata.scenario;
			j["commit"] = metadata.commit;
			j["seed"] = metadata.seed;
			j["max_ticks"] = metadata.maxTicks;
			j["num_lua_states"] = metadata.numLuaStates;
			j["control_state_count"] = ControlState::CONTROLSTATECOUNT;
			j["controller_frame_version"] = ControllerFrame::c_Version;
			j["controller_frame_size"] = ControllerFrame::c_EncodedSize;
			j["analog_scale"] = ControllerFrame::c_AnalogScale;
			json cfg = json::object();
			for (const auto& [key, value]: metadata.simConfig) {
				cfg[key] = value;
			}
			j["sim_config"] = cfg;
			return j;
		}

		ControllerLogMetadata MetadataFromJson(const json& j) {
			ControllerLogMetadata metadata;
			metadata.scenario = j.value("scenario", std::string());
			metadata.commit = j.value("commit", std::string());
			metadata.seed = j.value("seed", uint64_t{0});
			metadata.maxTicks = j.value("max_ticks", uint64_t{0});
			metadata.numLuaStates = j.value("num_lua_states", -1);
			if (j.contains("sim_config") && j["sim_config"].is_object()) {
				for (auto it = j["sim_config"].begin(); it != j["sim_config"].end(); ++it) {
					if (it.value().is_string()) {
						metadata.simConfig[it.key()] = it.value().get<std::string>();
					}
				}
			}
			return metadata;
		}
	}

	void ControllerLog::Clear() {
		metadata = {};
		ticks.clear();
	}

	void ControllerLog::AddTick(uint64_t tick, std::vector<ControllerFrame> frames) {
		std::sort(frames.begin(), frames.end(), [](const ControllerFrame& lhs, const ControllerFrame& rhs) {
			return lhs.actorUniqueID < rhs.actorUniqueID;
		});
		ticks.push_back(ControllerLogTick{tick, std::move(frames)});
	}

	const ControllerLogTick* ControllerLog::FindTick(uint64_t tick) const {
		auto it = std::lower_bound(ticks.begin(), ticks.end(), tick, [](const ControllerLogTick& rec, uint64_t value) {
			return rec.tick < value;
		});
		if (it == ticks.end() || it->tick != tick) {
			return nullptr;
		}
		return &*it;
	}

	bool ControllerLog::Write(const std::string& path, std::string* error) const {
		std::ofstream out(path, std::ios::binary);
		if (!out.is_open()) {
			SetError(error, "failed to open ControllerLog for writing: " + path);
			return false;
		}

		out.write(c_Magic, sizeof(c_Magic));
		WriteU16LE(out, c_LogVersion);
		const std::string metadataJson = MetadataToJson(metadata).dump();
		if (metadataJson.size() > std::numeric_limits<uint32_t>::max()) {
			SetError(error, "ControllerLog metadata is too large.");
			return false;
		}
		WriteU32LE(out, static_cast<uint32_t>(metadataJson.size()));
		out.write(metadataJson.data(), static_cast<std::streamsize>(metadataJson.size()));

		for (const ControllerLogTick& tick: ticks) {
			if (tick.frames.size() > std::numeric_limits<uint32_t>::max()) {
				SetError(error, "ControllerLog tick has too many frames.");
				return false;
			}
			std::vector<uint8_t> payload;
			payload.reserve(tick.frames.size() * ControllerFrame::c_EncodedSize);
			for (const ControllerFrame& frame: tick.frames) {
				std::vector<uint8_t> encoded = ControllerFrameCodec::Encode(frame);
				payload.insert(payload.end(), encoded.begin(), encoded.end());
			}
			WriteU64LE(out, tick.tick);
			WriteU32LE(out, static_cast<uint32_t>(tick.frames.size()));
			WriteU32LE(out, ControllerFrameCodec::PayloadChecksum(payload));
			out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
		}

		if (!out.good()) {
			SetError(error, "failed while writing ControllerLog: " + path);
			return false;
		}
		return true;
	}

	bool ControllerLog::Read(const std::string& path, std::string* error) {
		Clear();
		std::ifstream in(path, std::ios::binary);
		if (!in.is_open()) {
			SetError(error, "failed to open ControllerLog for reading: " + path);
			return false;
		}

		char magic[sizeof(c_Magic)];
		if (!ReadExact(in, magic, sizeof(magic)) || !std::equal(std::begin(magic), std::end(magic), std::begin(c_Magic))) {
			SetError(error, "ControllerLog magic mismatch.");
			return false;
		}

		uint16_t version = 0;
		if (!ReadU16LE(in, version) || version != c_LogVersion) {
			SetError(error, "ControllerLog version mismatch.");
			return false;
		}

		uint32_t metadataSize = 0;
		if (!ReadU32LE(in, metadataSize)) {
			SetError(error, "ControllerLog missing metadata length.");
			return false;
		}
		std::string metadataJson(metadataSize, '\0');
		if (!ReadExact(in, metadataJson.data(), metadataJson.size())) {
			SetError(error, "ControllerLog metadata is truncated.");
			return false;
		}
		try {
			const json j = json::parse(metadataJson);
			if (j.value("controller_frame_version", 0) != ControllerFrame::c_Version ||
			    j.value("controller_frame_size", 0U) != ControllerFrame::c_EncodedSize ||
			    j.value("control_state_count", 0) != ControlState::CONTROLSTATECOUNT) {
				SetError(error, "ControllerLog metadata is incompatible with this build.");
				return false;
			}
			metadata = MetadataFromJson(j);
		} catch (const std::exception& e) {
			SetError(error, std::string("ControllerLog metadata parse failed: ") + e.what());
			return false;
		}

		bool hasPreviousTick = false;
		uint64_t previousTick = 0;
		while (in.peek() != std::char_traits<char>::eof()) {
			uint64_t tick = 0;
			uint32_t frameCount = 0;
			uint32_t checksum = 0;
			if (!ReadU64LE(in, tick) || !ReadU32LE(in, frameCount) || !ReadU32LE(in, checksum)) {
				SetError(error, "ControllerLog tick header is truncated.");
				return false;
			}
			if (hasPreviousTick && tick <= previousTick) {
				SetError(error, "ControllerLog ticks are not strictly sorted.");
				return false;
			}
			if (frameCount > std::numeric_limits<size_t>::max() / ControllerFrame::c_EncodedSize) {
				SetError(error, "ControllerLog tick frame count is too large.");
				return false;
			}
			const size_t payloadSize = static_cast<size_t>(frameCount) * ControllerFrame::c_EncodedSize;
			std::vector<uint8_t> payload(payloadSize);
			if (!ReadExact(in, reinterpret_cast<char*>(payload.data()), payload.size())) {
				SetError(error, "ControllerLog tick payload is truncated.");
				return false;
			}
			if (ControllerFrameCodec::PayloadChecksum(payload) != checksum) {
				SetError(error, "ControllerLog tick payload checksum mismatch.");
				return false;
			}

			std::vector<ControllerFrame> frames;
			frames.reserve(frameCount);
			for (uint32_t i = 0; i < frameCount; ++i) {
				ControllerFrame frame;
				std::string decodeError;
				const uint8_t* frameBytes = payload.data() + (static_cast<size_t>(i) * ControllerFrame::c_EncodedSize);
				if (!ControllerFrameCodec::Decode(frameBytes, ControllerFrame::c_EncodedSize, frame, &decodeError)) {
					SetError(error, "ControllerLog frame decode failed: " + decodeError);
					return false;
				}
				if (!frames.empty() && frames.back().actorUniqueID >= frame.actorUniqueID) {
					SetError(error, "ControllerLog frames are not strictly sorted by actor id.");
					return false;
				}
				frames.push_back(frame);
			}
			ticks.push_back(ControllerLogTick{tick, std::move(frames)});
			hasPreviousTick = true;
			previousTick = tick;
		}
		return true;
	}

} // namespace RTE
