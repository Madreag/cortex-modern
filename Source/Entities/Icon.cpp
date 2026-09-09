#include "Icon.h"
#include "CheckpointArchive.h"
#include "GUICheckpoint.h"

using namespace RTE;

ConcreteClassInfo(Icon, Entity, 80);

Icon::Icon() {
	Clear();
}

Icon::Icon(const Icon& reference) {
	if (this != &reference) {
		Clear();
		Create(reference);
	}
}

Icon::~Icon() {
	Destroy(true);
}

void Icon::Clear() {
	m_BitmapFile.Reset();
	m_FrameCount = 0;
	m_BitmapsIndexed.clear();
	m_BitmapsTrueColor.clear();
	m_CheckpointBitmapOwners.clear();
}

int Icon::Create() {
	if (m_BitmapsIndexed.empty() || m_BitmapsTrueColor.empty()) {
		if (m_BitmapFile.GetDataPath().empty()) {
			m_BitmapFile.SetDataPath("Base.rte/GUIs/DefaultIcon.png");
		}

		m_BitmapFile.GetAsAnimation(m_BitmapsIndexed, m_FrameCount, COLORCONV_REDUCE_TO_256);
		m_BitmapFile.GetAsAnimation(m_BitmapsTrueColor, m_FrameCount, COLORCONV_8_TO_32);
	}
	return 0;
}

int Icon::Create(const Icon& reference) {
	Entity::Create(reference);

	m_BitmapFile = reference.m_BitmapFile;
	m_FrameCount = reference.m_FrameCount;
	m_BitmapsIndexed = reference.m_BitmapsIndexed;
	m_BitmapsTrueColor = reference.m_BitmapsTrueColor;
	m_CheckpointBitmapOwners = reference.m_CheckpointBitmapOwners;

	return 0;
}

int Icon::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Entity::ReadProperty(propName, reader));

	MatchProperty("BitmapFile", { reader >> m_BitmapFile; });
	MatchProperty("FrameCount", { reader >> m_FrameCount; });

	EndPropertyList;
}

int Icon::Save(Writer& writer) const {
	Entity::Save(writer);
	writer.NewProperty("BitmapFile");
	writer << m_BitmapFile;
	writer.NewProperty("FrameCount");
	writer << m_FrameCount;

	return 0;
}

void Icon::Destroy(bool notInherited) {
	if (!notInherited) {
		Entity::Destroy();
	}
	Clear();
}

std::string Icon::SaveCheckpoint() const {
	CheckpointWriter writer("Icon1");
	writer(Entity::SaveCheckpoint(), m_BitmapFile, m_FrameCount);
	std::vector<BITMAP*> images;
	std::unordered_map<BITMAP*, size_t> indices;
	const auto references = [&](const std::vector<BITMAP*>& bitmaps) {
		std::vector<size_t> result;
		for (BITMAP* bitmap: bitmaps) {
			if (!bitmap) { result.push_back(0); continue; }
			auto [found, inserted] = indices.emplace(bitmap, images.size() + 1);
			if (inserted) images.push_back(bitmap);
			result.push_back(found->second);
		}
		return result;
	};
	const auto indexed = references(m_BitmapsIndexed);
	const auto trueColor = references(m_BitmapsTrueColor);
	writer(images.size());
	for (BITMAP* image: images) writer(GUICheckpoint::SaveSharedBitmap(image));
	writer(indexed, trueColor);
	return writer.Text();
}

bool Icon::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "Icon1", true);
		std::string base, file; unsigned int frames;
		reader.Value(base); reader.Value(file); reader.Value(frames);
		std::vector<std::string> images;
		std::vector<size_t> indexed, trueColor;
		reader.Value(images); reader.Value(indexed); reader.Value(trueColor);
		reader.Finish();
		CheckpointWriter value("IconValues1"); value(base, file, frames, indexed, trueColor);
		CheckpointWriter group("IconSet1"); group(images, std::vector<std::string>{value.Text()});
		return LoadCheckpointSet(group.Text(), {this, 1}, validateOnly);
	} catch (const std::exception&) { return false; }
}

std::string Icon::SaveCheckpointSet(std::span<const Icon> icons) {
	std::vector<std::string> values, images;
	std::unordered_map<BITMAP*, size_t> indices;
	const auto references = [&](const std::vector<BITMAP*>& bitmaps) {
		std::vector<size_t> result;
		for (BITMAP* bitmap: bitmaps) {
			if (!bitmap) { result.push_back(0); continue; }
			auto [found, inserted] = indices.emplace(bitmap, images.size() + 1);
			if (inserted) images.push_back(GUICheckpoint::SaveSharedBitmap(bitmap));
			result.push_back(found->second);
		}
		return result;
	};
	for (const Icon& icon: icons) {
		const auto indexed = references(icon.m_BitmapsIndexed);
		const auto trueColor = references(icon.m_BitmapsTrueColor);
		CheckpointWriter value("IconValues1");
		value(icon.Entity::SaveCheckpoint(), icon.m_BitmapFile, icon.m_FrameCount, indexed, trueColor);
		values.push_back(value.Text());
	}
	CheckpointWriter writer("IconSet1");
	writer(images, values);
	return writer.Text();
}

bool Icon::LoadCheckpointSet(std::string_view text, std::span<Icon> icons, bool validateOnly) {
	try {
		auto apply = PrepareCheckpointSet(text, icons, validateOnly);
		if (apply) apply();
		return true;
	} catch (const std::exception&) { return false; }
}

std::function<void()> Icon::PrepareCheckpointSet(std::string_view text, std::span<Icon> icons, bool validateOnly) {
	CheckpointReader reader(text, "IconSet1", true);
	std::vector<std::string> images, values;
	reader.Value(images); reader.Value(values); reader.Finish();
	if (values.size() != icons.size()) throw std::runtime_error("invalid icon set count");
	struct State {
		std::string entity, file;
		unsigned int frameCount;
		std::vector<size_t> indexed, trueColor;
	};
	std::vector<State> states(values.size());
	for (size_t index = 0; index < states.size(); ++index) {
		auto& state = states[index];
		CheckpointReader value(values[index], "IconValues1", true);
		value.Value(state.entity); value.Value(state.file); value.Value(state.frameCount);
		value.Value(state.indexed); value.Value(state.trueColor); value.Finish();
		if (!icons[index].Entity::LoadCheckpoint(state.entity, true) || !icons[index].m_BitmapFile.LoadCheckpoint(state.file, true)) throw std::runtime_error("invalid icon metadata");
		for (const auto* references: {&state.indexed, &state.trueColor}) {
			for (size_t reference: *references) if (reference > images.size()) throw std::runtime_error("invalid icon bitmap reference");
		}
	}
	GUICheckpoint::LoadSharedBitmapPool(images, true);
	if (validateOnly) return {};
	struct Preparation {
		std::vector<std::unique_ptr<Icon>> icons;
		GUICheckpoint::PreparedBitmapPool pool;
		std::unordered_map<size_t, std::string> paths;
		bool applied = false;
	};
	auto prepared = std::make_shared<Preparation>();
	prepared->pool = GUICheckpoint::PrepareSharedBitmapPool(images);
	auto& pool = prepared->pool;
	auto& paths = prepared->paths;
	for (auto& state: states) {
		auto icon = std::make_unique<Icon>();
		if (!icon->Entity::LoadCheckpoint(state.entity) || !icon->m_BitmapFile.LoadCheckpoint(state.file, false, false)) throw std::runtime_error("could not prepare icon metadata");
		icon->m_FrameCount = state.frameCount;
		std::vector<bool> retained(pool.images.size(), false);
		const auto resolve = [&](const std::vector<size_t>& references, std::vector<BITMAP*>& bitmaps) {
			bitmaps.reserve(references.size());
			for (size_t index: references) {
				bitmaps.push_back(index ? pool.images[index - 1].get() : nullptr);
				if (index && !retained[index - 1]) {
					icon->m_CheckpointBitmapOwners.push_back(pool.images[index - 1]);
					retained[index - 1] = true;
				}
			}
		};
		resolve(state.indexed, icon->m_BitmapsIndexed); resolve(state.trueColor, icon->m_BitmapsTrueColor);
		if (!icon->m_BitmapFile.m_DataPath.empty()) paths[icon->m_BitmapFile.GetHash()] = icon->m_BitmapFile.m_DataPath;
		prepared->icons.push_back(std::move(icon));
	}
	if (!paths.empty()) ContentFile::s_PathHashes.reserve(ContentFile::s_PathHashes.size() + paths.size());
	return [icons, prepared]() {
		if (prepared->applied) return;
		prepared->pool.commit();
		while (!prepared->paths.empty()) {
			auto node = prepared->paths.extract(prepared->paths.begin());
			const auto found = ContentFile::s_PathHashes.find(node.key());
			if (found != ContentFile::s_PathHashes.end()) found->second.swap(node.mapped());
			else ContentFile::s_PathHashes.insert(std::move(node));
		}
		for (size_t index = 0; index < icons.size(); ++index) icons[index].SwapCheckpoint(*prepared->icons[index]);
		prepared->applied = true;
		prepared->icons.clear();
		prepared->pool.images.clear();
		prepared->pool.commit = {};
	};
}

void Icon::SwapCheckpoint(Icon& other) noexcept {
	using std::swap;
	swap(m_PresetName, other.m_PresetName); swap(m_CopiedFromPresetName, other.m_CopiedFromPresetName);
	swap(m_PresetDescription, other.m_PresetDescription); swap(m_FormattedReaderPosition, other.m_FormattedReaderPosition);
	swap(m_IsOriginalPreset, other.m_IsOriginalPreset); swap(m_DefinedInModule, other.m_DefinedInModule);
	swap(m_Groups, other.m_Groups); swap(m_RandomWeight, other.m_RandomWeight);
	m_BitmapFile.SwapCheckpoint(other.m_BitmapFile);
	swap(m_FrameCount, other.m_FrameCount); swap(m_BitmapsIndexed, other.m_BitmapsIndexed);
	swap(m_BitmapsTrueColor, other.m_BitmapsTrueColor); swap(m_CheckpointBitmapOwners, other.m_CheckpointBitmapOwners);
}
