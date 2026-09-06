#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	class Actor;
	class MovableObject;

	/// Runs the local player's actors ahead through the queued input-delay frames so the drawn frame
	/// shows their newest input. The previews are throwaway clones stepped against a speculative
	/// overlay of the world; the canonical sim is untouched.
	class LocalPrediction {
	public:
		/// Command-line override: -1 leaves the setting in charge, 0 forces off, 1 forces on.
		static void SetCommandLineOverride(int enabled) { s_Override = enabled; }
		static bool IsEnabled();
		/// A fixed preview depth for tests; 0 means the local input delay decides.
		static void SetDepthOverride(int depth) { s_DepthOverride = depth; }
		static int GetDepthOverride() { return s_DepthOverride; }

		/// Advances a clone of every locally controlled actor to the input-delay horizon; call before drawing.
		static void RunPreview();
		/// Draws the previews in their actors' slots; EndRender puts the actors back.
		static void BeginRender();
		static void EndRender();

		/// Drops the previews outright (a match teardown).
		static void Clear();
		/// One line of counters for the match report; empty when nothing was previewed.
		static std::string DescribeStats();

		/// What the last RunPreview did, for the gates.
		struct Outcome {
			std::string equipped; //!< The first previewed human's equipped item at the horizon.
			int roundsInMag = -1; //!< Rounds left in that item when it is a firearm.
			int takenRounds = -1; //!< Rounds in the taken resident firearm, for the fired comparison.
			uint64_t spawned = 0; //!< Objects the previews queued: rounds, shells, drops.
			std::string spawnedNames; //!< Their preset names, comma separated.
			bool firedOnce = false; //!< The equipped firearm reports having fired.
			uint64_t shadows = 0;
			uint64_t taken = 0;
			uint64_t violations = 0;
		};
		static const Outcome& GetLastOutcome() { return s_LastOutcome; }
		static std::string DescribeLastOutcome();

		static uint64_t GetPreviewCount() { return s_PreviewCount; }
		static uint64_t GetPreviewTicks() { return s_PreviewTicks; }
		static double GetPreviewMs() { return s_PreviewMs; }
		static uint64_t GetShadows();
		static uint64_t GetTaken();
		static uint64_t GetViolations();

	private:
		struct Preview {
			Actor* original = nullptr;
			Actor* clone = nullptr;
			int screen = -1;
		};

		static std::vector<Preview> s_Previews;
		static std::vector<MovableObject*> s_TakenResidents;
		static Outcome s_LastOutcome;
		static bool s_Rendering;
		static int s_Override;
		static int s_DepthOverride;
		static long long s_PreviewedTick;
		static uint64_t s_PreviewCount;
		static uint64_t s_PreviewTicks;
		static double s_PreviewMs;
	};
} // namespace RTE
