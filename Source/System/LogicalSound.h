#pragma once

#include "CheckpointArchive.h"
#include "SoundSimulation.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace RTE {

	constexpr size_t c_MaxLogicalVoicesPerContainer = 128;

	struct LogicalSoundKey {
		SoundExecutionKey value;
		template<class Archive> void Fields(Archive& archive) { archive(value.domain, value.objectUID, value.tick, value.phase, value.occurrence, value.ordinal); }
		std::string SaveCheckpoint() const { CheckpointWriter writer("LogicalSoundKey1"); const_cast<LogicalSoundKey*>(this)->Fields(writer); return writer.Text(); }
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
			try {
				LogicalSoundKey candidate;
				CheckpointReader reader(text, "LogicalSoundKey1");
				candidate.Fields(reader);
				reader.Finish();
				if (static_cast<unsigned>(candidate.value.domain) > 2) return false;
				if (!validateOnly) *this = candidate;
				return true;
			} catch (const std::exception&) { return false; }
		}
	};

	/// A voice's progress is a function of simulation time since its anchor, so a query never changes it.
	struct LogicalSoundVoice {
		LogicalSoundKey origin;
		uint32_t sampleOrdinal = 0;
		uint32_t sampleFrames = 0;
		uint32_t loopStart = 0;
		uint32_t loopEnd = 0;
		float sampleRate = 0;
		float pitch = 1;
		int bus = 0;
		long long anchorTicks = 0;
		double anchorPosition = 0;
		int loops = 0;
		bool paused = false;
		long long fadeStartTicks = -1;
		double fadeSeconds = 0;

		struct Progress {
			double position;
			int loops;
			bool finished;
		};

		Progress At(long long ticks, long long ticksPerSecond) const {
			Progress progress{anchorPosition, loops, false};
			if (!paused && ticks > anchorTicks) {
				const double seconds = static_cast<double>(ticks - anchorTicks) / static_cast<double>(ticksPerSecond);
				progress.position += seconds * static_cast<double>(sampleRate) * static_cast<double>(pitch);
			}
			const double loopExit = static_cast<double>(loopEnd) + 1.0;
			if (progress.loops != 0 && progress.position >= loopExit) {
				const double loopLength = static_cast<double>(loopEnd - loopStart) + 1.0;
				double completed = 1.0 + std::floor((progress.position - loopExit) / loopLength);
				if (progress.loops > 0) completed = std::min(completed, static_cast<double>(progress.loops));
				progress.position -= completed * loopLength;
				if (progress.loops > 0) progress.loops -= static_cast<int>(completed);
			}
			progress.finished = progress.position >= static_cast<double>(sampleFrames);
			return progress;
		}

		/// Moves the anchor to now before pitch or pause changes so the progress so far is kept.
		void Fold(long long ticks, long long ticksPerSecond) {
			const Progress progress = At(ticks, ticksPerSecond);
			if (progress.finished) return;
			anchorTicks = ticks;
			anchorPosition = progress.position;
			loops = progress.loops;
		}

		template<class Archive> void Fields(Archive& archive) { archive(origin, sampleOrdinal, sampleFrames, loopStart, loopEnd, sampleRate, pitch, bus, anchorTicks, anchorPosition, loops, paused, fadeStartTicks, fadeSeconds); }
		std::string SaveCheckpoint() const { CheckpointWriter writer("LogicalSoundVoice1"); const_cast<LogicalSoundVoice*>(this)->Fields(writer); return writer.Text(); }
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
			try {
				LogicalSoundVoice candidate;
				CheckpointReader reader(text, "LogicalSoundVoice1");
				candidate.Fields(reader);
				reader.Finish();
				const bool valid = candidate.sampleFrames > 0 && candidate.loopStart <= candidate.loopEnd && candidate.loopEnd < candidate.sampleFrames &&
				    std::isfinite(candidate.sampleRate) && candidate.sampleRate > 0 && std::isfinite(candidate.pitch) && candidate.pitch > 0 &&
				    candidate.bus >= 0 && candidate.bus <= 2 && candidate.anchorTicks >= 0 && std::isfinite(candidate.anchorPosition) &&
				    candidate.anchorPosition >= 0 && candidate.anchorPosition < static_cast<double>(candidate.sampleFrames) && candidate.loops >= -1 &&
				    candidate.fadeStartTicks >= -1 && std::isfinite(candidate.fadeSeconds) && candidate.fadeSeconds >= 0;
				if (!valid) return false;
				if (!validateOnly) *this = std::move(candidate);
				return true;
			} catch (const std::exception&) { return false; }
		}
	};

	struct LogicalSoundPlayback {
		LogicalSoundKey identity;
		std::vector<LogicalSoundVoice> voices;
		template<class Archive> void Fields(Archive& archive) { archive(identity, voices); }
		std::string SaveCheckpoint() const { CheckpointWriter writer("LogicalSoundPlayback1"); const_cast<LogicalSoundPlayback*>(this)->Fields(writer); return writer.Text(); }
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
			try {
				LogicalSoundPlayback candidate;
				CheckpointReader reader(text, "LogicalSoundPlayback1");
				candidate.Fields(reader);
				reader.Finish();
				if (candidate.voices.size() > c_MaxLogicalVoicesPerContainer) return false;
				if (!validateOnly) *this = std::move(candidate);
				return true;
			} catch (const std::exception&) { return false; }
		}
	};

} // namespace RTE
