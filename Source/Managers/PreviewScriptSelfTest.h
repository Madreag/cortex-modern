#pragma once

#include <cstdint>
#include <string>

namespace RTE {

	class Actor;
	class MovableObject;

	/// Detecting tests for previewed-emitter subtree coverage and the shadow-self preview scope.
	class PreviewScriptSelfTest {
	public:
		static void SetSubtreeProbe(bool enabled);
		static bool SubtreeProbeEnabled();
		/// Walks the original's subtree after Arm and plays a native sound keyed by a non-root UID.
		static void ProbeArmedEmitters(const Actor* original);
		static bool CheckSubtreeEmitter(long long pressTick);
		static bool InstallStrideCounter(MovableObject* object);
		static void SetSharedSlot(bool shared);
		static bool SharedSlot();

	private:
		static bool s_SubtreeProbe;
		static bool s_SharedSlot;
		static bool s_Probed;
		static bool s_Played;
		static bool s_ChildWasPreviewed;
		static uint64_t s_RootUID;
		static uint64_t s_ChildUID;
		static uint64_t s_CommittedTick;
	};
} // namespace RTE
