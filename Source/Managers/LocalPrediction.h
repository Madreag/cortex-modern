#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	class Actor;

	/// Runs the local player's actors ahead through the queued input-delay frames so the drawn frame
	/// shows their newest input. The canonical sim is untouched: the previews are throwaway clones.
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
		/// Draws the previews in their actors' slots; EndRender puts the actors back and drops the clones.
		static void BeginRender();
		static void EndRender();

		/// Drops the previews outright (a match teardown).
		static void Clear();
		/// One line of counters for the match report; empty when nothing was previewed.
		static std::string DescribeStats();

		static uint64_t GetPreviewCount() { return s_PreviewCount; }
		static uint64_t GetPreviewTicks() { return s_PreviewTicks; }
		static double GetPreviewMs() { return s_PreviewMs; }
		static uint64_t GetRefusals() { return s_Refusals; }

	private:
		struct Preview {
			Actor* original = nullptr;
			Actor* clone = nullptr;
			int screen = -1;
		};

		static std::vector<Preview> s_Previews;
		static bool s_Rendering;
		static int s_Override;
		static int s_DepthOverride;
		static long long s_PreviewedTick;
		static uint64_t s_PreviewCount;
		static uint64_t s_PreviewTicks;
		static double s_PreviewMs;
		static uint64_t s_Refusals;
	};
} // namespace RTE
