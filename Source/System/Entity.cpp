#include "Entity.h"
#include "CheckpointNativeSnapshot.h"
#include "CaptureSentinel.h"
#include "CheckpointArchive.h"
#include "CheckpointImage.h"
#include "RTETools.h"
#include "PresetMan.h"
#include "DataModule.h"
#include "Base64/base64.h"
#include "MovableObject.h"
#include "SceneMan.h"
#include "BitmapCheckpoint.h"
#include "System.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RTE {
	namespace {
		// Each thread adds to its own totals; a report merges them.
		struct CloneCostTotals {
			std::mutex mutex;
			std::unordered_map<const char*, std::array<int64_t, 3>> kinds;
		};
		struct CloneCostThreads {
			std::mutex mutex;
			std::vector<std::shared_ptr<CloneCostTotals>> threads;
		};
		CloneCostThreads& CloneCostRegistry() {
			static CloneCostThreads* registry = new CloneCostThreads;
			return *registry;
		}
		CloneCostTotals& CloneCosts() {
			thread_local const std::shared_ptr<CloneCostTotals> totals = [] {
				auto created = std::make_shared<CloneCostTotals>();
				std::lock_guard lock(CloneCostRegistry().mutex);
				CloneCostRegistry().threads.push_back(created);
				return created;
			}();
			return *totals;
		}
		thread_local CheckpointCloneCost* s_OpenCloneCost = nullptr;
		int64_t CloneCostNow() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	} // namespace

	bool CheckpointCloneCost::Enabled() {
		static const bool enabled = [] { const char* value = std::getenv("CCCP_CHECKPOINT_PHASES"); return value && std::string_view(value) == "1"; }();
		return enabled;
	}

	void CheckpointCloneCost::Begin() {
		m_Parent = s_OpenCloneCost;
		s_OpenCloneCost = this;
		m_Start = CloneCostNow();
	}

	void CheckpointCloneCost::End() {
		const int64_t inclusive = CloneCostNow() - m_Start;
		s_OpenCloneCost = m_Parent;
		if (m_Parent) m_Parent->m_Children += inclusive;
		auto& totals = CloneCosts();
		std::lock_guard lock(totals.mutex);
		auto& kind = totals.kinds[m_Kind];
		++kind[0]; kind[1] += inclusive; kind[2] += inclusive - m_Children;
	}

	CheckpointCloneCost::Totals CheckpointCloneCost::Take() {
		if (!Enabled()) return {};
		std::unordered_map<const char*, std::array<int64_t, 3>> merged;
		std::lock_guard registryLock(CloneCostRegistry().mutex);
		for (const auto& thread: CloneCostRegistry().threads) {
			std::lock_guard lock(thread->mutex);
			for (const auto& [kind, cost]: thread->kinds) for (size_t index = 0; index < cost.size(); ++index) merged[kind][index] += cost[index];
			thread->kinds.clear();
		}
		return Totals(merged.begin(), merged.end());
	}

	void CheckpointCloneCost::Report(uint64_t tick, Totals totals) {
		std::sort(totals.begin(), totals.end(), [](const auto& left, const auto& right) { return left.second[2] > right.second[2]; });
		for (const auto& [kind, cost]: totals) {
			System::PrintDiagnosticLine(std::format("[checkpoint-clone-cost] tick={} kind={} count={} inclusive_us={} exclusive_us={}", tick, kind, cost[0], cost[1] / 1000, cost[2] / 1000));
		}
	}

	CheckpointNativeSnapshot::CheckpointNativeSnapshot() :
		m_Serial([] { static std::atomic<uint64_t> serials{0}; return ++serials; }()),
		m_Clock{g_TimerMan.GetSimTimeTicks(), g_TimerMan.GetSimUpdateCount(), g_TimerMan.GetRealTickCount()} {
		if (SceneMan::IsConstructed()) g_SceneMan.VisitCheckpointMaterialOwners([this](const Material* material, int kind, size_t index) {
			m_MaterialOwners.try_emplace(material, kind, index);
		});
	}
	void CheckpointNativeSnapshot::RememberMaterial(const Material* source, const Material* target) {
		const auto owner = m_MaterialOwners.find(source);
		if (owner == m_MaterialOwners.end()) return;
		const auto [kind, index] = owner->second;
		m_MaterialReferences.TryEmplace(target, CheckpointWriter::CaptureNative([kind, index] {
			CheckpointWriter writer("MaterialReference1"); writer(kind, index); return writer.Text();
		}));
	}
	const CheckpointText* CheckpointNativeSnapshot::MaterialReference(const Material* target) const {
		return m_MaterialReferences.FindStored(target);
	}
	void CheckpointNativeSnapshot::RememberUID(const MovableObject* source, MovableObject* target) {
		if (source->GetUniqueID() > 0 && g_MovableMan.FindObjectByUniqueID(source->GetUniqueID()) == source) m_UIDs.TryEmplace(source->GetUniqueID(), target);
	}
	MovableObject* CheckpointNativeSnapshot::FindUID(long uid) const {
		return m_UIDs.Find(uid).value_or(nullptr);
	}
	struct CheckpointNativeSnapshot::Pixel {
		BITMAP bitmap{};
		GFX_VTABLE table{};
		mutable std::vector<uint8_t*> lines;
		std::shared_ptr<const BitmapSnapshot> snapshot;
		const BITMAP* loaded = nullptr;
		size_t rowBytes = 0;
		CheckpointText text;
		std::array<std::optional<std::string>, 2> paths;
		mutable std::once_flag ready;
		mutable std::string bytes;
		static std::string Rows(const BITMAP* source, size_t rowBytes) {
			std::string rows;
			rows.reserve(rowBytes * static_cast<size_t>(source->h));
			for (int row = 0; row < source->h; ++row) rows.append(reinterpret_cast<const char*>(source->line[row]), rowBytes);
			return rows;
		}
		void Materialize() const {
			std::call_once(ready, [this] {
				bytes = loaded ? Rows(loaded, rowBytes) : snapshot->PixelBytes();
				const size_t stride = loaded ? rowBytes : snapshot->rowBytes;
				for (size_t row = 0; row < lines.size(); ++row) lines[row] = reinterpret_cast<uint8_t*>(bytes.data()) + row * stride;
			});
		}
	};
	BITMAP* CheckpointNativeSnapshot::Freeze(BITMAP* source) {
		if (!source) return nullptr;
		auto& recent = Recent<1>(source);
		if (recent.first == source) return static_cast<BITMAP*>(recent.second);
		if (const auto* known = m_BitmapSources.FindStored(source)) {
			recent = {source, &(*known)->bitmap};
			return &(*known)->bitmap;
		}
		CheckpointCloneCost cost("BITMAP");
		auto pixel = std::make_shared<Pixel>();
		pixel->bitmap = *source; pixel->table = *source->vtable;
		pixel->bitmap.vtable = &pixel->table;
		pixel->lines.resize(source->h);
		pixel->bitmap.line = pixel->lines.data();
		pixel->bitmap.dat = nullptr; pixel->bitmap.extra = nullptr;
		for (int depth = 0; depth < 2; ++depth) {
			int requested = depth;
			if (const auto* path = ContentFile::LoadedBitmapPath(source, requested)) pixel->paths[depth] = *path;
		}
		const int depth = bitmap_color_depth(source);
		if ((pixel->paths[0] || pixel->paths[1]) && source->w > 0 && source->h > 0 && (depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32)) {
			// A loaded image keeps its pixels while it is loaded (a save names its file and a load refuses other pixels), so the saver reads it.
			pixel->loaded = source;
			pixel->rowBytes = static_cast<size_t>(source->w) * ((depth + 7) / 8);
			pixel->text = CheckpointText::Deferred([source, rowBytes = pixel->rowBytes] { return Pixel::Rows(source, rowBytes); }, pixel->rowBytes * static_cast<size_t>(source->h));
		} else if (auto captured = BitmapPixelCaptureScope::Capture(source, {})) { pixel->snapshot = captured->first; pixel->text = captured->second; }
		else {
			pixel->snapshot = BitmapSnapshot::Freeze(source);
			pixel->text = CheckpointText::Deferred([snapshot = pixel->snapshot] { return snapshot->PixelBytes(); }, pixel->snapshot->LogicalBytes());
		}
		// Another thread freezing an owner of the same image may have frozen it first; its copy is the one kept.
		const auto [kept, claimed] = m_BitmapSources.TryEmplace(source, pixel);
		if (claimed) m_Bitmaps.InsertOrAssign(&pixel->bitmap, pixel);
		recent = {source, &kept->bitmap};
		return &kept->bitmap;
	}
	std::optional<std::pair<std::shared_ptr<const BitmapSnapshot>, CheckpointText>> CheckpointNativeSnapshot::Pixels(const BITMAP* bitmap) const {
		const auto found = m_Bitmaps.Find(bitmap);
		if (!found) return {};
		return std::pair{(*found)->snapshot, (*found)->text};
	}
	std::optional<const std::string*> CheckpointNativeSnapshot::BitmapPath(const BITMAP* bitmap, int& depth) const {
		const auto found = m_Bitmaps.Find(bitmap);
		if (!found) return {};
		const auto& paths = (*found)->paths;
		if (depth < 0) for (size_t index = 0; index < paths.size(); ++index) if (paths[index]) { depth = static_cast<int>(index); return &*paths[index]; }
		if (depth >= 0 && static_cast<size_t>(depth) < paths.size() && paths[depth]) return &*paths[depth];
		return static_cast<const std::string*>(nullptr);
	}
	void CheckpointNativeSnapshot::MaterializePixels() const {
		m_Bitmaps.ForEach([](const BITMAP*, const std::shared_ptr<Pixel>& pixel) { pixel->Materialize(); });
	}
	void* CheckpointNativeSnapshot::AllocateFrozen(size_t bytes, size_t alignment) {
		static constexpr size_t c_StorageBytes = size_t{256} << 10;
		if (bytes > c_StorageBytes / 8 || alignment > alignof(std::max_align_t)) return nullptr;
		struct Cursor { uint64_t snapshot = 0; uintptr_t at = 0, end = 0; };
		thread_local Cursor cursor;
		if (cursor.snapshot != m_Serial) cursor = {m_Serial, 0, 0};
		uintptr_t at = (cursor.at + alignment - 1) & ~(alignment - 1);
		if (!cursor.at || at + bytes > cursor.end) {
			std::shared_ptr<void> storage = CheckpointBuffer::AllocateCaptureBytes(c_StorageBytes);
			const uintptr_t base = reinterpret_cast<uintptr_t>(storage.get());
			{
				OwnerShard& shard = Owners();
				std::lock_guard lock(shard.mutex);
				shard.storage.push_back(std::move(storage));
			}
			cursor.end = base + c_StorageBytes;
			at = (base + alignment - 1) & ~(alignment - 1);
		}
		cursor.at = at + bytes;
		return reinterpret_cast<void*>(at);
	}
	CheckpointText CheckpointNativeSnapshot::FreezeWriter(const Serializable* source) {
		if (const auto known = m_WriterValues.Find(source)) return *known;
		CheckpointCloneCost cost("Writer text");
		CheckpointWriter::BatchOverride ordinary(false);
		CheckpointWriter::CacheScope uncached(nullptr);
		auto values = Writer::Capture([source](Writer& writer) { writer << source; }, 1);
		return m_WriterValues.TryEmplace(source, std::move(values)).first;
	}
	CheckpointNativeSnapshot::~CheckpointNativeSnapshot() {
		// A freeze that stopped early leaves claimed storage that was never constructed.
		m_Reserved.ForEach([](const Entity*, const Reservation& reserved) { reserved.type->DeallocateCheckpointMemory(reserved.memory); });
		for (auto& shard: m_OwnerShards) for (auto& object: shard.owners) if (Entity* value = std::exchange(object, nullptr)) delete value;
		for (auto& shard: m_OwnerShards) for (auto& [value, destroy]: shard.values) if (value) destroy(value);
	}

	Entity::Entity(const Entity& source, CheckpointNativeSnapshot& snapshot) :
		m_PresetName(source.m_PresetName), m_CopiedFromPresetName(source.m_CopiedFromPresetName),
		m_PresetDescription(source.m_PresetDescription), m_FormattedReaderPosition(source.m_FormattedReaderPosition),
		m_IsOriginalPreset(source.m_IsOriginalPreset), m_DefinedInModule(source.m_DefinedInModule),
		m_Groups(source.m_Groups), m_RandomWeight(source.m_RandomWeight),
		m_CheckpointWriteGeneration(source.m_CheckpointWriteGeneration), m_FrozenCheckpointNative(true), m_CheckpointSnapshot(&snapshot),
		m_CheckpointModuleAndPreset(source.GetModuleAndPresetName()) {
		m_CheckpointOwnerSlot = snapshot.Bind(source, this);
		const Entity* preset = nullptr;
		{
			CheckpointCloneCost cost("preset lookup");
			preset = source.GetPresetForCopy();
		}
		m_CheckpointPreset = snapshot.Object(preset);
	}
	Entity* Entity::FreezeCheckpointNative(CheckpointNativeSnapshot&) const {
		throw UnsupportedCheckpointNative("native checkpoint snapshot is not implemented for " + GetClassName());
	}
	void CheckpointNativeSnapshot::AssignEntity(Entity& target, const Entity& source) {
		target.m_FrozenCheckpointNative = true;
		target.m_CheckpointSnapshot = this;
		target.m_CheckpointOwnerSlot = Bind(source, &target);
		target.m_PresetName = source.m_PresetName;
		target.m_CopiedFromPresetName = source.m_CopiedFromPresetName;
		target.m_PresetDescription = source.m_PresetDescription;
		target.m_FormattedReaderPosition = source.m_FormattedReaderPosition;
		target.m_IsOriginalPreset = source.m_IsOriginalPreset;
		target.m_DefinedInModule = source.m_DefinedInModule;
		target.m_Groups = source.m_Groups;
		target.m_RandomWeight = source.m_RandomWeight;
		target.m_CheckpointWriteGeneration = source.m_CheckpointWriteGeneration;
		target.m_CheckpointModuleAndPreset = source.GetModuleAndPresetName();
		target.m_CheckpointPreset = Object(source.GetPresetForCopy());
	}
	thread_local unsigned int Entity::s_CheckpointCloneDepth = 0;
	thread_local void* Entity::s_DeletedCheckpointMemory = nullptr;
	bool Entity::IsCheckpointClone() { return s_CheckpointCloneDepth != 0 || MovableObject::IsFaithfulClone(); }

	void Entity::ReportCheckpointValueWrite() {
		m_CheckpointValueTrap = false;
		CheckpointValueWritten(this);
	}

	std::string Entity::SaveCheckpoint() const {
		CheckpointWriter archive("Entity1");
		archive(m_PresetName, m_CopiedFromPresetName, m_PresetDescription, m_FormattedReaderPosition, m_IsOriginalPreset, m_DefinedInModule, m_RandomWeight);
		archive(std::set<std::string>(m_Groups.begin(), m_Groups.end()));
		return archive.Text();
	}

	bool Entity::LoadCheckpoint(std::string_view text, bool validateOnly) {
		try {
			CheckpointReader archive(text, "Entity1", validateOnly);
			archive(m_PresetName, m_CopiedFromPresetName, m_PresetDescription, m_FormattedReaderPosition, m_IsOriginalPreset, m_DefinedInModule, m_RandomWeight);
			std::set<std::string> groups;
			archive.Value(groups);
			archive.OnCommit([this, groups = std::move(groups)] { m_Groups.clear(); m_Groups.insert(groups.begin(), groups.end()); });
			archive.Finish();
			return true;
		} catch (const std::exception&) { return false; }
	}

	Entity::ClassInfo Entity::m_sClass("Entity");
	Entity::ClassInfo* Entity::ClassInfo::s_ClassHead = 0;

	Entity::Entity() {
		Clear();
		CaptureSentinel::NoteCreation("Entity", this);
	}

	Entity::~Entity() {
		if (m_FrozenCheckpointNative) {
			if (m_CheckpointOwnerSlot) *m_CheckpointOwnerSlot = nullptr;
			if (m_CheckpointAllocation) s_DeletedCheckpointMemory = m_CheckpointAllocation;
			return;
		}
		Destroy(true);
	}

	void Entity::Clear() {
		m_PresetName = "None";
		m_CopiedFromPresetName.clear();
		m_IsOriginalPreset = false;
		m_DefinedInModule = -1;
		m_PresetDescription.clear();
		m_Groups.clear();
		m_RandomWeight = 100;
		m_CheckpointWriteGeneration = 0;
	}

	int Entity::Create() {
		return 0;
	}

	int Entity::Create(const Entity& reference) {
		m_PresetName = reference.m_PresetName;
		m_CopiedFromPresetName = reference.m_IsOriginalPreset ? reference.m_PresetName : reference.m_CopiedFromPresetName;
		// Note how m_IsOriginalPreset is NOT assigned, automatically indicating that the copy is not an original Preset!
		m_DefinedInModule = reference.m_DefinedInModule;
		m_PresetDescription = reference.m_PresetDescription;

		for (const std::string& group: reference.m_Groups) {
			m_Groups.emplace(group);
		}
		m_RandomWeight = reference.m_RandomWeight;
		m_CheckpointWriteGeneration = 0;
		return 0;
	}

	int Entity::ReadProperty(const std::string_view& propName, Reader& reader) {
		StartPropertyList(
		    // Search for a property name match failed!
		    // TODO: write this out to some log file
		    return Serializable::ReadProperty(propName, reader););

		MatchProperty("CopyOf", {
			std::string refName = reader.ReadPropValue();
			if (refName != "None") {
				std::string className = GetClassName();
				const Entity* preset = g_PresetMan.GetEntityPreset(className, refName, reader.GetReadModuleID());
				if (preset) {
					preset->Clone(this);
				} else {
					reader.ReportError("Couldn't find the preset \"" + refName + "\" of type \"" + className + "\" when trying to do CopyOf.");
				}
			}
		});
		MatchForwards("PresetName") MatchProperty("InstanceName", {
			SetPresetName(reader.ReadPropValue());
			// Preset name might have "[ModuleName]/" preceding it, detect it here and select proper module!
			size_t slashPos = m_PresetName.find_first_of('/');
			if (slashPos != std::string::npos) {
				m_PresetName = m_PresetName.substr(slashPos + 1);
			}
			// Mark this so that the derived class knows it should be added to the PresetMan when it's done reading all properties.
			m_IsOriginalPreset = true;
			// Indicate where this was read from
			m_DefinedInModule = reader.GetReadModuleID();
		});
		MatchProperty("Description", {
			std::string descriptionValue = reader.ReadPropValue();
			if (descriptionValue == "MultiLineText") {
				m_PresetDescription.clear();
				while (reader.NextProperty() && reader.ReadPropName() == "AddLine") {
					m_PresetDescription += reader.ReadPropValue() + "\n\n";
				}
				if (!m_PresetDescription.empty()) {
					m_PresetDescription.resize(m_PresetDescription.size() - 2);
				}
			} else {
				m_PresetDescription = descriptionValue;
			}
		});
		MatchProperty("SpecialBehaviour_PresetName", {
			const std::string value = reader.ReadPropValue();
			m_PresetName = value == "~" ? "" : base64_decode(value);
		});
		MatchProperty("SpecialBehaviour_Description", {
			const std::string value = reader.ReadPropValue();
			m_PresetDescription = value == "~" ? "" : base64_decode(value);
		});
		MatchProperty("SpecialBehaviour_ModuleID", { reader >> m_DefinedInModule; });
		MatchProperty("SpecialBehaviour_ClearGroups", {
			bool clear;
			reader >> clear;
			if (clear) { m_Groups.clear(); }
		});
		MatchProperty("RandomWeight", {
			reader >> m_RandomWeight;
			m_RandomWeight = Limit(m_RandomWeight, 100, 0);
		});
		MatchProperty("AddToGroup", {
			std::string newGroup;
			reader >> newGroup;
			AddToGroup(newGroup);
			// Do this in AddToGroup instead?
			g_PresetMan.RegisterGroup(newGroup, reader.GetReadModuleID());
		});

		EndPropertyList;
	}

	int Entity::Save(Writer& writer) const {
		Serializable::Save(writer);
		const auto* identity = writer.IdentityOverride(this);

		// Is an original preset definition
		if (identity ? identity->original : m_IsOriginalPreset) {
			writer.NewPropertyWithValue("PresetName", identity ? identity->name : m_PresetName);

			if (!m_PresetDescription.empty()) {
				writer.NewPropertyWithValue("Description", m_PresetDescription);
			}
			// Only write out a copy reference if there is one
		} else if (const Entity* preset = GetPresetForCopy()) {
			writer.NewPropertyWithValue("CopyOf", preset->GetModuleAndPresetName());
		}
		if (writer.IsSnapshot()) SaveSnapshotIdentity(writer);

		// TODO: Make proper save system that knows not to save redundant data!
		/*
		for (auto itr = m_Groups.begin(); itr != m_Groups.end(); ++itr) {
		    writer.NewPropertyWithValue("AddToGroup", *itr);
		}
		*/
		return 0;
	}

	int Entity::SavePresetCopy(Writer& writer) const {
		// Can only save out copies with this
		if (m_IsOriginalPreset && !writer.IdentityOverride(this)) {
			RTEAbort("Tried to save out a pure Preset Copy Reference from an original Preset!");
			return -1;
		}
		writer.ObjectStart(GetClassName());
		writer.NewPropertyWithValue("CopyOf", GetModuleAndPresetName());
		writer.ObjectEnd();

		return 0;
	}

	const Entity* Entity::GetPreset() const {
		return g_PresetMan.GetEntityPreset(GetClassName(), GetPresetName(), m_DefinedInModule);
	}

	const Entity* Entity::GetPresetForCopy() const {
		if (m_FrozenCheckpointNative) return m_CheckpointPreset;
		const std::string& name = m_IsOriginalPreset || m_CopiedFromPresetName.empty() ? m_PresetName : m_CopiedFromPresetName;
		if (CheckpointWriter::BatchEnabled() && !name.empty() && name != "None") {
			if (auto* cache = CheckpointWriter::CurrentCache()) return cache->FindPreset(GetClassName(), name, m_DefinedInModule);
		}
		return name.empty() || name == "None" ? nullptr : g_PresetMan.GetEntityPreset(GetClassName(), name, m_DefinedInModule);
	}

	void Entity::SaveSnapshotIdentity(Writer& writer) const {
		const auto* identity = writer.IdentityOverride(this);
		const auto& name = identity ? identity->name : m_PresetName;
		if (writer.IsCapturing() && CheckpointWriter::BatchEnabled()) {
			struct Fields {
				std::string name, description;
				int module;
				std::vector<std::string> groups;
			};
			Fields fields{name, m_PresetDescription, identity ? identity->module : m_DefinedInModule,
			    {m_Groups.begin(), m_Groups.end()}};
			size_t bytes = sizeof(fields) + fields.name.size() + fields.description.size() + fields.groups.size() * sizeof(std::string);
			for (const std::string& group: fields.groups) bytes += group.size();
			const int indent = writer.GetIndent();
			writer.Append(CheckpointText::Deferred([fields = std::move(fields), indent]() mutable {
				std::sort(fields.groups.begin(), fields.groups.end());
				return Writer::Capture([&](Writer& output) {
					output.NewPropertyWithValue("SpecialBehaviour_PresetName", fields.name.empty() ? CheckpointText("~") : CheckpointText(fields.name).Base64());
					output.NewPropertyWithValue("SpecialBehaviour_Description", fields.description.empty() ? CheckpointText("~") : CheckpointText(fields.description).Base64());
					output.NewPropertyWithValue("SpecialBehaviour_ModuleID", fields.module);
					output.NewPropertyWithValue("SpecialBehaviour_ClearGroups", true);
					for (const std::string& group: fields.groups) output.NewPropertyWithValue("AddToGroup", group);
				}, indent).Text();
			}, bytes));
			return;
		}
		writer.NewPropertyWithValue("SpecialBehaviour_PresetName", name.empty() ? CheckpointText("~") : CheckpointText(name).Base64());
		writer.NewPropertyWithValue("SpecialBehaviour_Description", m_PresetDescription.empty() ? CheckpointText("~") : CheckpointText(m_PresetDescription).Base64());
		writer.NewPropertyWithValue("SpecialBehaviour_ModuleID", identity ? identity->module : m_DefinedInModule);
		writer.NewPropertyWithValue("SpecialBehaviour_ClearGroups", true);
		std::vector<std::string> groups(m_Groups.begin(), m_Groups.end());
		std::sort(groups.begin(), groups.end());
		for (const std::string& group: groups) {
			writer.NewPropertyWithValue("AddToGroup", group);
		}
	}

	std::string Entity::GetModuleAndPresetName() const {
		if (m_FrozenCheckpointNative) return m_CheckpointModuleAndPreset;
		if (m_DefinedInModule < 0) {
			return GetPresetName();
		}
		const DataModule* dataModule = g_PresetMan.GetDataModule(m_DefinedInModule);

		if (!dataModule) {
			return GetPresetName();
		}
		return dataModule->GetFileName() + "/" + GetPresetName();
	}

	std::string Entity::GetModuleName() const {
		if (m_DefinedInModule >= 0) {
			if (const DataModule* dataModule = g_PresetMan.GetDataModule(m_DefinedInModule)) {
				return dataModule->GetFileName();
			}
		}
		return "";
	}

	bool Entity::MigrateToModule(int whichModule) {
		if (m_DefinedInModule == whichModule) {
			return false;
		}
		m_IsOriginalPreset = true; // This now a unique snowflake
		m_DefinedInModule = whichModule;
		return true;
	}

	Reader& operator>>(Reader& reader, Entity& operand) {
		// Get this before reading Entity, since if it's the last one in its datafile, the stream will show the parent file instead
		std::string objectFilePath = reader.GetCurrentFilePath();
		// Read the Entity from the file and try to add it to PresetMan
		operand.Create(reader);
		if (!reader.IsCheckpoint()) g_PresetMan.AddEntityPreset(&operand, reader.GetReadModuleID(), reader.GetPresetOverwriting(), objectFilePath);

		return reader;
	}

	Reader& operator>>(Reader& reader, Entity* operand) {
		if (operand) {
			// Get this before reading Entity, since if it's the last one in its datafile, the stream will show the parent file instead
			std::string objectFilePath = reader.GetCurrentFilePath();
			// Read the Entity from the file and try to add it to PresetMan
			operand->Create(reader);
			if (!reader.IsCheckpoint()) g_PresetMan.AddEntityPreset(operand, reader.GetReadModuleID(), reader.GetPresetOverwriting(), objectFilePath);
		} else {
			reader.ReportError("Tried to read an .ini file into a null Entity pointer!");
		}
		return reader;
	}

	Entity::ClassInfo::ClassInfo(const std::string& name, ClassInfo* parentInfo, MemoryAllocate allocFunc, MemoryDeallocate deallocFunc, Entity* (*newFunc)(), int allocBlockCount) :
	    m_Name(name),
	    m_ParentInfo(parentInfo),
	    m_Allocate(allocFunc),
	    m_Deallocate(deallocFunc),
	    m_NewInstance(newFunc),
	    m_NextClass(s_ClassHead) {
		s_ClassHead = this;

		m_AllocatedPool.clear();
		m_PoolAllocBlockCount = (allocBlockCount > 0) ? allocBlockCount : 10;
	}

	Entity::ClassInfo::~ClassInfo() {
		for (void* memory: m_AllocatedPool) m_Deallocate(memory);
	}

	std::list<std::string> Entity::ClassInfo::GetClassNames() {
		std::list<std::string> retList;
		for (const ClassInfo* itr = s_ClassHead; itr != 0; itr = itr->m_NextClass) {
			retList.push_back(itr->GetName());
		}
		return retList;
	}

	const Entity::ClassInfo* Entity::ClassInfo::GetClass(const std::string& name) {
		if (name.empty() || name == "None") {
			return 0;
		}
		for (const ClassInfo* itr = s_ClassHead; itr != 0; itr = itr->m_NextClass) {
			if (itr->GetName() == name) {
				return itr;
			}
		}
		return 0;
	}

	void Entity::ClassInfo::FillAllPools(int fillAmount) {
		for (ClassInfo* itr = s_ClassHead; itr != 0; itr = itr->m_NextClass) {
			if (itr->IsConcrete()) {
				itr->FillPool(fillAmount);
			}
		}
	}

	void Entity::ClassInfo::FillPool(int fillAmount) {
#ifdef __SANITIZE_ADDRESS__
		// If we have ASan, make this a no-op.
		(void)(fillAmount); // Silence warning about unused variable.
#else

		// Default to the set block allocation size if fillAmount is 0
		if (fillAmount <= 0) {
			fillAmount = m_PoolAllocBlockCount;
		}

		// If concrete class, fill up the pool with pre-allocated memory blocks the size of the type
		if (m_Allocate && fillAmount > 0) {
			for (int i = 0; i < fillAmount; ++i) {
				m_AllocatedPool.push_back(m_Allocate());
			}
		}
#endif
	}

	bool Entity::ClassInfo::IsClassOrChildClassOf(const ClassInfo* classInfoToCheck) const {
		if (GetName() == classInfoToCheck->GetName()) {
			return true;
		} else if (m_ParentInfo) {
			return m_ParentInfo->IsClassOrChildClassOf(classInfoToCheck);
		}
		return false;
	}

	void* Entity::ClassInfo::GetPoolMemory() {
#ifdef __SANITIZE_ADDRESS__
		// If compiled with ASan, sidestep pooling and just use the allocator normally.

		void* foundMemory = m_Allocate();
		RTEAssert(foundMemory, "m_Allocate failed! to make memory!");
#else

		std::lock_guard<std::mutex> guard(m_Mutex);

		RTEAssert(IsConcrete(), "Trying to get pool memory of an abstract Entity class!");

		// If the pool is empty, then fill it up again with as many instances as we are set to
		if (m_AllocatedPool.empty()) {
			FillPool((m_PoolAllocBlockCount > 0) ? m_PoolAllocBlockCount : 10);
		}

		// Get the instance in the top of the pool and pop it off
		void* foundMemory = m_AllocatedPool.back();
		m_AllocatedPool.pop_back();

		RTEAssert(foundMemory, "Could not find an available instance in the pool, even after increasing its size!");
#endif

		// Keep track of the number of instances passed out
		m_InstancesInUse++;

		return foundMemory;
	}

	int Entity::ClassInfo::ReturnPoolMemory(void* returnedMemory) {
		if (!returnedMemory) {
			return 0;
		}
		if (s_DeletedCheckpointMemory == returnedMemory) {
			s_DeletedCheckpointMemory = nullptr;
			m_Deallocate(returnedMemory);
			return 0;
		}
		// A frozen object built in its snapshot's storage gives nothing back; the snapshot frees the storage.
		if (s_DeletedCheckpointMemory == CheckpointNativeSnapshot::FrozenStorageMark(returnedMemory)) {
			s_DeletedCheckpointMemory = nullptr;
			return 0;
		}

#ifdef __SANITIZE_ADDRESS__
		// If compiled with ASan, sidestep pooling and just use the allocator normally.
		m_Deallocate(returnedMemory);
#else
		std::lock_guard<std::mutex> guard(m_Mutex);
		try { m_AllocatedPool.push_back(returnedMemory); }
		catch (const std::bad_alloc&) { m_Deallocate(returnedMemory); }
#endif

		// Keep track of the number of instances passed in
		m_InstancesInUse--;

		return m_InstancesInUse;
	}

	void* Entity::ClassInfo::AllocateCheckpointMemory() {
		if (!IsConcrete()) throw std::runtime_error("cannot allocate an abstract native checkpoint value");
		void* memory = m_Allocate();
		if (!memory) throw std::bad_alloc();
		return memory;
	}

	void Entity::ClassInfo::DumpPoolMemoryInfo(const Writer& fileWriter) {
		for (const ClassInfo* itr = s_ClassHead; itr != nullptr; itr = itr->m_NextClass) {
			if (itr->IsConcrete()) {
				fileWriter.NewLineString(itr->GetName() + ": " + std::to_string(itr->m_InstancesInUse), false);
			}
		}
	}
} // namespace RTE
