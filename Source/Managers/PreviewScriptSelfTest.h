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
		static void SetStrideCounter(bool enabled);
		static bool StrideCounterRequested();
		static void NotePreviewStride(bool initialized);
		static bool PreviewStrideRan() { return s_PreviewStrideSeen; }
		static bool CheckNestedHookScope();
		/// Exercises retirement ownership and observes links before and after disposal.
		static bool RunRetirementArm(char mode);

		/// What the overlay link probe rigged inside the preview it was armed for; link targets are compared by address afterwards, never read.
		struct OverlayLinkProbe {
			char mode = 0;
			bool ran = false;
			std::string failure; //!< Why the probe could not rig its links; empty when it did.
			const Actor* residentActor = nullptr;
			const MovableObject* residentItem = nullptr;
			const MovableObject* residentPart = nullptr; //!< The resident's own counterpart of the linked shadow part.
			MovableObject* clone = nullptr;
			long cloneUID = 0;
			MovableObject* spawn = nullptr; //!< The device the clone dropped; it retires with the overlay.
			long spawnUID = 0;
			std::string spawnPreset;
			MovableObject* spawnPart = nullptr;
			MovableObject* spawnWound = nullptr;
			MovableObject* cloneWound = nullptr; //!< A wound on the surviving clone, holding a link of its own.
			MovableObject* shadowLinkPart = nullptr;
			MovableObject* spawnPartLinkPart = nullptr;
			MovableObject* spawnWoundLinkPart = nullptr;
			MovableObject* shadowPartLinkPart = nullptr;
		};
		/// Arms a one-shot probe for the next preview's first clone: 'b' spawn and shadow links, 'r' a shadow item in reach, 'c' spawn parts, wounds and a shadow part.
		static void ArmOverlayLinkProbe(char mode, const Actor* residentActor, const MovableObject* residentItem);
		static void DisarmOverlayLinkProbe();
		static const OverlayLinkProbe& GetOverlayLinkProbe() { return s_OverlayLinkProbe; }

	private:
		static void RunOverlayLinkProbe(MovableObject* object);
		static OverlayLinkProbe s_OverlayLinkProbe;
		static bool s_SubtreeProbe;
		static bool s_SharedSlot;
		static bool s_StrideCounter;
		static bool s_PreviewStrideSeen;
		static bool s_Probed;
		static bool s_Played;
		static bool s_ChildWasPreviewed;
		static uint64_t s_RootUID;
		static uint64_t s_ChildUID;
		static uint64_t s_CommittedTick;
	};
} // namespace RTE
