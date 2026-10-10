#include "Atom.h"
#include "CheckpointNativeSnapshot.h"
#include "Vector.h"
#include "Material.h"
#include "Color.h"
#include "Base64/base64.h"
#include "CheckpointArchive.h"
#include "CheckpointProperties.h"
#include "ScenarioRunner.h"

#include "SLTerrain.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "MOSRotating.h"
#include "MOPixel.h"
#include "PresetMan.h"
#include "Actor.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <future>
#include <tuple>

#include <format>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#undef GetClassName
#endif

using namespace RTE;

const std::string Atom::c_ClassName = "Atom";
std::mutex Atom::s_MemoryPoolMutex;
std::vector<void*> Atom::s_AllocatedPool;
int Atom::s_PoolAllocBlockCount = 200;
int Atom::s_InstancesInUse = 0;

namespace {
	struct NativeAtomPool {
		CheckpointPagePool pages;
		std::vector<void*> free;
	};
	NativeAtomPool* s_NativeAtomPool = nullptr;
	thread_local bool s_NativeAtomAllocation = false;
}

Atom::AllocationScope::AllocationScope(bool enabled) : m_Previous(s_NativeAtomAllocation) {
	s_NativeAtomAllocation = s_NativeAtomAllocation || enabled;
}

Atom::AllocationScope::~AllocationScope() { s_NativeAtomAllocation = m_Previous; }

struct Atom::FreezeState : std::enable_shared_from_this<FreezeState> {
	struct MaterialValue { CheckpointText text; int index; bool hasText; };
	std::shared_ptr<const CheckpointPagePool::Snapshot> pages;
	std::once_flag fieldsReady;
	std::shared_ptr<const FrozenList> fields;
	std::mutex materialMutex;
	std::unordered_map<const Material*, std::shared_ptr<const MaterialValue>> materials;
	const uint64_t serial = [] { static std::atomic<uint64_t> serials{0}; return ++serials; }();
};

Atom::SnapshotScope::SnapshotScope(bool enabled) {
	if (!enabled) return;
	m_State = std::make_shared<FreezeState>();
	{
		std::lock_guard lock(s_MemoryPoolMutex);
		if (s_NativeAtomPool) m_State->pages = s_NativeAtomPool->pages.Freeze();
	}
	m_Previous = s_FreezeState.exchange(m_State.get(), std::memory_order_acq_rel);
}

Atom::SnapshotScope::~SnapshotScope() {
	if (m_State) s_FreezeState.store(m_Previous, std::memory_order_release);
}

std::shared_ptr<const CheckpointPagePool::Snapshot> Atom::SnapshotScope::Pages() const {
	return m_State ? m_State->pages : nullptr;
}

// This forms a circle around the Atom's offset center, to check for mask color pixels in order to determine the normal at the Atom's position.
const int Atom::s_NormalChecks[c_NormalCheckCount][2] = {{0, -3}, {1, -3}, {2, -2}, {3, -1}, {3, 0}, {3, 1}, {2, 2}, {1, 3}, {0, 3}, {-1, 3}, {-2, 2}, {-3, 1}, {-3, 0}, {-3, -1}, {-2, -2}, {-1, -3}};

HitData CheckpointNativeSnapshot::Freeze(const HitData& source) {
	HitData value(source);
	for (size_t index = 0; index < 2; ++index) {
		value.Body[index] = nullptr; value.RootBody[index] = nullptr;
		value.HitMaterial[index] = Freeze(source.HitMaterial[index]);
	}
	return value;
}

Atom::Atom(const Atom& source, CheckpointNativeSnapshot& snapshot) :
	m_FrozenNative(true),
	m_Offset(snapshot.Freeze(source.m_Offset)),
	m_OriginalOffset(snapshot.Freeze(source.m_OriginalOffset)),
	m_Normal(snapshot.Freeze(source.m_Normal)),
	m_Material(snapshot.Freeze(source.m_Material)),
	m_SubgroupID(snapshot.Freeze(source.m_SubgroupID)),
	m_StepWasTaken(snapshot.Freeze(source.m_StepWasTaken)),
	m_StepRatio(snapshot.Freeze(source.m_StepRatio)),
	m_SegTraj(snapshot.Freeze(source.m_SegTraj)),
	m_SegProgress(snapshot.Freeze(source.m_SegProgress)),
	m_ChangedDir(snapshot.Freeze(source.m_ChangedDir)),
	m_PrevError(snapshot.Freeze(source.m_PrevError)),
	m_ResultWrapped(snapshot.Freeze(source.m_ResultWrapped)),
	m_MOHitsDisabled(snapshot.Freeze(source.m_MOHitsDisabled)),
	m_TerrainHitsDisabled(snapshot.Freeze(source.m_TerrainHitsDisabled)),
	m_OwnerMO(nullptr),
	m_CheckpointOwner(nullptr),
	m_IgnoreMOID(snapshot.Freeze(source.m_IgnoreMOID)),
	m_IgnoreMOIDs(snapshot.Freeze(source.m_IgnoreMOIDs)),
	m_IgnoreMOIDsByGroup(snapshot.CopyValue(source.m_IgnoreMOIDsByGroup)),
	m_LastTrailPoints(snapshot.Freeze(source.m_LastTrailPoints)),
	m_TrailPoints(snapshot.Freeze(source.m_TrailPoints)),
	m_LastHit(snapshot.Freeze(source.m_LastHit)),
	m_MOIDHit(snapshot.Freeze(source.m_MOIDHit)),
	m_TerrainMatHit(snapshot.Freeze(source.m_TerrainMatHit)),
	m_NumPenetrations(snapshot.Freeze(source.m_NumPenetrations)),
	m_TrailColor(snapshot.Freeze(source.m_TrailColor)),
	m_TrailLength(snapshot.Freeze(source.m_TrailLength)),
	m_TrailLengthVariation(snapshot.Freeze(source.m_TrailLengthVariation)),
	m_IntPos{},
	m_PrevIntPos{},
	m_TrailPos{},
	m_HitPos{},
	m_Delta{},
	m_Delta2{},
	m_Increment{},
	m_Error(snapshot.Freeze(source.m_Error)),
	m_Dom(snapshot.Freeze(source.m_Dom)),
	m_Sub(snapshot.Freeze(source.m_Sub)),
	m_DomSteps(snapshot.Freeze(source.m_DomSteps)),
	m_SubSteps(snapshot.Freeze(source.m_SubSteps)),
	m_SubStepped(snapshot.Freeze(source.m_SubStepped)),
	// The frozen materials name themselves on the saver through the snapshot; only carried names are copied here.
	m_CheckpointMaterialReferences(source.m_HasCheckpointMaterials ? source.m_CheckpointMaterialReferences : std::array<std::string, 3>{}),
	m_HasCheckpointMaterials(source.m_HasCheckpointMaterials),
	m_CheckpointLinkIDs(source.CaptureCheckpointLinkIDs()),
	m_HasCheckpointLinks(true),
	m_CheckpointInitialized(snapshot.Freeze(source.m_CheckpointInitialized)) {
	snapshot.FreezeArray(m_IntPos, source.m_IntPos);
	snapshot.FreezeArray(m_PrevIntPos, source.m_PrevIntPos);
	snapshot.FreezeArray(m_TrailPos, source.m_TrailPos);
	snapshot.FreezeArray(m_HitPos, source.m_HitPos);
	snapshot.FreezeArray(m_Delta, source.m_Delta);
	snapshot.FreezeArray(m_Delta2, source.m_Delta2);
	snapshot.FreezeArray(m_Increment, source.m_Increment);
}

Atom::Atom() {
	NoteConstruction();
	Clear();
}

Atom::Atom(const Atom& reference) {
	NoteConstruction();
	if (this != &reference) {
		Clear();
		Create(reference);
	}
}

/// Convenience constructor to both instantiate an Atom in memory and Create it at the same time.
/// @param offset An offset Vector that will be used to offset collision calculations.
/// @param material A Material that defines what material this Atom is made of.
/// @param owner The owner MovableObject of this Atom. Ownership is NOT transferred!
/// @param trailColor The trail color.
/// @param trailLength The trail length. If 0, no trail will be drawn.
Atom::Atom(const Vector& offset, Material const* material, MovableObject* owner, Color trailColor, int trailLength) {
	NoteConstruction();
	Clear();
	Create(offset, material, owner, trailColor, trailLength);
}

/// Convenience constructor to both instantiate an Atom in memory and Create it at the same time.
/// @param offset An offset Vector that will be used to offset collision calculations.
/// @param materialID The material ID of the Material that defines what this Atom is made of.
/// @param owner The owner MovableObject of this Atom. Ownership is NOT transferred!
/// @param trailColor The trail color.
/// @param trailLength The trail length. If 0, no trail will be drawn.
Atom::Atom(const Vector& offset, unsigned char materialID, MovableObject* owner, Color trailColor, int trailLength) {
	NoteConstruction();
	Clear();
	Create(offset, g_SceneMan.GetMaterialFromID(materialID), owner, trailColor, trailLength);
}

namespace {
	std::mutex s_StackSamplesMutex;
	std::array<std::array<void*, 14>, 8> s_StackSamples{};
	uint64_t s_StackSamplesTaken = 0;
	std::atomic<uint64_t> s_Constructions{0};
} // namespace

void Atom::NoteConstruction() {
	s_LiveCount.fetch_add(1, std::memory_order_relaxed);
	const uint64_t count = s_Constructions.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
	if (s_StackSampleEvery != 0 && count % s_StackSampleEvery == 0) {
		std::array<void*, 14> frames{};
		CaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
		std::lock_guard lock(s_StackSamplesMutex);
		s_StackSamples[s_StackSamplesTaken++ % s_StackSamples.size()] = frames;
	}
#endif
}

std::string Atom::SampledConstructionStacks() {
	std::lock_guard lock(s_StackSamplesMutex);
	std::string text = "atom_constructions=" + std::to_string(s_Constructions.load());
#ifdef _WIN32
	const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	for (uint64_t index = s_StackSamplesTaken > s_StackSamples.size() ? s_StackSamplesTaken - s_StackSamples.size() : 0; index < s_StackSamplesTaken; ++index) {
		text += " stack=";
		for (void* frame: s_StackSamples[index % s_StackSamples.size()]) {
			const uintptr_t address = reinterpret_cast<uintptr_t>(frame);
			if (address >= imageBase && address < imageBase + 0x3000000) text += std::format("{:X},", address - imageBase);
		}
	}
#endif
	return text;
}

Atom::~Atom() {
	if (m_FrozenNative) return;
	s_LiveCount.fetch_sub(1, std::memory_order_relaxed);
	// Clear touches a dying atom's owners first thing; comparing its fields before and after would only touch them again.
	m_CheckpointInitialized = false;
	Destroy();
}

void Atom::Destroy() {
	Clear();
}

void Atom::TouchCheckpoint() {
	if (m_OwnerMO) m_OwnerMO->TouchCheckpoint();
	if (m_CheckpointOwner && m_CheckpointOwner != m_OwnerMO) m_CheckpointOwner->TouchCheckpoint();
}

void Atom::Clear() {
	if (m_OwnerMO || m_CheckpointOwner) TouchCheckpoint();
	CheckpointChange changed(*this, [this] {
		return CheckpointFields(
			m_ChangedDir, m_CheckpointMaterialReferences, m_Delta, m_Delta2, m_Dom, m_DomSteps,
			m_Error, m_HasCheckpointMaterials, m_HitPos, m_IgnoreMOID, m_IgnoreMOIDs.empty(), m_IgnoreMOIDsByGroup,
			m_Increment, m_IntPos, m_LastHit, m_LastTrailPoints.empty(), m_MOHitsDisabled, m_MOIDHit,
			m_Material, m_Normal, m_NumPenetrations, m_Offset, m_OriginalOffset, m_PrevError,
			m_PrevIntPos, m_ResultWrapped, m_SegProgress, m_SegTraj, m_StepRatio, m_StepWasTaken,
			m_Sub, m_SubStepped, m_SubSteps, m_SubgroupID, m_TerrainHitsDisabled, m_TerrainMatHit,
			m_TrailColor, m_TrailLength, m_TrailLengthVariation, m_TrailPoints.empty(), m_TrailPos);
	}, m_CheckpointInitialized);
	m_CheckpointInitialized = true;
    m_CheckpointMaterialReferences.fill({});
    m_HasCheckpointMaterials = false;
	m_CheckpointLinkIDs.fill(0);
	m_HasCheckpointLinks = false;
	m_LastTrailPoints.clear();
	m_TrailPoints.clear();
	m_Offset.Reset();
	m_OriginalOffset.Reset();
	m_Normal.Reset();
	m_Material = g_SceneMan.GetMaterialFromID(g_MaterialAir);
	m_SubgroupID = 0;
	m_MOHitsDisabled = false;
	m_TerrainHitsDisabled = false;
	m_OwnerMO = nullptr;
	m_IgnoreMOID = g_NoMOID;
	m_IgnoreMOIDs.clear();
	m_MOIDHit = g_NoMOID;
	m_TerrainMatHit = g_MaterialAir;
	m_LastHit.Reset();
	/*
	m_HitVel.Reset();
	m_HitRadius.Reset();
	m_HitImpulse.Reset();
	*/
	m_TrailColor.Reset();
	m_TrailLength = 0;
	m_TrailLengthVariation = 0.0F;
	m_NumPenetrations = 0;
	m_ChangedDir = true;
	m_ResultWrapped = false;
	m_PrevError = 0;
	m_StepRatio = 1.0F;
	m_SegProgress = 0.0F;

	// SetupPos branches on m_IntPos before the first step sets it.
	m_IntPos[X] = m_IntPos[Y] = 0;
	m_PrevIntPos[X] = m_PrevIntPos[Y] = 0;

	m_IgnoreMOIDsByGroup = 0;

	// Bresenham step state. A fresh Atom can be stepped before SetupSeg runs (an Attachable added
	// mid-travel by an OnCollideWithTerrain script), so a stale pool value makes StepForward diverge.
	m_TrailPos[X] = m_TrailPos[Y] = 0;
	m_HitPos[X] = m_HitPos[Y] = 0;
	m_Delta[X] = m_Delta[Y] = 0;
	m_Delta2[X] = m_Delta2[Y] = 0;
	m_Increment[X] = m_Increment[Y] = 0;
	m_Error = 0;
	m_Dom = 0;
	m_Sub = 0;
	m_DomSteps = 0;
	m_SubSteps = 0;
	m_SubStepped = false;
	m_StepWasTaken = false;
	m_SegTraj.Reset();
}

int Atom::Create(const Vector& offset, Material const* material, MovableObject* owner, Color trailColor, int trailLength) {
	m_Offset = m_OriginalOffset = offset;
	// Use the offset as normal for now
	m_Normal = m_Offset;
	m_Normal.Normalize();
	m_Material = material;
	m_OwnerMO = owner;
	m_TrailColor = trailColor;
	m_TrailLength = trailLength;

	return 0;
}

int Atom::Create(const Atom& reference) {
	m_Offset = reference.m_Offset;
	m_OriginalOffset = reference.m_OriginalOffset;
	m_Normal = reference.m_Normal;
	m_Material = reference.m_Material;
	m_SubgroupID = reference.m_SubgroupID;
	m_TrailColor = reference.m_TrailColor;
	m_TrailLength = reference.m_TrailLength;
	m_TrailLengthVariation = reference.m_TrailLengthVariation;
	m_ChangedDir = reference.m_ChangedDir;
	m_PrevError = reference.m_PrevError;
	m_TerrainHitsDisabled = reference.m_TerrainHitsDisabled;
	m_NumPenetrations = reference.m_NumPenetrations;

	// These need to be set manually by the new owner.
	m_OwnerMO = nullptr;
	m_IgnoreMOIDsByGroup = 0;

	if (MovableObject::IsFaithfulClone()) {
		m_LastHit = reference.m_LastHit;
		// The copied material pointers are what the names would resolve to, unless the reference itself still holds names.
		if (!MovableObject::FaithfulCloneForPreview() || reference.m_HasCheckpointMaterials) {
			m_CheckpointMaterialReferences = reference.CaptureCheckpointMaterialReferences();
			m_HasCheckpointMaterials = true;
		}
		m_CheckpointLinkIDs = reference.CaptureCheckpointLinkIDs();
		m_HasCheckpointLinks = true;
		m_StepWasTaken = reference.m_StepWasTaken;
		m_StepRatio = reference.m_StepRatio;
		m_SegTraj = reference.m_SegTraj;
		m_SegProgress = reference.m_SegProgress;
		m_ResultWrapped = reference.m_ResultWrapped;
		m_MOHitsDisabled = reference.m_MOHitsDisabled;
		m_IgnoreMOID = reference.m_IgnoreMOID;
		m_IgnoreMOIDs = reference.m_IgnoreMOIDs;
		m_LastTrailPoints = reference.m_LastTrailPoints;
		m_TrailPoints = reference.m_TrailPoints;
		m_MOIDHit = reference.m_MOIDHit;
		m_TerrainMatHit = reference.m_TerrainMatHit;
		for (int i = 0; i < 2; ++i) {
			m_IntPos[i] = reference.m_IntPos[i];
			m_PrevIntPos[i] = reference.m_PrevIntPos[i];
			m_TrailPos[i] = reference.m_TrailPos[i];
			m_HitPos[i] = reference.m_HitPos[i];
			m_Delta[i] = reference.m_Delta[i];
			m_Delta2[i] = reference.m_Delta2[i];
			m_Increment[i] = reference.m_Increment[i];
		}
		m_Error = reference.m_Error;
		m_Dom = reference.m_Dom;
		m_Sub = reference.m_Sub;
		m_DomSteps = reference.m_DomSteps;
		m_SubSteps = reference.m_SubSteps;
		m_SubStepped = reference.m_SubStepped;
	}
	return 0;
}

std::array<long, 5> Atom::CaptureCheckpointLinkIDs() const {
	if (m_HasCheckpointLinks) return m_CheckpointLinkIDs;
	// The last collision may have destroyed its other body. Never dereference a
	// scratch pointer until the registry confirms that it still names a live MO.
	const auto liveID = [](const MovableObject* object) {
		return object && g_MovableMan.IsKnownObject(object) ? object->GetUniqueID() : 0L;
	};
	return {m_OwnerMO ? m_OwnerMO->GetUniqueID() : 0L,
	    liveID(m_LastHit.Body[0]), liveID(m_LastHit.Body[1]),
	    liveID(m_LastHit.RootBody[0]), liveID(m_LastHit.RootBody[1])};
}

std::array<std::string, 3> Atom::CaptureCheckpointMaterialReferences() const {
    if (m_HasCheckpointMaterials) return m_CheckpointMaterialReferences;
    return {g_SceneMan.SaveMaterialReference(m_Material), g_SceneMan.SaveMaterialReference(m_LastHit.HitMaterial[0]), g_SceneMan.SaveMaterialReference(m_LastHit.HitMaterial[1])};
}

std::string Atom::SaveCheckpoint() const {
    CheckpointWriter writer("Atom2");
    VisitCheckpoint(writer, *this);
    std::array<CheckpointText, 3> materials;
    const Material* sources[] = {m_Material, m_LastHit.HitMaterial[0], m_LastHit.HitMaterial[1]};
    for (size_t index = 0; index < materials.size(); ++index) {
        auto* cache = !m_HasCheckpointMaterials && CheckpointWriter::IsCapturing() && CheckpointWriter::BatchEnabled() ? CheckpointWriter::CurrentCache() : nullptr;
        constexpr unsigned materialChannel = std::numeric_limits<unsigned>::max();
        // Material references remain stable during the joined world freeze.
        if (cache) {
            if (const CheckpointText* current = cache->PeekCurrent(sources[index], materialChannel)) {
                materials[index] = *current;
                continue;
            }
        }
        CheckpointText reference = CheckpointWriter::Native([&] { return m_HasCheckpointMaterials ? m_CheckpointMaterialReferences[index] : g_SceneMan.SaveMaterialReference(sources[index]); });
        materials[index] = cache ? cache->Remember(sources[index], materialChannel, std::move(reference)) : std::move(reference);
    }
    writer(materials, CaptureCheckpointLinkIDs(), m_IgnoreMOIDsByGroup != nullptr);
    return writer.Text();
}

namespace {
	// Raw field copies avoid Atom and Color constructor side effects.
	struct AtomColorValues {
		std::array<int, 4> channels;
		std::string SaveCheckpoint() const {
			CheckpointWriter writer("Color1");
			writer(channels);
			return writer.Text();
		}
	};
	template<class T> constexpr size_t AtomPackedSize() {
		if constexpr (std::is_same_v<T, bool>) return sizeof(unsigned char);
		else if constexpr (std::is_integral_v<T> || std::is_enum_v<T> || std::is_same_v<T, float>) return sizeof(T);
		else if constexpr (std::is_same_v<T, Vector>) return 2 * sizeof(float);
		else if constexpr (std::is_same_v<T, Color>) return 4 * sizeof(int);
		else if constexpr (std::is_array_v<T>) return std::extent_v<T> * AtomPackedSize<std::remove_extent_t<T>>();
		else if constexpr (CheckpointArray<T>) return std::tuple_size_v<T> * AtomPackedSize<typename T::value_type>();
		else if constexpr (requires { typename T::first_type; typename T::second_type; }) return AtomPackedSize<typename T::first_type>() + AtomPackedSize<typename T::second_type>();
		else return 2 * sizeof(size_t);
	}
	template<class T> void PackAtomValue(char* __restrict into, size_t& at, std::pmr::vector<char>& dynamic, const T& value) {
		if constexpr (std::is_same_v<T, Vector>) {
			PackAtomValue(into, at, dynamic, value.m_X); PackAtomValue(into, at, dynamic, value.m_Y);
		} else if constexpr (std::is_same_v<T, Color>) {
			const int channels[] = {value.GetR(), value.GetG(), value.GetB(), value.GetIndex()};
			for (int channel: channels) PackAtomValue(into, at, dynamic, channel);
		} else if constexpr (std::is_array_v<T> || CheckpointArray<T>) {
			for (const auto& field: value) PackAtomValue(into, at, dynamic, field);
		} else if constexpr (requires { typename T::first_type; typename T::second_type; }) {
			PackAtomValue(into, at, dynamic, value.first); PackAtomValue(into, at, dynamic, value.second);
		} else if constexpr (requires { typename T::value_type; }) {
			const size_t count = value.size(), offset = dynamic.size();
			PackAtomValue(into, at, dynamic, count); PackAtomValue(into, at, dynamic, offset);
			if (count) {
				dynamic.resize(offset + count * AtomPackedSize<typename T::value_type>());
				size_t next = offset;
				for (const auto& field: value) PackAtomValue(dynamic.data(), next, dynamic, field);
			}
		} else {
			static_assert(std::is_trivially_copyable_v<T>);
			std::memcpy(into + at, &value, sizeof(value));
			at += sizeof(value);
		}
	}
	template<class T> constexpr bool AtomDynamicField = !std::is_array_v<T> && !CheckpointArray<T> && requires { typename T::value_type; };
	template<class T, bool ColorField> constexpr size_t AtomMetadataCount() {
		if constexpr (std::is_same_v<T, Color>) return ColorField ? 1 : 0;
		else if constexpr (AtomDynamicField<T>) return ColorField ? 0 : 1;
		else if constexpr (std::is_array_v<T>) return std::extent_v<T> * AtomMetadataCount<std::remove_extent_t<T>, ColorField>();
		else if constexpr (CheckpointArray<T>) return std::tuple_size_v<T> * AtomMetadataCount<typename T::value_type, ColorField>();
		else if constexpr (requires { typename T::first_type; typename T::second_type; }) return AtomMetadataCount<typename T::first_type, ColorField>() + AtomMetadataCount<typename T::second_type, ColorField>();
		else return 0;
	}
	struct AtomFieldRange {
		enum class Source { Image, Colors, Dynamic };
		Source source;
		size_t offset, into, size;
	};
	void AddAtomRange(std::vector<AtomFieldRange>& ranges, AtomFieldRange::Source source, size_t offset, size_t& into, size_t size) {
		if (!ranges.empty() && ranges.back().source == source && ranges.back().offset + ranges.back().size == offset && ranges.back().into + ranges.back().size == into) ranges.back().size += size;
		else ranges.push_back({source, offset, into, size});
		into += size;
	}
	template<class T> void AtomFieldRanges(std::vector<AtomFieldRange>& ranges, const Atom* atom, size_t& into, size_t& colors, size_t& dynamic, const T& value, bool frozenColors = false) {
		using Source = AtomFieldRange::Source;
		if constexpr (std::is_same_v<T, Vector>) {
			AtomFieldRanges(ranges, atom, into, colors, dynamic, value.m_X, frozenColors);
			AtomFieldRanges(ranges, atom, into, colors, dynamic, value.m_Y, frozenColors);
		} else if constexpr (std::is_same_v<T, Color>) {
			if (frozenColors) value.VisitCheckpointFields([&](const auto&... channels) { (AtomFieldRanges(ranges, atom, into, colors, dynamic, channels, true), ...); });
			else AddAtomRange(ranges, Source::Colors, colors++ * 4 * sizeof(int), into, 4 * sizeof(int));
		} else if constexpr (std::is_array_v<T> || CheckpointArray<T>) {
			for (const auto& field: value) AtomFieldRanges(ranges, atom, into, colors, dynamic, field, frozenColors);
		} else if constexpr (requires { typename T::first_type; typename T::second_type; }) {
			AtomFieldRanges(ranges, atom, into, colors, dynamic, value.first, frozenColors);
			AtomFieldRanges(ranges, atom, into, colors, dynamic, value.second, frozenColors);
		} else if constexpr (AtomDynamicField<T>) {
			AddAtomRange(ranges, Source::Dynamic, dynamic++ * 2 * sizeof(size_t), into, 2 * sizeof(size_t));
		} else {
			static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == AtomPackedSize<T>());
			const uintptr_t base = reinterpret_cast<uintptr_t>(atom), field = reinterpret_cast<uintptr_t>(&value);
			if (field < base || field - base > sizeof(Atom) - sizeof(T)) throw std::logic_error("atom field lies outside its snapshot");
			AddAtomRange(ranges, Source::Image, field - base, into, sizeof(T));
		}
	}
	template<class Record, class T> void CaptureAtomMetadata(Record& record, std::pmr::vector<char>& bytes, size_t& colors, size_t& dynamic, const T& value) {
		if constexpr (std::is_same_v<T, Color>) {
			const size_t at = colors++ * 4;
			record.colors[at] = value.GetR(); record.colors[at + 1] = value.GetG(); record.colors[at + 2] = value.GetB(); record.colors[at + 3] = value.GetIndex();
		} else if constexpr (std::is_array_v<T> || CheckpointArray<T>) {
			for (const auto& field: value) CaptureAtomMetadata(record, bytes, colors, dynamic, field);
		} else if constexpr (requires { typename T::first_type; typename T::second_type; }) {
			CaptureAtomMetadata(record, bytes, colors, dynamic, value.first); CaptureAtomMetadata(record, bytes, colors, dynamic, value.second);
		} else if constexpr (AtomDynamicField<T>) {
			const size_t count = value.size(), offset = bytes.size(), at = dynamic++ * 2;
			constexpr size_t elementBytes = AtomPackedSize<typename T::value_type>();
			if (count > (bytes.max_size() - offset) / elementBytes) throw std::length_error("atom array is too large");
			record.dynamic[at] = count; record.dynamic[at + 1] = offset;
			using Element = typename T::value_type;
			if constexpr ((std::is_integral_v<Element> || std::is_enum_v<Element> || std::is_same_v<Element, float>) && requires { value.data(); }) {
				static_assert(elementBytes == sizeof(Element));
				if (count) {
					const char* first = reinterpret_cast<const char*>(value.data());
					bytes.insert(bytes.end(), first, first + count * elementBytes);
				}
			} else {
				bytes.resize(offset + count * elementBytes);
				size_t next = offset;
				for (const auto& field: value) PackAtomValue(bytes.data(), next, bytes, field);
			}
		}
	}
	template<class T> auto UnpackAtomValue(std::string_view& values, std::string_view dynamic) {
		if constexpr (std::is_same_v<T, bool>) {
			return static_cast<unsigned int>(CheckpointProperties::Unpack<unsigned char>(values));
		} else if constexpr (std::is_same_v<T, float>) {
			return CheckpointProperties::Unpack<uint32_t>(values);
		} else if constexpr (std::is_same_v<T, Vector>) {
			return std::array{UnpackAtomValue<float>(values, dynamic), UnpackAtomValue<float>(values, dynamic)};
		} else if constexpr (std::is_same_v<T, Color>) {
			AtomColorValues color;
			for (int& channel: color.channels) channel = UnpackAtomValue<int>(values, dynamic);
			return color;
		} else if constexpr (std::is_array_v<T> || CheckpointArray<T>) {
			using Element = typename decltype([] {
				if constexpr (std::is_array_v<T>) return std::type_identity<std::remove_extent_t<T>>{};
				else return std::type_identity<typename T::value_type>{};
			}())::type;
			constexpr size_t count = [] { if constexpr (std::is_array_v<T>) return std::extent_v<T>; else return std::tuple_size_v<T>; }();
			std::array<decltype(UnpackAtomValue<Element>(values, dynamic)), count> result;
			for (auto& field: result) field = UnpackAtomValue<Element>(values, dynamic);
			return result;
		} else if constexpr (requires { typename T::first_type; typename T::second_type; }) {
			return std::pair{UnpackAtomValue<typename T::first_type>(values, dynamic), UnpackAtomValue<typename T::second_type>(values, dynamic)};
		} else if constexpr (requires { typename T::value_type; }) {
			const size_t count = CheckpointProperties::Unpack<size_t>(values), offset = CheckpointProperties::Unpack<size_t>(values);
			constexpr size_t width = AtomPackedSize<typename T::value_type>();
			if (offset > dynamic.size() || count > (dynamic.size() - offset) / width) throw std::logic_error("truncated atom array");
			std::string_view source = dynamic.substr(offset, count * width);
			std::vector<decltype(UnpackAtomValue<typename T::value_type>(source, dynamic))> result;
			result.reserve(count);
			for (size_t index = 0; index < count; ++index) result.push_back(UnpackAtomValue<typename T::value_type>(source, dynamic));
			return result;
		} else {
			return CheckpointProperties::Unpack<T>(values);
		}
	}
	template<class... T> auto UnpackAtomFields(std::type_identity<std::tuple<T...>>, std::string_view values, std::string_view dynamic) {
		auto result = std::tuple{UnpackAtomValue<T>(values, dynamic)...};
		if (!values.empty()) throw std::logic_error("trailing atom values");
		return result;
	}
}

struct Atom::FrozenList {
	struct Tail {
		std::vector<MOID> ignored;
		std::vector<std::pair<int, int>> previous, trail;
		std::array<std::string, 3> materials;
	};
	struct Record { const Atom* address; size_t backup, tail; std::array<long, 5> links; };
	std::shared_ptr<FreezeState> state;
	std::vector<Record> records;
	std::vector<Tail> tails;
	std::vector<char> backup;
	std::vector<AtomFieldRange> ranges;
	std::vector<std::pair<const Material*, std::shared_ptr<const FreezeState::MaterialValue>>> materials;
	struct Layout {
		size_t material, hitMaterials[2], hasMaterials, groupIgnore;
		size_t prevError, penetrations, terrainDisabled, changedDir, offsetX, offsetY, subgroup;
		size_t originalOffsetX, originalOffsetY, trailColor[3], trailLength, trailLengthVariation;
	} layout{};
	static constexpr size_t none = std::numeric_limits<size_t>::max();
	using Raw = std::array<char, sizeof(Atom)>;
	const FreezeState::MaterialValue& MaterialValueFor(const Material* source) const {
		const auto found = std::find_if(materials.begin(), materials.end(), [source](const auto& value) { return value.first == source; });
		if (found == materials.end() || !found->second) throw std::logic_error("frozen atom material was not captured");
		return *found->second;
	}
	Raw Read(const Record& record) const {
		Raw result;
		if (record.backup != none) std::memcpy(result.data(), backup.data() + record.backup, result.size());
		else if (!state->pages->Read(record.address, result.data(), result.size())) throw std::logic_error("frozen atom lies outside its recorded pool");
		return result;
	}
	template<class T> static T Field(const Raw& raw, size_t offset) {
		static_assert(std::is_trivially_copyable_v<T>);
		T result;
		std::memcpy(&result, raw.data() + offset, sizeof(result));
		return result;
	}
	FrozenAtom Values(const Raw& raw) const {
		const auto& fields = state->fields->layout;
		const int penetrations = Field<int>(raw, fields.penetrations);
		long long residue = static_cast<long long>(Field<int>(raw, fields.prevError)) * 256 + (penetrations < 255 ? penetrations : 255);
		residue = residue * 4 + (Field<unsigned char>(raw, fields.terrainDisabled) ? 2 : 0) + (Field<unsigned char>(raw, fields.changedDir) ? 1 : 0);
		return {residue, Vector(Field<float>(raw, fields.offsetX), Field<float>(raw, fields.offsetY)), Field<int>(raw, fields.subgroup),
		    MaterialValueFor(Field<const Material*>(raw, fields.material)).index};
	}
	size_t Bytes() const {
		size_t bytes = records.size() * (sizeof(Record) + sizeof(Atom)) + ranges.size() * sizeof(AtomFieldRange);
		for (const Tail& tail: tails) {
			bytes += sizeof(Tail) + tail.ignored.size() * sizeof(MOID) + (tail.previous.size() + tail.trail.size()) * sizeof(std::pair<int, int>);
			for (const auto& text: tail.materials) bytes += text.size();
		}
		for (const auto& [source, material]: materials) bytes += sizeof(FreezeState::MaterialValue) + material->text.OwnedBytes();
		return bytes;
	}
};

std::shared_ptr<const Atom::FrozenList> Atom::FreezeList(const std::vector<Atom*>& atoms, bool values) {
	auto list = std::make_shared<FrozenList>();
	list->state = s_FreezeState.load(std::memory_order_acquire)->shared_from_this();
	list->records.reserve(atoms.size());
	if (!atoms.empty()) std::call_once(list->state->fieldsReady, [&] {
		auto fields = std::make_shared<FrozenList>();
		const Atom& atom = *atoms.front();
		const auto offset = [&](const auto& value) { return reinterpret_cast<const char*>(&value) - reinterpret_cast<const char*>(&atom); };
		auto& layout = fields->layout;
		layout.material = offset(atom.m_Material);
		for (size_t i = 0; i < 2; ++i) layout.hitMaterials[i] = offset(atom.m_LastHit.HitMaterial[i]);
		layout.hasMaterials = offset(atom.m_HasCheckpointMaterials); layout.groupIgnore = offset(atom.m_IgnoreMOIDsByGroup);
		layout.prevError = offset(atom.m_PrevError); layout.penetrations = offset(atom.m_NumPenetrations);
		layout.terrainDisabled = offset(atom.m_TerrainHitsDisabled); layout.changedDir = offset(atom.m_ChangedDir);
		layout.offsetX = offset(atom.m_Offset.m_X); layout.offsetY = offset(atom.m_Offset.m_Y); layout.subgroup = offset(atom.m_SubgroupID);
		layout.originalOffsetX = offset(atom.m_OriginalOffset.m_X); layout.originalOffsetY = offset(atom.m_OriginalOffset.m_Y);
		atom.m_TrailColor.VisitCheckpointFields([&](const auto& red, const auto& green, const auto& blue, const auto&) {
			layout.trailColor[0] = offset(red); layout.trailColor[1] = offset(green); layout.trailColor[2] = offset(blue);
		});
		layout.trailLength = offset(atom.m_TrailLength); layout.trailLengthVariation = offset(atom.m_TrailLengthVariation);
		size_t into = 0, colors = 0, dynamic = 0;
		const auto describe = [&](const auto&... values) { (AtomFieldRanges(fields->ranges, &atom, into, colors, dynamic, values, true), ...); };
		VisitCheckpoint(describe, atom);
		list->state->fields = std::move(fields);
	});
	list->materials.reserve(3);
	std::array<const Material*, 3> lastMaterials{};
	std::array<bool, 3> haveMaterial{};
	for (const Atom* atom: atoms) {
		FrozenList::Record record{atom, FrozenList::none, FrozenList::none, values ? atom->CaptureCheckpointLinkIDs() : std::array<long, 5>{}};
		if (!list->state->pages || !list->state->pages->CanBorrow(atom, sizeof(Atom))) {
			record.backup = list->backup.size();
			const char* source = reinterpret_cast<const char*>(atom);
			list->backup.insert(list->backup.end(), source, source + sizeof(Atom));
		}
		if (values && (!atom->m_IgnoreMOIDs.empty() || !atom->m_LastTrailPoints.empty() || !atom->m_TrailPoints.empty() || atom->m_HasCheckpointMaterials)) {
			record.tail = list->tails.size();
			list->tails.push_back({atom->m_IgnoreMOIDs, atom->m_LastTrailPoints, atom->m_TrailPoints,
			    atom->m_HasCheckpointMaterials ? atom->m_CheckpointMaterialReferences : std::array<std::string, 3>{}});
		}
		const Material* sources[] = {atom->m_Material, atom->m_LastHit.HitMaterial[0], atom->m_LastHit.HitMaterial[1]};
		for (size_t index = 0; index < (values ? 3 : 1); ++index) {
			if (values && atom->m_HasCheckpointMaterials) break;
			const Material* source = sources[index];
			if (haveMaterial[index] && lastMaterials[index] == source) continue;
			haveMaterial[index] = true; lastMaterials[index] = source;
			if (std::none_of(list->materials.begin(), list->materials.end(), [source](const auto& material) { return material.first == source; })) list->materials.emplace_back(source, nullptr);
		}
		list->records.push_back(record);
	}
	// A freeze names a few dozen materials thousands of times, so each thread keeps the names it has already looked up.
	struct Known { uint64_t state = 0; std::vector<std::pair<const Material*, std::shared_ptr<const FreezeState::MaterialValue>>> values; };
	thread_local Known known;
	if (known.state != list->state->serial) known = {list->state->serial, {}};
	for (auto& [source, material]: list->materials) {
		const auto found = std::find_if(known.values.begin(), known.values.end(), [source](const auto& value) { return value.first == source; });
		if (found != known.values.end() && (!values || found->second->hasText)) { material = found->second; continue; }
		std::lock_guard lock(list->state->materialMutex);
		auto& kept = list->state->materials[source];
		if (!kept || (values && !kept->hasText)) {
			const CheckpointText text = values ? CheckpointWriter::Native([source] { return g_SceneMan.SaveMaterialReference(source); }) : CheckpointText();
			kept = std::make_shared<FreezeState::MaterialValue>(FreezeState::MaterialValue{text, source ? source->GetIndex() : -1, values});
		}
		material = kept;
		if (found != known.values.end()) found->second = kept;
		else known.values.emplace_back(source, kept);
	}
	return list;
}

CheckpointText Atom::CaptureFrozenList(const std::shared_ptr<const FrozenList>& list) {
	const auto types = [](const auto&... values) { return std::type_identity<std::tuple<std::remove_cvref_t<decltype(values)>...>>{}; };
	using Types = decltype(VisitCheckpoint(types, std::declval<const Atom&>()));
	const auto size = [](const auto&... values) { return std::integral_constant<size_t, (AtomPackedSize<std::remove_cvref_t<decltype(values)>>() + ... + size_t{0})>{}; };
	constexpr size_t width = decltype(VisitCheckpoint(size, std::declval<const Atom&>()))::value;
	const auto dynamicCount = [](const auto&... values) { return std::integral_constant<size_t, (AtomMetadataCount<std::remove_cvref_t<decltype(values)>, false>() + ... + size_t{0})>{}; };
	static_assert(decltype(VisitCheckpoint(dynamicCount, std::declval<const Atom&>()))::value == 3);
	return CheckpointText::Deferred([list] {
		std::string result = std::to_string(list->records.size()) + " ";
		for (const auto& record: list->records) {
			const auto& layout = list->state->fields->layout;
			const auto raw = list->Read(record);
			struct Dynamic { std::array<size_t, 6> dynamic; std::array<int, 4> colors; } metadata;
			std::pmr::vector<char> dynamic;
			size_t color = 0, at = 0;
			const FrozenList::Tail empty;
			const auto& tail = record.tail == FrozenList::none ? empty : list->tails.at(record.tail);
			CaptureAtomMetadata(metadata, dynamic, color, at, tail.ignored);
			CaptureAtomMetadata(metadata, dynamic, color, at, tail.previous);
			CaptureAtomMetadata(metadata, dynamic, color, at, tail.trail);
			std::array<char, width> packed;
			for (const auto& range: list->state->fields->ranges) {
				const char* source = range.source == AtomFieldRange::Source::Image ? raw.data() : reinterpret_cast<const char*>(metadata.dynamic.data());
				std::memcpy(packed.data() + range.into, source + range.offset, range.size);
			}
			CheckpointWriter writer("Atom2");
			const auto fields = UnpackAtomFields(Types{}, std::string_view(packed.data(), packed.size()), std::string_view(dynamic.data(), dynamic.size()));
			std::apply([&writer](const auto&... values) { writer(values...); }, fields);
			if (FrozenList::Field<unsigned char>(raw, layout.hasMaterials)) {
				for (const auto& reference: tail.materials) writer(CheckpointText(reference));
			} else {
				writer(list->MaterialValueFor(FrozenList::Field<const Material*>(raw, layout.material)).text);
				for (size_t offset: layout.hitMaterials) writer(list->MaterialValueFor(FrozenList::Field<const Material*>(raw, offset)).text);
			}
			writer(record.links, FrozenList::Field<const void*>(raw, layout.groupIgnore) != nullptr);
			const std::string& text = writer.Text();
			result += std::to_string(text.size()); result += " "; result += text; result += " ";
		}
		return result;
	}, list->Bytes());
}

bool Atom::CaptureFrozenProperties(Writer& writer, const std::vector<Atom*>& atoms) {
	if (!CheckpointWriter::BatchEnabled() || !s_FreezeState.load(std::memory_order_acquire)) return false;
	return CaptureFrozenListProperties(writer, FreezeList(atoms, false));
}

bool Atom::CaptureFrozenListProperties(Writer& writer, const std::shared_ptr<const FrozenList>& list) {
	if (!list) return false;
	const int indent = writer.GetIndent();
	writer.Append(CheckpointText::Deferred([list, indent] {
		const std::vector<FrozenAtom> records = FrozenValues(list);
		return Writer::Capture([&](Writer& output) {
			for (const auto& record: records) output.NewPropertyWithValue("AtomGroupResidue", record.residue);
			for (const auto& record: records) CheckpointProperties::Owned<"AtomGroupOffset", CheckpointProperties::VectorValue>::WriteValue(output, {record.offset.m_X, record.offset.m_Y});
			for (const auto& record: records) output.NewPropertyWithValue("AtomGroupSubID", record.subgroup);
			for (const auto& record: records) output.NewPropertyWithValue("AtomGroupMaterial", record.material);
		}, indent).Text();
	}, list->Bytes()));
	return true;
}

std::vector<Atom::FrozenAtom> Atom::FrozenValues(const std::shared_ptr<const FrozenList>& list) {
	std::vector<FrozenAtom> values;
	values.reserve(list->records.size());
	for (const auto& record: list->records) values.push_back(list->Values(list->Read(record)));
	return values;
}

void Atom::SaveFrozenAtoms(Writer& writer, const std::shared_ptr<const FrozenList>& list) {
	const FrozenList::Tail empty;
	const auto& layout = list->state->fields->layout;
	for (const auto& record: list->records) {
		const auto raw = list->Read(record);
		const auto& tail = record.tail == FrozenList::none ? empty : list->tails.at(record.tail);
		const Material* material = FrozenList::Field<const Material*>(raw, layout.material);
		Color trailColor;
		trailColor.SetR(FrozenList::Field<int>(raw, layout.trailColor[0])); trailColor.SetG(FrozenList::Field<int>(raw, layout.trailColor[1])); trailColor.SetB(FrozenList::Field<int>(raw, layout.trailColor[2]));
		writer.NewProperty("AddAtom");
		writer.ObjectStart(c_ClassName);
		writer.NewPropertyWithValue("Offset", Vector(FrozenList::Field<float>(raw, layout.offsetX), FrozenList::Field<float>(raw, layout.offsetY)));
		writer.NewPropertyWithValue("OriginalOffset", Vector(FrozenList::Field<float>(raw, layout.originalOffsetX), FrozenList::Field<float>(raw, layout.originalOffsetY)));
		if (!writer.IsSnapshot()) writer.NewPropertyWithValue("Material", material);
		else if (FrozenList::Field<unsigned char>(raw, layout.hasMaterials)) writer.NewPropertyWithValue("SpecialBehaviour_MaterialReference", CheckpointWriter::Native([&tail] { return tail.materials[0]; }).Base64(true));
		else writer.NewPropertyWithValue("SpecialBehaviour_MaterialReference", list->MaterialValueFor(material).text.Base64(true));
		writer.NewPropertyWithValue("TrailColor", trailColor);
		writer.NewPropertyWithValue("TrailLength", FrozenList::Field<int>(raw, layout.trailLength));
		writer.NewPropertyWithValue("TrailLengthVariation", FrozenList::Field<float>(raw, layout.trailLengthVariation));
		writer.ObjectEnd();
	}
}

CheckpointText Atom::CaptureCheckpointList(const std::vector<Atom*>& atoms) {
	if (CheckpointWriter::BatchEnabled() && s_FreezeState.load(std::memory_order_acquire)) return CaptureFrozenList(FreezeList(atoms));
	const auto types = [](const auto&... values) { return std::type_identity<std::tuple<std::remove_cvref_t<decltype(values)>...>>{}; };
	using Types = decltype(VisitCheckpoint(types, std::declval<const Atom&>()));
	const auto size = [](const auto&... values) { return std::integral_constant<size_t, (AtomPackedSize<std::remove_cvref_t<decltype(values)>>() + ... + size_t{0})>{}; };
	constexpr size_t width = decltype(VisitCheckpoint(size, std::declval<const Atom&>()))::value;
	const auto colorCount = [](const auto&... values) { return std::integral_constant<size_t, (AtomMetadataCount<std::remove_cvref_t<decltype(values)>, true>() + ... + size_t{0})>{}; };
	const auto dynamicCount = [](const auto&... values) { return std::integral_constant<size_t, (AtomMetadataCount<std::remove_cvref_t<decltype(values)>, false>() + ... + size_t{0})>{}; };
	struct Record {
		Record() {}
		std::array<int, decltype(VisitCheckpoint(colorCount, std::declval<const Atom&>()))::value * 4> colors;
		std::array<size_t, decltype(VisitCheckpoint(dynamicCount, std::declval<const Atom&>()))::value * 2> dynamic;
		std::array<size_t, 3> materials;
		std::array<long, 5> links;
		bool groupIgnoreList;
	};
	auto storage = CheckpointBuffer::LeaseCaptureStorage();
	auto* resource = storage ? storage.get() : std::pmr::get_default_resource();
	std::vector<AtomFieldRange> ranges;
	size_t firstByte = sizeof(Atom), lastByte = 0;
	if (!atoms.empty()) {
		size_t into = 0, colors = 0, dynamic = 0;
		const auto describe = [&](const auto&... values) { (AtomFieldRanges(ranges, atoms.front(), into, colors, dynamic, values), ...); };
		VisitCheckpoint(describe, *atoms.front());
		if (into != width) throw std::logic_error("atom snapshot field size differs");
		for (const auto& range: ranges) if (range.source == AtomFieldRange::Source::Image) {
			firstByte = std::min(firstByte, range.offset); lastByte = std::max(lastByte, range.offset + range.size);
		}
		if (lastByte == 0) firstByte = 0;
		for (auto& range: ranges) if (range.source == AtomFieldRange::Source::Image) range.offset -= firstByte;
	} else firstByte = 0;
	const size_t imageBytes = lastByte - firstByte;
	struct Byte { char value; Byte() {} };
	static_assert(sizeof(Byte) == 1 && std::is_trivially_copyable_v<Byte>);
	std::pmr::vector<Byte> images{resource};
	if (imageBytes && atoms.size() > images.max_size() / imageBytes) throw std::length_error("atom snapshots are too large");
	images.resize(atoms.size() * imageBytes);
	std::pmr::vector<Record> records{resource};
	records.reserve(atoms.size());
	std::pmr::vector<char> dynamic{resource};
	size_t dynamicBytes = 0;
	for (const Atom* atom: atoms) dynamicBytes += atom->m_IgnoreMOIDs.size() * AtomPackedSize<MOID>()
	    + (atom->m_LastTrailPoints.size() + atom->m_TrailPoints.size()) * AtomPackedSize<std::pair<int, int>>();
	dynamic.reserve(dynamicBytes);
	std::vector<CheckpointText> materials;
	std::unordered_map<const Material*, size_t> materialIndices;
	std::map<std::string, size_t> restoredIndices;
	size_t bytes = 0;
	for (const Atom* atom: atoms) {
		std::array<size_t, 3> indices;
		const Material* sources[] = {atom->m_Material, atom->m_LastHit.HitMaterial[0], atom->m_LastHit.HitMaterial[1]};
		for (size_t index = 0; index < indices.size(); ++index) {
			if (atom->m_HasCheckpointMaterials) {
				const std::string& reference = atom->m_CheckpointMaterialReferences[index];
				const auto [found, inserted] = restoredIndices.try_emplace(reference, materials.size());
				if (inserted) materials.emplace_back(reference);
				indices[index] = found->second;
			} else {
				const auto [found, inserted] = materialIndices.try_emplace(sources[index], materials.size());
				if (inserted) {
					auto* cache = CheckpointWriter::CurrentCache();
					constexpr unsigned channel = std::numeric_limits<unsigned>::max();
					const CheckpointText* current = cache ? cache->PeekCurrent(sources[index], channel) : nullptr;
					if (current) materials.push_back(*current);
					else {
						CheckpointText reference = CheckpointWriter::Native([&] { return g_SceneMan.SaveMaterialReference(sources[index]); });
						materials.push_back(cache ? cache->Remember(sources[index], channel, std::move(reference)) : std::move(reference));
					}
				}
				indices[index] = found->second;
			}
		}
		Record& record = records.emplace_back();
		// Only scalar ranges are decoded; copied pointer bytes never name live objects.
		std::memcpy(images.data() + (records.size() - 1) * imageBytes, reinterpret_cast<const char*>(atom) + firstByte, imageBytes);
		size_t colors = 0, arrays = 0;
		const auto metadata = [&](const auto&... values) { (CaptureAtomMetadata(record, dynamic, colors, arrays, values), ...); };
		VisitCheckpoint(metadata, *atom);
		record.materials = indices;
		record.links = atom->CaptureCheckpointLinkIDs();
		record.groupIgnoreList = atom->m_IgnoreMOIDsByGroup != nullptr;
		bytes += sizeof(Record);
	}
	bytes += images.size() + dynamic.size() + ranges.size() * sizeof(AtomFieldRange);
	for (const CheckpointText& material: materials) bytes += sizeof(CheckpointText) + material.OwnedBytes();
	// The saver formats owned atom fields in Atom2 order with their length prefixes.
	return CheckpointText::Deferred([storage = std::move(storage), records = std::move(records), images = std::move(images), ranges = std::move(ranges), imageBytes,
	    dynamic = std::move(dynamic), materials = std::move(materials)] {
		std::string list = std::to_string(records.size()) + " ";
		for (size_t index = 0; index < records.size(); ++index) {
			const Record& record = records[index];
			std::array<char, width> packed;
			for (const auto& range: ranges) {
				const char* source = range.source == AtomFieldRange::Source::Image ? reinterpret_cast<const char*>(images.data()) + index * imageBytes :
				    range.source == AtomFieldRange::Source::Colors ? reinterpret_cast<const char*>(record.colors.data()) : reinterpret_cast<const char*>(record.dynamic.data());
				std::memcpy(packed.data() + range.into, source + range.offset, range.size);
			}
			CheckpointWriter writer("Atom2");
			const auto fields = UnpackAtomFields(Types{}, std::string_view(packed.data(), packed.size()), std::string_view(dynamic.data(), dynamic.size()));
			std::apply([&writer](const auto&... values) { writer(values...); }, fields);
			for (size_t index: record.materials) writer(materials.at(index));
			writer(record.links, record.groupIgnoreList);
			const std::string& text = writer.Text();
			list += std::to_string(text.size());
			list += " ";
			list += text;
			list += " ";
		}
		return list;
	}, bytes);
}


std::string Atom::CheckpointListSelfTestMismatch() {
	std::string expected;
	CheckpointText captured;
	CheckpointText frozen, properties, later, arrivedLinks;
	std::string expectedLater, expectedArrived;
	std::string expectedProperties;
	{
		struct RestoreCounter {
			long counter = MovableObject::GetUniqueIDCounter();
			~RestoreCounter() { MovableObject::PinUniqueIDCounter(counter); }
		} restoreCounter;
		MOPixel owner, arrived;
		if (owner.MovableObject::Create() < 0) return "could not register atom link fixture";
		auto first = std::make_unique<Atom>();
		void* native;
		{
			std::lock_guard lock(s_MemoryPoolMutex);
			if (!s_NativeAtomPool) s_NativeAtomPool = new NativeAtomPool;
			if (s_NativeAtomPool->free.empty()) s_NativeAtomPool->pages.Grow(sizeof(Atom), 2, s_NativeAtomPool->free);
			native = s_NativeAtomPool->free.back(); s_NativeAtomPool->free.pop_back(); ++s_InstancesInUse;
		}
		std::unique_ptr<Atom> second(::new (native) Atom);
		const auto fill = []<class Self, class T>(Self& self, T& value, uint64_t& bits) -> void {
			if constexpr (std::is_same_v<T, Vector>) {
				self(self, value.m_X, bits); self(self, value.m_Y, bits);
			} else if constexpr (std::is_same_v<T, Color>) {
				value.SetR(17); value.SetG(31); value.SetB(47);
			} else if constexpr (std::is_array_v<T> || CheckpointArray<T>) {
				for (auto& field: value) self(self, field, bits);
			} else if constexpr (!AtomDynamicField<T>) {
				static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(bits));
				bits = bits * UINT64_C(6364136223846793005) + 1;
				std::memcpy(&value, &bits, sizeof(T));
			}
		};
		uint64_t bits = 31;
		const auto allFields = [&](auto&... values) { (fill(fill, values, bits), ...); };
		VisitCheckpoint(allFields, *second);
		second->m_ChangedDir = true; second->m_TerrainHitsDisabled = false;
		second->SetOwner(&owner);
		second->m_LastHit.Body[0] = &owner; second->m_LastHit.RootBody[1] = &owner;
		first->m_Offset.m_X = std::bit_cast<float>(uint32_t{0x7fc01234});
		first->m_OriginalOffset.m_Y = -0.0F;
		first->m_IgnoreMOIDs = {1, 17, 99};
		first->m_LastTrailPoints = {{-31, 2}, {100, -200}};
		first->m_TrailPoints = {{7, 8}};
		first->m_IntPos[0] = std::numeric_limits<int>::min();
		first->m_LastHit.TotalMass[1] = std::bit_cast<float>(uint32_t{0xffc05678});
		const unsigned char raw = 254;
		std::memcpy(&first->m_LastHit.Terminate[0], &raw, sizeof(raw));
		first->m_HasCheckpointLinks = true;
		first->m_CheckpointLinkIDs = {0, 41, 0, 51, 61};
		first->m_HasCheckpointMaterials = true;
		first->m_CheckpointMaterialReferences = {std::string("named\0material", 14), "", "same"};
		std::vector<MOID> ignored = {23};
		first->m_IgnoreMOIDsByGroup = &ignored;
		std::vector<Atom*> atoms = {first.get(), second.get(), first.get()};
		const auto ordinary = [](const std::vector<Atom*>& values) {
			std::string result = std::to_string(values.size()) + " ";
			for (const Atom* atom: values) {
				const std::string text = atom->SaveCheckpoint();
				result += std::to_string(text.size()) + " " + text + " ";
			}
			return result;
		};
		expected = ordinary(atoms);
		expectedProperties = Writer::Capture([&](Writer& writer) {
			for (const Atom* atom: atoms) writer.NewPropertyWithValue("AtomGroupResidue", atom->PackTravelResidue());
			for (const Atom* atom: atoms) writer.NewPropertyWithValue("AtomGroupOffset", atom->GetOffset());
			for (const Atom* atom: atoms) writer.NewPropertyWithValue("AtomGroupSubID", static_cast<long long>(atom->GetSubID()));
			for (const Atom* atom: atoms) writer.NewPropertyWithValue("AtomGroupMaterial", static_cast<int>(atom->GetMaterial()->GetIndex()));
		}, 3).Text();
		{
			CheckpointWriter::BatchScope batch(true);
			captured = CheckpointWriter::CaptureValues([&] { return CaptureCheckpointList(atoms); });
			SnapshotScope snapshot(true);
			frozen = CheckpointWriter::CaptureValues([&] { return CaptureCheckpointList(atoms); });
			properties = Writer::Capture([&](Writer& writer) {
				if (!CaptureFrozenProperties(writer, atoms)) throw std::logic_error("atom page snapshot was not used");
			}, 3);
			second->SetTrailLength(409);
			g_MovableMan.UnregisterObject(&owner);
			expectedLater = ordinary(atoms);
			later = CheckpointWriter::CaptureValues([&] { return CaptureCheckpointList(atoms); });
			if (arrived.MovableObject::Create() < 0) return "could not register atom link during capture";
			second->SetOwner(&arrived);
			second->m_LastHit.Body[1] = &arrived; second->m_LastHit.RootBody[0] = &arrived;
			expectedArrived = ordinary(atoms);
			arrivedLinks = CheckpointWriter::CaptureValues([&] { return CaptureCheckpointList(atoms); });
		}
		first->m_IgnoreMOIDs.clear();
		first->m_LastTrailPoints.clear();
		first->m_CheckpointMaterialReferences.fill("changed");
		first->m_LastHit.TotalMass[1] = 123.0F;
		second->m_Offset.m_X = 123.0F;
		second.reset();
		{
			std::lock_guard lock(s_MemoryPoolMutex);
			if (s_NativeAtomPool->free.back() != native) return "native atom slot did not return to its pool";
			s_NativeAtomPool->free.pop_back(); ++s_InstancesInUse;
		}
		std::unique_ptr<Atom> reused(::new (native) Atom);
		reused->SetPrevError(-123);
	}
	// All live atoms, strings, trails and group pointers have died before traversal.
	std::array<std::future<std::pair<std::string, std::string>>, 4> readers;
	for (auto& reader: readers) reader = std::async(std::launch::async, [captured] {
		CheckpointWriter::BatchScope nextCapture(true);
		const auto fresh = CheckpointWriter::CaptureValues([] { return CaptureCheckpointList({}); });
		if (fresh.Text() != "0 ") throw std::logic_error("new atom capture differs");
		return std::pair{captured.Text(), captured.SharedText()};
	});
	for (auto& reader: readers) {
		const auto [full, shared] = reader.get();
		if (full != expected) return "owned atom list differs from the ordinary field order";
		if (shared != expected) return "atom list lost shared fields";
	}
	for (auto& reader: readers) reader = std::async(std::launch::async, [frozen, properties] { return std::pair{frozen.Text(), properties.Text()}; });
	for (auto& reader: readers) {
		const auto [full, columns] = reader.get();
		if (full != expected || frozen.SharedText() != expected) return "frozen atom pages differ after mutation, death and pool reuse";
		if (columns != expectedProperties || properties.SharedText() != expectedProperties) return "frozen atom columns differ after source death";
	}
	if (std::async(std::launch::async, [later] { return later.Text(); }).get() != expectedLater || later.SharedText() != expectedLater)
		return "atom changes during the capture were hidden from later native readers";
	if (std::async(std::launch::async, [arrivedLinks] { return arrivedLinks.Text(); }).get() != expectedArrived || arrivedLinks.SharedText() != expectedArrived)
		return "atom links registered during capture were lost after source death";
	CheckpointWriter::BatchScope batch(true);
	const auto empty = CaptureCheckpointList({});
	if (empty.Text() != "0 " || empty.SharedText() != "0 ") return "empty atom list differs";
	return {};
}

bool Atom::LoadCheckpoint(std::string_view text, bool validateOnly) {
    try {
        const bool legacy = text.starts_with("5 Atom1 ");
        CheckpointReader reader(text, legacy ? "Atom1" : "Atom2", validateOnly);
        VisitCheckpoint(reader, *this);
        std::array<std::string, 3> materials;
        std::array<long, 5> links;
        bool usesGroupIgnoreList;
        if (legacy) {
            std::array<int, 3> indices;
            reader.Value(indices[0]); reader.Value(links); reader.Value(usesGroupIgnoreList);
            reader.Value(indices[1]); reader.Value(indices[2]);
            for (size_t index = 0; index < indices.size(); ++index) {
                if (indices[index] < -1 || indices[index] >= c_PaletteEntriesNumber) return false;
                materials[index] = g_SceneMan.SaveMaterialReference(indices[index] < 0 ? nullptr : g_SceneMan.GetMaterialFromID(static_cast<unsigned char>(indices[index])));
            }
        } else {
            reader.Value(materials); reader.Value(links); reader.Value(usesGroupIgnoreList);
            for (const auto& material: materials) if (!SceneMan::ValidateMaterialReference(material)) return false;
        }
        for (long uid: links) if (uid < 0) return false;
        reader.OnCommit([this, materials, links, usesGroupIgnoreList] {
			m_CheckpointMaterialReferences = materials; m_HasCheckpointMaterials = true;
			m_CheckpointLinkIDs = links; m_HasCheckpointLinks = true;
            if (!usesGroupIgnoreList) m_IgnoreMOIDsByGroup = nullptr;
        });
        reader.Finish();
        return true;
    } catch (const std::exception&) { return false; }
}

void Atom::ResolveCheckpointLinks() {
    if (m_HasCheckpointMaterials) {
        std::array<const Material*, 3> materials;
        for (size_t index = 0; index < materials.size(); ++index) materials[index] = g_SceneMan.ResolveMaterialReference(m_CheckpointMaterialReferences[index]);
        m_Material = materials[0]; m_LastHit.HitMaterial[0] = materials[1]; m_LastHit.HitMaterial[1] = materials[2];
        m_CheckpointMaterialReferences.fill({}); m_HasCheckpointMaterials = false;
    }
	if (!m_HasCheckpointLinks) return;
	m_OwnerMO = g_MovableMan.FindObjectByUniqueID(m_CheckpointLinkIDs[0]);
	for (int index = 0; index < 2; ++index) {
		m_LastHit.Body[index] = g_MovableMan.FindObjectByUniqueID(m_CheckpointLinkIDs[1 + index]);
		m_LastHit.RootBody[index] = g_MovableMan.FindObjectByUniqueID(m_CheckpointLinkIDs[3 + index]);
	}
	m_CheckpointLinkIDs.fill(0);
	m_HasCheckpointLinks = false;
}

int Atom::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("Offset", { reader >> m_Offset; });
	MatchProperty("OriginalOffset", { reader >> m_OriginalOffset; });
    MatchProperty("SpecialBehaviour_MaterialReference", {
        const std::string material = base64_decode(reader.ReadPropValue());
        if (!SceneMan::ValidateMaterialReference(material)) reader.ReportError("invalid Atom material checkpoint reference");
        m_CheckpointMaterialReferences = CaptureCheckpointMaterialReferences();
        m_CheckpointMaterialReferences[0] = material; m_HasCheckpointMaterials = true;
        if (const Material* found = g_SceneMan.ResolveMaterialReference(material, true)) m_Material = found;
    });
	MatchProperty("Material", {
        m_CheckpointMaterialReferences.fill({}); m_HasCheckpointMaterials = false;
		Material mat;
		mat.Reset();
		reader >> mat;
		m_Material = g_SceneMan.AddMaterialCopy(&mat);
		if (!m_Material) {
			RTEAbort("Failed to store material \"" + mat.GetPresetName() + "\". Aborting!");
		}
	});
	MatchProperty("TrailColor", { reader >> m_TrailColor; });
	MatchProperty("TrailLength", { reader >> m_TrailLength; });
	MatchProperty("TrailLengthVariation", { reader >> m_TrailLengthVariation; });

	EndPropertyList;
}

int Atom::Save(Writer& writer) const {
	Serializable::Save(writer);

	writer.NewPropertyWithValue("Offset", m_Offset);
	writer.NewPropertyWithValue("OriginalOffset", m_OriginalOffset);
    if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_MaterialReference", CheckpointWriter::Native([&] { return m_HasCheckpointMaterials ? m_CheckpointMaterialReferences[0] : g_SceneMan.SaveMaterialReference(m_Material); }).Base64(true));
    else writer.NewPropertyWithValue("Material", m_Material);
	writer.NewPropertyWithValue("TrailColor", m_TrailColor);
	writer.NewPropertyWithValue("TrailLength", m_TrailLength);
	writer.NewPropertyWithValue("TrailLengthVariation", m_TrailLengthVariation);

	return 0;
}

void* Atom::GetPoolMemory() {
	std::lock_guard<std::mutex> guard(s_MemoryPoolMutex);
	if (s_NativeAtomAllocation || ScenarioRunner::HasLockstepCoordinator()) {
		// The native pool stays alive through manager and static teardown.
		if (!s_NativeAtomPool) s_NativeAtomPool = new NativeAtomPool;
		if (s_NativeAtomPool->free.empty()) s_NativeAtomPool->pages.Grow(sizeof(Atom), std::max(s_PoolAllocBlockCount, 10), s_NativeAtomPool->free);
		void* memory = s_NativeAtomPool->free.back();
		s_NativeAtomPool->free.pop_back();
		++s_InstancesInUse;
		return memory;
	}

	// If the pool is empty, then fill it up again with as many instances as we are set to
	if (s_AllocatedPool.empty()) {
		FillPool((s_PoolAllocBlockCount > 0) ? s_PoolAllocBlockCount : 10);
	}

	// Get the instance in the top of the pool and pop it off
	void* foundMemory = s_AllocatedPool.back();
	s_AllocatedPool.pop_back();

	RTEAssert(foundMemory, "Could not find an available instance in the pool, even after increasing its size!");

	// Keep track of the number of instances passed out
	s_InstancesInUse++;

	return foundMemory;
}

void Atom::FillPool(int fillAmount) {
	// Made after the pool and so destroyed before it: at exit the free blocks go back to the allocator.
	static struct PoolRelease {
		~PoolRelease() {
			std::lock_guard<std::mutex> guard(s_MemoryPoolMutex);
			for (void* memory: s_AllocatedPool) {
				free(memory);
			}
			s_AllocatedPool.clear();
		}
	} s_PoolRelease;

	// Default to the set block allocation size if fillAmount is 0
	if (fillAmount <= 0) {
		fillAmount = s_PoolAllocBlockCount;
	}

	// If concrete class, fill up the pool with pre-allocated memory blocks the size of the type
	if (fillAmount > 0) {
		// As many as we're asked to make
		for (int i = 0; i < fillAmount; ++i) {
			s_AllocatedPool.push_back(malloc(sizeof(Atom)));
		}
	}
}

int Atom::ReturnPoolMemory(void* returnedMemory) {
	if (!returnedMemory) {
		return false;
	}

	std::lock_guard<std::mutex> guard(s_MemoryPoolMutex);
	if (s_NativeAtomPool && s_NativeAtomPool->pages.Contains(returnedMemory)) s_NativeAtomPool->free.push_back(returnedMemory);
	else s_AllocatedPool.push_back(returnedMemory);

	// Keep track of the number of instances passed in
	s_InstancesInUse--;

	return s_InstancesInUse;
}

bool Atom::CalculateNormal(BITMAP* sprite, Vector spriteCenter) {
	RTEAssert(sprite, "Trying to set up Atom normal without passing in bitmap");

	// Can't set up a normal on an atom that doesn't have an offset from its parent's center
	if (m_Offset.IsZero()) {
		m_Normal.Reset();
		return false;
	}
	// See if the atom even ends up in the sprite at all
	Vector atomPos = spriteCenter + m_Offset;
	if (atomPos.m_X < 0 || atomPos.m_Y < 0 || atomPos.m_X >= sprite->w || atomPos.m_Y >= sprite->h) {
		return false;
	}
	// Go through all the check positions from the atom's position on the sprite
	m_Normal.Reset();
	int checkPixel = 0;
	for (int check = 0; check < c_NormalCheckCount; ++check) {
		// Establish the current integer position to check for nothingness on the sprite
		checkPixel = getpixel(sprite, atomPos.m_X + s_NormalChecks[check][X], atomPos.m_Y + s_NormalChecks[check][Y]);

		// If the pixel was outside of the bitmap, or on the key color, then that's a valid direction for normal, add it to the accumulated normal
		if (checkPixel < 0 || checkPixel == g_MaskColor) {
			m_Normal.m_X += s_NormalChecks[check][X];
			m_Normal.m_Y += s_NormalChecks[check][Y];
		}
	}
	m_Normal.Normalize();
	/*
	// Check whether the normal vector makes sense at all. It can't point against the offset, for example
	if (m_Normal.Dot(m_Offset) < 0)
	{
	    // Abort and revert to offset-based normal
	    m_Normal = m_Offset;
	    m_Normal.Normalize();
	    return false;
	}
	*/
	return true;
}

void Atom::DrawTrail(BITMAP* targetBitmap, const Vector& targetPos) const {
	if (m_TrailLength == 0 || (m_LastTrailPoints.size() + m_TrailPoints.size()) == 0) {
		return;
	}

	if (!targetBitmap) {
		return;
	}

	Vector topLeftExtent = Vector(FLT_MAX, FLT_MAX);
	Vector bottomRightExtent = Vector(-FLT_MAX, -FLT_MAX);

	// This should probably only be set in the sim update and cached
	int length = static_cast<int>(static_cast<float>(m_TrailLength) * RandomNum(1.0F - m_TrailLengthVariation, 1.0F));

	// Might be better to have one list for the trailpoints and keep track of the divide between last and current, but this is simpler
	// TODO, improve this so that we don't suddenly have the trail dissapear when the atom despawns, we need to continue rendering this for one extra sim update
	int endPoint = m_LastTrailPoints.size() + (m_TrailPoints.size() * g_TimerMan.GetSimUpdateProportion());
	std::vector<std::pair<int, int>> allTrailPoints = m_LastTrailPoints;
	allTrailPoints.insert(allTrailPoints.end(), m_TrailPoints.begin(), m_TrailPoints.end());
	for (int i = endPoint - std::min(length, static_cast<int>(endPoint)); i < endPoint; ++i) {
		Vector trailPointPos = Vector(allTrailPoints[i].first, allTrailPoints[i].second) - targetPos;
		putpixel(targetBitmap, trailPointPos.GetFloorIntX(), trailPointPos.GetFloorIntY(), m_TrailColor.GetIndex());

		topLeftExtent.m_X = std::min(topLeftExtent.m_X, trailPointPos.m_X);
		topLeftExtent.m_Y = std::min(topLeftExtent.m_Y, trailPointPos.m_Y);
		bottomRightExtent.m_X = std::max(bottomRightExtent.m_X, trailPointPos.m_X);
		bottomRightExtent.m_Y = std::max(bottomRightExtent.m_Y, trailPointPos.m_Y);
	}

	// No trail point drawn leaves the extents at +-FLT_MAX, which no int holds: there is nothing to register.
	if (topLeftExtent.m_X > bottomRightExtent.m_X) {
		return;
	}
	g_SceneMan.RegisterDrawing(targetBitmap, g_NoMOID, topLeftExtent.m_X, topLeftExtent.m_Y, bottomRightExtent.m_X + 1.0F, bottomRightExtent.m_Y + 1.0F);
}

bool Atom::IsIgnoringMOID(MOID whichMOID) {
	if (whichMOID == m_IgnoreMOID) {
		return true;
	}
	const MovableObject* hitMO = g_MovableMan.GetMOFromID(whichMOID);
	hitMO = hitMO ? hitMO->GetRootParent() : 0;

	// First check if we are ignoring the team of the MO we hit, or if it's an AtomGroup and we're ignoring all those
	if (m_OwnerMO && hitMO) {
		if (m_OwnerMO->IgnoresTeamHits() && hitMO->IgnoresTeamHits() && m_OwnerMO->GetTeam() == hitMO->GetTeam()) {
			return true;
		}
		if ((m_OwnerMO->IgnoresAtomGroupHits() && dynamic_cast<const MOSRotating*>(hitMO)) || (hitMO->IgnoresAtomGroupHits() && dynamic_cast<const MOSRotating*>(m_OwnerMO))) {
			return true;
		}
		if ((m_OwnerMO->GetIgnoresActorHits() && dynamic_cast<const Actor*>(hitMO)) || (hitMO->GetIgnoresActorHits() && dynamic_cast<const Actor*>(m_OwnerMO))) {
			return true;
		}
	}
	// Now check for explicit ignore
	bool ignored = false;
	for (const MOID& moid: m_IgnoreMOIDs) {
		if (moid == whichMOID) {
			ignored = true;
			break;
		}
	}
	// Check in AtomGroup-owned list if it's assigned to this atom
	if (!ignored && m_IgnoreMOIDsByGroup) {
		for (const MOID& moid: *m_IgnoreMOIDsByGroup) {
			if (moid == whichMOID) {
				ignored = true;
				break;
			}
		}
	}
	return ignored;
}

bool Atom::MOHitResponse() {
	RTEAssert(m_OwnerMO, "Stepping an Atom without a parent MO!");

	if (m_OwnerMO->m_HitsMOs && m_MOIDHit != g_NoMOID /*&& IsIgnoringMOID(m_MOIDHit)*/) {
		m_LastHit.HitPoint.Reset();
		m_LastHit.BitmapNormal.Reset();
		bool hit[2];
		hit[X] = hit[Y] = false;
		bool validHit = true;

		// Check for the collision point in the dominant direction of travel.
		if (m_Delta[m_Dom] && ((m_Dom == X && g_SceneMan.GetMOIDPixel(m_HitPos[X], m_IntPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID) || (m_Dom == Y && g_SceneMan.GetMOIDPixel(m_IntPos[X], m_HitPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID))) {
			hit[m_Dom] = true;
			m_LastHit.HitPoint = (m_Dom == X) ? Vector(m_HitPos[X], m_IntPos[Y]) : Vector(m_IntPos[X], m_HitPos[Y]);
			m_LastHit.BitmapNormal[m_Dom] = -m_Increment[m_Dom];
		}

		// Check for the collision point in the submissive direction of travel.
		if (m_SubStepped && m_Delta[m_Sub] && ((m_Sub == X && g_SceneMan.GetMOIDPixel(m_HitPos[X], m_IntPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID) || (m_Sub == Y && g_SceneMan.GetMOIDPixel(m_IntPos[X], m_HitPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID))) {
			hit[m_Sub] = true;
			if (m_LastHit.HitPoint.IsZero()) {
				m_LastHit.HitPoint = (m_Sub == X) ? Vector(m_HitPos[X], m_IntPos[Y]) : Vector(m_IntPos[X], m_HitPos[Y]);
			} else {
				// We hit pixels in both sub and dom directions on the other MO, a corner hit.
				m_LastHit.HitPoint.SetXY(m_HitPos[X], m_HitPos[Y]);
			}
			m_LastHit.BitmapNormal[m_Sub] = -m_Increment[m_Sub];
		}

		// If neither the direct dominant or sub directions yielded a collision point, then that means we hit right on the corner edge of a pixel, and that is the collision point.
		if (!hit[m_Dom] && !hit[m_Sub]) {
			hit[m_Dom] = hit[m_Sub] = true;
			m_LastHit.HitPoint.SetXY(m_HitPos[X], m_HitPos[Y]);
			m_LastHit.BitmapNormal.SetXY(-m_Increment[X], -m_Increment[Y]);
		}
		m_LastHit.BitmapNormal.Normalize();

		if (!m_Normal.IsZero()) {
			m_LastHit.BitmapNormal = -m_OwnerMO->RotateOffset(m_Normal);
		}

		// Cancel collision response for this if it appears the collision is happening in the 'wrong' direction, meaning away from the center.
		// This happens when things are sunk into each other, and thus getting 'hooked' on each other
		if (m_LastHit.HitRadius[HITOR].Dot(m_LastHit.BitmapNormal) >= 0) {
			// Hitee hit radius and the normal presented to the hitor are facing each other! We are colliding in the wrong direction!
			validHit = false;
		}

		m_LastHit.Body[HITOR] = m_OwnerMO;
		m_LastHit.Body[HITEE] = g_MovableMan.GetMOFromID(m_MOIDHit);

#ifndef RELEASE_BUILD
		RTEAssert(m_LastHit.Body[HITEE], "Hitee MO is 0 in Atom::MOHitResponse!");
		RTEAssert(m_MOIDHit == m_LastHit.Body[HITEE]->GetID(), "g_MovableMan.GetMOFromID messed up in Atom::MOHitResponse!");
#endif

		// Get the roots for both bodies
		if (m_LastHit.Body[HITOR]) {
			m_LastHit.RootBody[HITOR] = m_LastHit.Body[HITOR]->GetRootParent();
		}
		if (m_LastHit.Body[HITEE]) {
			m_LastHit.RootBody[HITEE] = m_LastHit.Body[HITEE]->GetRootParent();
		}

		if (SceneMan::IsTrackedUID(m_OwnerMO->GetUniqueID()) || SceneMan::IsTrackedUID(m_LastHit.Body[HITEE]->GetUniqueID())) {
			SceneMan::TraceTerrainEvent("ghit", std::bit_cast<int32_t>(m_LastHit.HitPoint.m_X), std::bit_cast<int32_t>(m_LastHit.HitPoint.m_Y), static_cast<int>(m_LastHit.Body[HITEE]->GetUniqueID()), std::bit_cast<int32_t>(m_OwnerMO->GetVel().m_X), static_cast<int>(m_OwnerMO->GetUniqueID()));
		}
		validHit = validHit && m_LastHit.Body[HITEE]->CollideAtPoint(m_LastHit);

		return validHit;
	}
	RTEAbort("Atom not supposed to do MO hit response if it didn't hit anything!");
	return false;
}

HitData& Atom::TerrHitResponse() {
	RTEAssert(m_OwnerMO, "Stepping an Atom without a parent MO!");

	if (m_TerrainMatHit) {
		MID hitMaterialID = g_SceneMan.GetTerrMatter(m_HitPos[X], m_HitPos[Y]);
		MID domMaterialID = g_MaterialAir;
		MID subMaterialID = g_MaterialAir;
		m_LastHit.HitMaterial[HITOR] = m_Material;
		Material const* hitMaterial = m_LastHit.HitMaterial[HITEE] = g_SceneMan.GetMaterialFromID(hitMaterialID);
		Material const* domMaterial = g_SceneMan.GetMaterialFromID(g_MaterialAir);
		Material const* subMaterial = g_SceneMan.GetMaterialFromID(g_MaterialAir);
		bool hit[2];
		hit[X] = hit[Y] = false;
		m_LastHit.BitmapNormal.Reset();
		Vector hitAcc = m_LastHit.HitVel[HITOR];

		// Check for and react upon a collision in the dominant direction of travel.
		if (m_Delta[m_Dom] && ((m_Dom == X && g_SceneMan.GetTerrMatter(m_HitPos[X], m_IntPos[Y])) || (m_Dom == Y && g_SceneMan.GetTerrMatter(m_IntPos[X], m_HitPos[Y])))) {
			hit[m_Dom] = true;
			domMaterialID = (m_Dom == X) ? g_SceneMan.GetTerrMatter(m_HitPos[X], m_IntPos[Y]) : g_SceneMan.GetTerrMatter(m_IntPos[X], m_HitPos[Y]);
			domMaterial = g_SceneMan.GetMaterialFromID(domMaterialID);

			// Edit the normal accordingly.
			m_LastHit.BitmapNormal[m_Dom] = -m_Increment[m_Dom];
			// Bounce according to the collision.
			hitAcc[m_Dom] = -hitAcc[m_Dom] - hitAcc[m_Dom] * m_Material->GetRestitution() * domMaterial->GetRestitution();
		}

		// Check for and react upon a collision in the submissive direction of travel.
		if (m_SubStepped && m_Delta[m_Sub] && ((m_Sub == X && g_SceneMan.GetTerrMatter(m_HitPos[X], m_IntPos[Y])) || (m_Sub == Y && g_SceneMan.GetTerrMatter(m_IntPos[X], m_HitPos[Y])))) {
			hit[m_Sub] = true;
			subMaterialID = (m_Sub == X) ? g_SceneMan.GetTerrMatter(m_HitPos[X], m_IntPos[Y]) : g_SceneMan.GetTerrMatter(m_IntPos[X], m_HitPos[Y]);
			subMaterial = g_SceneMan.GetMaterialFromID(subMaterialID);

			// Edit the normal accordingly.
			m_LastHit.BitmapNormal[m_Sub] = -m_Increment[m_Sub];
			// Bounce according to the collision.
			hitAcc[m_Sub] = -hitAcc[m_Sub] - hitAcc[m_Sub] * m_Material->GetRestitution() * subMaterial->GetRestitution();
		}

		// If hit right on the corner of a pixel, bounce straight back with no friction.
		if (!hit[m_Dom] && !hit[m_Sub]) {
			// Edit the normal accordingly.
			m_LastHit.BitmapNormal[m_Dom] = -m_Increment[m_Dom];
			m_LastHit.BitmapNormal[m_Sub] = -m_Increment[m_Sub];

			hit[m_Dom] = true;
			hitAcc[m_Dom] = -hitAcc[m_Dom] - hitAcc[m_Dom] * m_Material->GetRestitution() * hitMaterial->GetRestitution();
			hit[m_Sub] = true;
			hitAcc[m_Sub] = -hitAcc[m_Sub] - hitAcc[m_Sub] * m_Material->GetRestitution() * hitMaterial->GetRestitution();
		} else if (hit[m_Dom] && !hit[m_Sub]) {
			// Calculate the effects of friction.
			m_LastHit.BitmapNormal[m_Sub] = -m_Increment[m_Sub] * m_Material->GetFriction() * domMaterial->GetFriction();
			hitAcc[m_Sub] = -hitAcc[m_Sub] * m_Material->GetFriction() * domMaterial->GetFriction();
		} else if (hit[m_Sub] && !hit[m_Dom]) {
			m_LastHit.BitmapNormal[m_Dom] = -m_Increment[m_Dom] * m_Material->GetFriction() * domMaterial->GetFriction();
			hitAcc[m_Dom] = -hitAcc[m_Dom] * m_Material->GetFriction() * subMaterial->GetFriction();
		}
		m_LastHit.BitmapNormal.Normalize();

		// Calculate effects of moment of inertia will have on the impulse.
		float MIhandle = m_LastHit.HitRadius[HITOR].GetPerpendicular().Dot(m_LastHit.BitmapNormal);

		// Calculate the actual impulse force.
		m_LastHit.ResImpulse[HITOR] = hitAcc / ((1.0F / m_LastHit.TotalMass[HITOR]) + (MIhandle * MIhandle / m_LastHit.MomInertia[HITOR]));
		// Scale by the impulse factor.
		m_LastHit.ResImpulse[HITOR] *= m_LastHit.ImpulseFactor[HITOR];

		return m_LastHit;
	}
	RTEAbort("Atom not supposed to do Terrain hit response if it didn't hit anything!");
	return m_LastHit;
}

bool Atom::SetupPos(Vector startPos) {
	CheckpointChange changed(*this, [this] { return CheckpointFields(m_IntPos, m_PrevIntPos, m_TerrainHitsDisabled, m_TerrainMatHit); });
	RTEAssert(m_OwnerMO, "Stepping an Atom without a parent MO!");

	// Only save the previous positions if they are in the scene
	if (m_IntPos[X] > 0 && m_IntPos[Y] > 0) {
		m_PrevIntPos[X] = m_IntPos[X];
		m_PrevIntPos[Y] = m_IntPos[Y];
		m_IntPos[X] = std::floor(startPos.m_X);
		m_IntPos[Y] = std::floor(startPos.m_Y);
	} else {
		m_IntPos[X] = m_PrevIntPos[X] = std::floor(startPos.m_X);
		m_IntPos[Y] = m_PrevIntPos[Y] = std::floor(startPos.m_Y);
	}

	if ((m_TerrainMatHit = g_SceneMan.GetTerrMatter(m_IntPos[X], m_IntPos[Y])) != g_MaterialAir) {
		m_OwnerMO->SetHitWhatTerrMaterial(m_TerrainMatHit);
		if (m_OwnerMO->IntersectionWarning()) {
			m_TerrainHitsDisabled = true;
			if (SceneMan::IsTrackedUID(m_OwnerMO->GetUniqueID())) {
				SceneMan::TraceTerrainEvent("aign", m_IntPos[X], m_IntPos[Y], m_TerrainMatHit, 0, static_cast<int>(m_OwnerMO->GetUniqueID()));
			}
		}
	} else {
		m_TerrainHitsDisabled = false;
	}

	return m_MOIDHit != g_NoMOID || m_TerrainMatHit != g_MaterialAir;
}

int Atom::SetupSeg(Vector startPos, Vector trajectory, float stepRatio) {
	CheckpointChange changed(*this, [this] { return CheckpointFields(m_Delta, m_Delta2, m_Dom, m_DomSteps, m_Error, m_Increment, m_MOIDHit, m_SegProgress, m_SegTraj, m_StepRatio, m_StepWasTaken, m_Sub, m_SubStepped, m_SubSteps, m_TerrainMatHit); });
	RTEAssert(m_OwnerMO, "Stepping an Atom without a parent MO!");
	m_TerrainMatHit = g_MaterialAir;
	m_MOIDHit = g_NoMOID;

	m_StepRatio = stepRatio;
	m_SegProgress = 0.0F;
	m_SegTraj = trajectory;

	// Bresenham's line drawing algorithm preparation
	m_Delta[X] = std::floor(startPos.m_X + trajectory.m_X) - std::floor(startPos.m_X);
	m_Delta[Y] = std::floor(startPos.m_Y + trajectory.m_Y) - std::floor(startPos.m_Y);
	m_DomSteps = 0;
	m_SubSteps = 0;
	m_SubStepped = false;

	if (m_Delta[X] < 0) {
		m_Increment[X] = -1;
		m_Delta[X] = -m_Delta[X];
	} else {
		m_Increment[X] = 1;
	}

	if (m_Delta[Y] < 0) {
		m_Increment[Y] = -1;
		m_Delta[Y] = -m_Delta[Y];
	} else {
		m_Increment[Y] = 1;
	}

	// Scale by 2, for better accuracy of the error at the first pixel
	m_Delta2[X] = m_Delta[X] << 1;
	m_Delta2[Y] = m_Delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (m_Delta[X] > m_Delta[Y]) {
		m_Dom = X;
		m_Sub = Y;
	} else {
		m_Dom = Y;
		m_Sub = X;
	}
	m_Error = m_Delta2[m_Sub] - m_Delta[m_Dom];

	m_DomSteps = 0;
	m_SubSteps = 0;
	m_SubStepped = false;
	m_StepWasTaken = false;

	// Return how many steps there are for this atom to take
	return m_Delta[m_Dom] - m_DomSteps;
}

bool Atom::StepForward() {
	RTEAssert(m_OwnerMO, "Stepping an Atom without a parent MO!");

	// Only take the step if the step ratio permits it
	float prevProgress = m_SegProgress;
	if (m_Delta[m_Dom] && (m_SegProgress += m_StepRatio) >= std::floor(prevProgress + 1.0F)) {
		m_StepWasTaken = true;
		m_MOIDHit = g_NoMOID;
		m_TerrainMatHit = g_MaterialAir;
		bool hitStep = false;

		if (m_DomSteps < m_Delta[m_Dom]) {
			++m_DomSteps;
			if (m_SubStepped) {
				++m_SubSteps;
			}
			m_SubStepped = false;

			m_IntPos[m_Dom] += m_Increment[m_Dom];
			if (m_Error >= 0) {
				m_IntPos[m_Sub] += m_Increment[m_Sub];
				m_SubStepped = true;
				m_Error -= m_Delta2[m_Dom];
			}
			// if (m_ChangedDir){
			m_Error += m_Delta2[m_Sub];
			//} else {
			// m_Error = m_PrevError;
			//}

			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(m_IntPos[X], m_IntPos[Y]);

			// Detect terrain hits, if not disabled.
			if (!m_OwnerMO->m_IgnoreTerrain && g_MaterialAir != (m_TerrainMatHit = g_SceneMan.GetTerrMatter(m_IntPos[X], m_IntPos[Y]))) {
				// Check if we're temporarily disabled from hitting terrain
				if (!m_TerrainHitsDisabled) {
					m_OwnerMO->SetHitWhatTerrMaterial(m_TerrainMatHit);

					m_HitPos[X] = m_IntPos[X];
					m_HitPos[Y] = m_IntPos[Y];
					RTEAssert(m_TerrainMatHit != 0, "Atom returning step with positive hit but without ID stored!");
					hitStep = true;
				}
			} else {
				// Re-enable terrain hits if we are now out of the terrain again
				m_TerrainHitsDisabled = false;
			}

			// Detect hits with non-ignored MO's, if enabled.
			if (m_OwnerMO->m_HitsMOs) {
				m_MOIDHit = g_SceneMan.GetMOIDPixel(m_IntPos[X], m_IntPos[Y], m_OwnerMO->GetTeam());
				if (IsIgnoringMOID(m_MOIDHit)) {
					m_MOIDHit = g_NoMOID;
				}

				if (m_MOIDHit != g_NoMOID) {
					if (!m_MOHitsDisabled) {
						m_HitPos[X] = m_IntPos[X];
						m_HitPos[Y] = m_IntPos[Y];
						RTEAssert(m_MOIDHit != g_NoMOID, "Atom returning step with positive hit but without ID stored!");
						hitStep = true;
						m_OwnerMO->SetHitWhatMOID(m_MOIDHit);
					}
				} else {
					m_MOHitsDisabled = false;
				}
			}
			return hitStep;
		}
		std::string abortString = "Atom shouldn't be taking steps beyond the trajectory!";
		if (m_OwnerMO) {
			abortString += "\nRoot owner is " + m_OwnerMO->GetPresetName() + ".";
			if (m_SubgroupID != 0) {
				const MovableObject* realOwner = g_MovableMan.FindObjectByUniqueID(m_SubgroupID);
				abortString += " Owner is " + realOwner->GetPresetName() + ".";
			}
		}
		abortString += "\n\nDomSteps: " + std::to_string(m_DomSteps) + ", Dominant Direction: " + std::to_string(m_Dom) + ", Delta[Dom]: " + std::to_string(m_Delta[m_Dom]) + ", Trajectory: (" + std::to_string(m_SegTraj.m_X) + ", " + std::to_string(m_SegTraj.m_Y) + ")";
		RTEAbort(abortString);
		m_OwnerMO->SetToDelete();
	}
	m_StepWasTaken = false;
	return 0;
}

void Atom::StepBack() {
	RTEAssert(m_OwnerMO, "Stepping an Atom without a parent MO!");

	// Not complete undo because we lost what these were during last step.
	/*
	m_MOIDHit = g_NoMOID;
	m_TerrainMatHit = g_MaterialAir;
	m_ChangedDir = true;
	m_PrevError = m_Error;
	*/

	m_SegProgress -= m_StepRatio;
	if (m_StepWasTaken) {
		--m_DomSteps;
		m_IntPos[m_Dom] -= m_Increment[m_Dom];
		if (m_SubStepped) {
			--m_SubSteps;
			m_IntPos[m_Sub] -= m_Increment[m_Sub];
		}
		g_SceneMan.WrapPosition(m_IntPos[X], m_IntPos[Y]);
	}
}

int Atom::Travel(float travelTime, bool autoTravel) {
	ZoneScoped;

	if (!m_OwnerMO) {
		RTEAbort("Traveling an Atom without a parent MO!");
		return travelTime;
	}

	Vector& position = m_OwnerMO->m_Pos;
	Vector& velocity = m_OwnerMO->m_Vel;
	float mass = m_OwnerMO->GetMass();
	float sharpness = m_OwnerMO->GetSharpness();
	bool& didWrap = m_OwnerMO->m_DidWrap;
	m_LastHit.Reset();

	BITMAP* trailBitmap = 0;

	int hitCount = 0;
	int error = 0;
	int dom = 0;
	int sub = 0;
	int domSteps = 0;
	int subSteps = 0;

	int intPos[2];
	int hitPos[2];
	int delta[2];
	int delta2[2];
	int increment[2];

	float timeLeft = travelTime;
	float segProgress = 0.0F;
	float retardation;

	bool hit[2];
	bool sinkHit;
	bool subStepped;
	// bool endOfTraj = false;

	const Material* hitMaterial = 0; // g_SceneMan.GetMaterialFromID(g_MaterialAir);
	unsigned char hitMaterialID = 0;

	const Material* domMaterial = 0; // g_SceneMan.GetMaterialFromID(g_MaterialAir);
	unsigned char domMaterialID = 0;

	const Material* subMaterial = 0; // g_SceneMan.GetMaterialFromID(g_MaterialAir);
	unsigned char subMaterialID = 0;

	Vector segTraj;
	Vector hitAccel;

	// Both point sets are archived through the owner, and this is the one write of them that a
	// resting object can still reach.
	if (!m_TrailPoints.empty() || !m_LastTrailPoints.empty()) TouchCheckpoint();
	m_LastTrailPoints = m_TrailPoints;
	m_TrailPoints.clear();

	SceneMan::SetTerrainEventContext(m_OwnerMO ? static_cast<long>(m_OwnerMO->GetUniqueID()) : 0);

	didWrap = false;
	int removeOrphansRadius = m_OwnerMO->m_RemoveOrphanTerrainRadius;
	int removeOrphansMaxArea = m_OwnerMO->m_RemoveOrphanTerrainMaxArea;
	float removeOrphansRate = m_OwnerMO->m_RemoveOrphanTerrainRate;

	// Bake in the Atom offset.
	position += m_Offset;

	// Loop for all the different straight segments (between bounces etc) that have to be traveled during the timeLeft.
	do {
		intPos[X] = std::floor(position.m_X);
		intPos[Y] = std::floor(position.m_Y);

		// Get trail bitmap and put first pixel.
		if (m_TrailLength) {
			m_TrailPoints.push_back({intPos[X], intPos[Y]});
		}
		// Compute and scale the actual on-screen travel trajectory for this segment, based on the velocity, the travel time and the pixels-per-meter constant.
		segTraj = velocity * timeLeft * c_PPM;

		delta[X] = std::floor(position.m_X + segTraj.m_X) - intPos[X];
		delta[Y] = std::floor(position.m_Y + segTraj.m_Y) - intPos[Y];

		// This tends to trigger a lot. It shouldn't, and ought to be properly fixed, but... TODO
		// RTEAssert(std::abs(delta[X]) < 2500 && std::abs(delta[Y] < 2500), "Extremely long difference trajectory found during Atom::Travel. Owner is " + m_OwnerMO->GetPresetName() + ", with Vel (" + std::to_string(velocity.GetX()) + ", " + std::to_string(velocity.GetY()) + ").");

		// segProgress = 0.0F;
		// delta2[X] = 0;
		// delta2[Y] = 0;
		// increment[X] = 0;
		// increment[Y] = 0;
		hit[X] = false;
		hit[Y] = false;
		// domSteps = 0;
		subSteps = 0;
		subStepped = false;
		sinkHit = false;
		hitAccel.Reset();

		if (delta[X] == 0 && delta[Y] == 0) {
			break;
		}

		// HitMaterial->Reset();
		// domMaterial->Reset();
		// subMaterial->Reset();

		// Bresenham's line drawing algorithm preparation
		if (delta[X] < 0) {
			increment[X] = -1;
			delta[X] = -delta[X];
		} else {
			increment[X] = 1;
		}
		if (delta[Y] < 0) {
			increment[Y] = -1;
			delta[Y] = -delta[Y];
		} else {
			increment[Y] = 1;
		}
		// Scale by 2, for better accuracy of the error at the first pixel
		delta2[X] = delta[X] << 1;
		delta2[Y] = delta[Y] << 1;

		// If X is dominant, Y is submissive, and vice versa.
		if (delta[X] > delta[Y]) {
			dom = X;
			sub = Y;
		} else {
			dom = Y;
			sub = X;
		}

		error = m_ChangedDir ? delta2[sub] - delta[dom] : m_PrevError;

		// Bresenham's line drawing algorithm execution
		for (domSteps = 0; domSteps < delta[dom] && !(hit[X] || hit[Y]); ++domSteps) {
			// Check for the special case if the Atom is starting out embedded in terrain. This can happen if something large gets copied to the terrain and embeds some Atoms.
			if (!m_OwnerMO->m_IgnoreTerrain && domSteps == 0 && g_SceneMan.GetTerrMatter(intPos[X], intPos[Y]) != g_MaterialAir) {
				++hitCount;
				hit[X] = hit[Y] = true;
				if (g_SceneMan.TryPenetrate(intPos[X], intPos[Y], velocity * mass * sharpness, velocity, retardation, 0.5F, m_NumPenetrations, removeOrphansRadius, removeOrphansMaxArea, removeOrphansRate)) {
					// segProgress = 0.0F;
					velocity += velocity * retardation;
					continue;
				} else {
					// segProgress = 1.0F;
					velocity.SetXY(0, 0);
					timeLeft = 0.0F;
					break;
				}
			}

			if (subStepped) {
				++subSteps;
			}
			subStepped = false;

			intPos[dom] += increment[dom];
			if (error >= 0) {
				intPos[sub] += increment[sub];
				subStepped = true;
				error -= delta2[dom];
			}
			error += delta2[sub];

			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			///////////////////////////////////////////////////////////////////////////////////////////////////
			// Atom-MO collision detection and response.

			// Detect hits with non-ignored MO's, if enabled.
			m_MOIDHit = m_OwnerMO->m_HitsMOs ? g_SceneMan.GetMOIDPixel(intPos[X], intPos[Y], m_OwnerMO->GetTeam()) : g_NoMOID;
			if (m_MOIDHit != g_NoMOID && !IsIgnoringMOID(m_MOIDHit)) {
				m_OwnerMO->SetHitWhatMOID(m_MOIDHit);

				++hitCount;
				hitPos[X] = intPos[X];
				hitPos[Y] = intPos[Y];

				// Back up so the Atom is not inside the MO.
				intPos[dom] -= increment[dom];
				if (subStepped) {
					intPos[sub] -= increment[sub];
				}

				m_LastHit.Reset();
				m_LastHit.TotalMass[HITOR] = mass;
				// TODO: Is this right? Perhaps should be 0?")
				m_LastHit.MomInertia[HITOR] = 1.0F;
				m_LastHit.ImpulseFactor[HITOR] = 1.0F;
				m_LastHit.ImpulseFactor[HITEE] = 1.0F;
				// m_LastHit.HitPoint = Vector(hitPos[X], hitPos[Y]);
				m_LastHit.HitVel[HITOR] = velocity;

				MovableObject* MO = g_MovableMan.GetMOFromID(m_MOIDHit);

				if (MO) {
					MO->SetHitWhatParticleUniqueID(m_OwnerMO->GetUniqueID());
				}

				m_LastHit.Body[HITOR] = m_OwnerMO;
				m_LastHit.Body[HITEE] = MO;

#ifndef RELEASE_BUILD
				RTEAssert(m_LastHit.Body[HITEE], "Hitee MO is 0 in Atom::Travel!");
				RTEAssert(m_MOIDHit == m_LastHit.Body[HITEE]->GetID(), "g_MovableMan.GetMOFromID messed up in Atom::MOHitResponse!");
#endif

				// Don't do this normal approximation based on object centers, it causes particles to 'slide into' sprite objects when they should be resting on them.
				// Orthogonal normals only, as the pixel boundaries themselves! See further down for the setting of this.
				// m_LastHit.BitmapNormal = m_LastHit.Body[HITOR]->GetPos() - m_LastHit.Body[HITEE]->GetPos();
				// m_LastHit.BitmapNormal.Normalize();

				// Gold special collection case!
				// TODO: Make material IDs more robust!")
				if (m_Material->GetIndex() == c_GoldMaterialID && g_MovableMan.IsOfActor(m_MOIDHit)) {
					if (Actor* actor = dynamic_cast<Actor*>(g_MovableMan.GetMOFromID(m_LastHit.Body[HITEE]->GetRootID())); actor && !actor->IsDead()) {
						actor->AddGold(m_OwnerMO->GetMass() * g_SceneMan.GetOzPerKg() * removeOrphansRadius ? 1.25F : 1.0F);
						m_OwnerMO->SetToDelete(true);
						// This is to break out of the do-while and the function properly.
						m_LastHit.Terminate[HITOR] = hit[dom] = hit[sub] = true;
						break;
					}
				}

				// Check for the collision point in the dominant direction of travel.
				if (delta[dom] && ((dom == X && g_SceneMan.GetMOIDPixel(hitPos[X], intPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID) || (dom == Y && g_SceneMan.GetMOIDPixel(intPos[X], hitPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID))) {
					hit[dom] = true;
					m_LastHit.HitPoint = (dom == X) ? Vector(hitPos[X], intPos[Y]) : Vector(intPos[X], hitPos[Y]);
					m_LastHit.BitmapNormal[dom] = -increment[dom];
				}

				// Check for the collision point in the submissive direction of travel.
				if (subStepped && delta[sub] && ((sub == X && g_SceneMan.GetMOIDPixel(hitPos[X], intPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID) || (sub == Y && g_SceneMan.GetMOIDPixel(intPos[X], hitPos[Y], m_OwnerMO->GetTeam()) != g_NoMOID))) {
					hit[sub] = true;
					if (m_LastHit.HitPoint.IsZero()) {
						m_LastHit.HitPoint = (sub == X) ? Vector(hitPos[X], intPos[Y]) : Vector(intPos[X], hitPos[Y]);
					} else {
						// We hit pixels in both sub and dom directions on the other MO, a corner hit.
						m_LastHit.HitPoint.SetXY(hitPos[X], hitPos[Y]);
						m_LastHit.BitmapNormal[sub] = -increment[sub];
					}
				}

				// If neither the direct dominant or sub directions yielded a collision point, then that means we hit right on the corner edge of a pixel, and that is the collision point.
				if (!hit[dom] && !hit[sub]) {
					hit[dom] = hit[sub] = true;
					m_LastHit.HitPoint.SetXY(hitPos[X], hitPos[Y]);
					m_LastHit.BitmapNormal.SetXY(-increment[X], -increment[Y]);
				}

				// Now normalize the normal in case it's diagonal due to hit in both directions
				m_LastHit.BitmapNormal.Normalize();

				// Make this Atom ignore hits with this MO for the rest of the frame, to avoid erroneous multiple hits because the hit MO doesn't move away until it itself

				// is updated and the impulses produced in this hit are taken into effect.
				AddMOIDToIgnore(m_MOIDHit);

				if (SceneMan::IsTrackedUID(m_OwnerMO->GetUniqueID()) || SceneMan::IsTrackedUID(m_LastHit.Body[HITEE]->GetUniqueID())) {
					SceneMan::TraceTerrainEvent("ghit", std::bit_cast<int32_t>(m_LastHit.HitPoint.m_X), std::bit_cast<int32_t>(m_LastHit.HitPoint.m_Y), static_cast<int>(m_LastHit.Body[HITEE]->GetUniqueID()), std::bit_cast<int32_t>(velocity.m_X), static_cast<int>(m_OwnerMO->GetUniqueID()));
				}
				m_LastHit.Body[HITEE]->CollideAtPoint(m_LastHit);
				hitAccel = m_LastHit.ResImpulse[HITOR] / mass;

				// Report the hit to both MO's in collision
				m_LastHit.RootBody[HITOR] = m_LastHit.Body[HITOR]->GetRootParent();
				m_LastHit.RootBody[HITEE] = m_LastHit.Body[HITEE]->GetRootParent();
				m_LastHit.RootBody[HITOR]->OnMOHit(m_LastHit);
				m_LastHit.RootBody[HITEE]->OnMOHit(m_LastHit);
			}

			///////////////////////////////////////////////////////////////////////////////////////////////////
			// Atom-Terrain collision detection and response.

			// If there was no MO collision detected, then check for terrain hits.
			else if (!m_OwnerMO->m_IgnoreTerrain && (hitMaterialID = g_SceneMan.GetTerrMatter(intPos[X], intPos[Y]))) {
				if (hitMaterialID != g_MaterialAir) {
					m_OwnerMO->SetHitWhatTerrMaterial(hitMaterialID);
				}

				hitMaterial = g_SceneMan.GetMaterialFromID(hitMaterialID);
				hitPos[X] = intPos[X];
				hitPos[Y] = intPos[Y];
				++hitCount;

#ifdef DEBUG_BUILD
				if (m_TrailLength) {
					putpixel(trailBitmap, intPos[X], intPos[Y], 199);
				}
#endif
				// Try penetration of the terrain.
				if (hitMaterial->GetIndex() != g_MaterialOutOfBounds && g_SceneMan.TryPenetrate(intPos[X], intPos[Y], velocity * mass * sharpness, velocity, retardation, 0.65F, m_NumPenetrations, removeOrphansRadius, removeOrphansMaxArea, removeOrphansRate)) {
					hit[dom] = hit[sub] = sinkHit = true;
					++m_NumPenetrations;
					m_ChangedDir = false;
					m_PrevError = error;

					// Calculate the penetration/sink response effects.
					hitAccel = velocity * retardation;
				} else {
					// Penetration failed, bounce.
					m_NumPenetrations = 0;
					m_ChangedDir = true;
					m_PrevError = error;

					// Back up so the Atom is not inside the terrain.
					intPos[dom] -= increment[dom];
					if (subStepped) {
						intPos[sub] -= increment[sub];
					}

					// Undo scene wrapping, if necessary
					g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

					// Check if particle is sticky and should adhere to where it collided
					if (!m_OwnerMO->IsMissionCritical() && velocity.MagnitudeIsGreaterThan(1.0F)) {
						MOPixel* ownerMOAsPixel = dynamic_cast<MOPixel*>(m_OwnerMO);
						if (RandomNum() < std::max(m_Material->GetStickiness(), ownerMOAsPixel ? ownerMOAsPixel->GetStaininess() : 0.0f)) {
							// Weighted random select between stickiness or staininess
							const float randomChoice = RandomNum(0.0f, m_Material->GetStickiness() + (ownerMOAsPixel ? ownerMOAsPixel->GetStaininess() : 0.0f));
							if (randomChoice <= m_Material->GetStickiness()) {
								m_OwnerMO->SetPos(Vector(intPos[X], intPos[Y]));
								m_OwnerMO->DrawToTerrain(g_SceneMan.GetTerrain());
								m_OwnerMO->SetToDelete(true);
								m_LastHit.Terminate[HITOR] = hit[dom] = hit[sub] = true;
								break;
							} else if (MOPixel* ownerMOAsPixel = dynamic_cast<MOPixel*>(m_OwnerMO); ownerMOAsPixel && randomChoice <= m_Material->GetStickiness() + ownerMOAsPixel->GetStaininess()) {
								Vector stickPos(intPos[X], intPos[Y]);
								stickPos += velocity * (c_PPM * g_TimerMan.GetDeltaTimeSecs()) * RandomNum();
								int terrainMaterialID = g_SceneMan.GetTerrain()->GetMaterialPixel(stickPos.GetFloorIntX(), stickPos.GetFloorIntY());
								SceneMan::TraceTerrainEvent("stnr", stickPos.GetFloorIntX(), stickPos.GetFloorIntY(), terrainMaterialID, intPos[X], intPos[Y]);
								if (terrainMaterialID != g_MaterialAir && terrainMaterialID != g_MaterialDoor) {
									m_OwnerMO->SetPos(Vector(stickPos.GetRoundIntX(), stickPos.GetRoundIntY()));
								} else {
									m_OwnerMO->SetPos(Vector(intPos[X], intPos[Y]));
								}
								m_OwnerMO->DrawToTerrain(g_SceneMan.GetTerrain());
								m_OwnerMO->SetToDelete(true);
								m_LastHit.Terminate[HITOR] = hit[dom] = hit[sub] = true;
								break;
							}
						}
					}

					// Check for and react upon a collision in the dominant direction of travel.
					if (delta[dom] && ((dom == X && g_SceneMan.GetTerrMatter(hitPos[X], intPos[Y])) || (dom == Y && g_SceneMan.GetTerrMatter(intPos[X], hitPos[Y])))) {
						hit[dom] = true;
						domMaterialID = (dom == X) ? g_SceneMan.GetTerrMatter(hitPos[X], intPos[Y]) : g_SceneMan.GetTerrMatter(intPos[X], hitPos[Y]);
						domMaterial = g_SceneMan.GetMaterialFromID(domMaterialID);

						// Bounce according to the collision.
						hitAccel[dom] = -velocity[dom] - velocity[dom] * m_Material->GetRestitution() * domMaterial->GetRestitution();
					}

					// Check for and react upon a collision in the submissive direction of travel.
					if (subStepped && delta[sub] && ((sub == X && g_SceneMan.GetTerrMatter(hitPos[X], intPos[Y])) || (sub == Y && g_SceneMan.GetTerrMatter(intPos[X], hitPos[Y])))) {
						hit[sub] = true;
						subMaterialID = (sub == X) ? g_SceneMan.GetTerrMatter(hitPos[X], intPos[Y]) : g_SceneMan.GetTerrMatter(intPos[X], hitPos[Y]);
						subMaterial = g_SceneMan.GetMaterialFromID(subMaterialID);

						// Bounce according to the collision.
						hitAccel[sub] = -velocity[sub] - velocity[sub] * m_Material->GetRestitution() * subMaterial->GetRestitution();
					}

					// If hit right on the corner of a pixel, bounce straight back with no friction.
					if (!hit[dom] && !hit[sub]) {
						hit[dom] = true;
						hitAccel[dom] = -velocity[dom] - velocity[dom] * m_Material->GetRestitution() * hitMaterial->GetRestitution();
						hit[sub] = true;
						hitAccel[sub] = -velocity[sub] - velocity[sub] * m_Material->GetRestitution() * hitMaterial->GetRestitution();
					} else if (hit[dom] && !hit[sub]) {
						// Calculate the effects of friction.
						hitAccel[sub] -= velocity[sub] * m_Material->GetFriction() * domMaterial->GetFriction();
					} else if (hit[sub] && !hit[dom]) {
						hitAccel[dom] -= velocity[dom] * m_Material->GetFriction() * subMaterial->GetFriction();
					}
				}
			} else if (m_TrailLength) {
				m_TrailPoints.push_back({intPos[X], intPos[Y]});
			}

			///////////////////////////////////////////////////////////////////////////////////////////////////
			// Apply Collision Responses

			// If we hit anything, and are about to start a new segment instead of a step, apply the collision response effects to the owning MO.
			if ((hit[X] || hit[Y]) && !m_LastHit.Terminate[HITOR]) {
				// Calculate the progress made on this segment before hitting something.
				// We count the hitting step made if it resulted in a terrain sink, because the Atoms weren't stepped back out of intersection.
				// segProgress = static_cast<float>(domSteps + sinkHit) / static_cast<float>(delta[dom]);
				segProgress = (static_cast<float>(domSteps + static_cast<int>(sinkHit)) < delta[dom]) ? (static_cast<float>(domSteps + static_cast<int>(sinkHit)) / std::fabs(static_cast<float>(segTraj[dom]))) : 1.0F;

				// Now calculate the total time left to travel, according to the progress made.
				timeLeft -= timeLeft * segProgress;

				// Move position forward to the hit position.
				// position += segTraj * segProgress;
				// Only move the dom forward by int domSteps, so we don't cross into a pixel too far
				position[dom] += (domSteps + static_cast<int>(sinkHit)) * increment[dom];

				// Move the submissive direction forward by as many int steps, or the full float segTraj if all sub-steps are clear
				if ((subSteps + static_cast<int>(subStepped && sinkHit)) < delta[sub]) {
					position[sub] += (subSteps + static_cast<int>(subStepped && sinkHit)) * increment[sub];
				} else {
					position[sub] += segTraj[sub];
				}

				Vector testPos = position - m_Offset;

				didWrap = g_SceneMan.WrapPosition(testPos) || didWrap;

				// Apply the collision response acceleration to the linear velocity of the owner MO.
				velocity += hitAccel;
			}
		}
	} while ((hit[X] || hit[Y]) && /* !segTraj.GetFloored().IsZero() && */ hitCount < 100 && !m_LastHit.Terminate[HITOR]);

	// RTEAssert(hitCount < 100, "Atom travel resulted in more than 100 segments!!");

	// Extract Atom offset.
	position -= m_Offset;

	// Travel along the remaining segTraj.
	if (!(hit[X] || hit[Y]) && autoTravel) {
		position += segTraj;
	}

	didWrap = g_SceneMan.WrapPosition(position) || didWrap;

	ClearMOIDIgnoreList();

	return hitCount;
}

void HitData::Clear() {
	HitPoint.Reset();
	VelDiff.Reset();
	BitmapNormal.Reset();
	HitDenominator = 0;

	for (unsigned short i = 0; i < 2; ++i) {
		Body[i] = 0;
		RootBody[i] = 0;
		HitVel[i].Reset();
		TotalMass[i] = 0;
		MomInertia[i] = 0;
		HitRadius[i].Reset();
		HitMaterial[i] = 0;
		PreImpulse[i].Reset();
		ResImpulse[i].Reset();
		ImpulseFactor[i] = 0;
		SquaredMIHandle[i] = 0;
		Terminate[i] = false;
	}
}

HitData& HitData::operator=(const HitData& rhs) {
	if (this == &rhs) {
		return *this;
	}
	Clear();

	HitPoint = rhs.HitPoint;
	VelDiff = rhs.VelDiff;
	BitmapNormal = rhs.BitmapNormal;
	HitDenominator = rhs.HitDenominator;

	for (unsigned short i = 0; i < 2; ++i) {
		Body[i] = rhs.Body[i];
		RootBody[i] = rhs.RootBody[i];
		HitVel[i] = rhs.HitVel[i];
		TotalMass[i] = rhs.TotalMass[i];
		MomInertia[i] = rhs.MomInertia[i];
		HitRadius[i] = rhs.HitRadius[i];
		HitMaterial[i] = rhs.HitMaterial[i];
		PreImpulse[i] = rhs.PreImpulse[i];
		ResImpulse[i] = rhs.ResImpulse[i];
		ImpulseFactor[i] = rhs.ImpulseFactor[i];
		SquaredMIHandle[i] = rhs.SquaredMIHandle[i];
		Terminate[i] = rhs.Terminate[i];
	}
	return *this;
}
