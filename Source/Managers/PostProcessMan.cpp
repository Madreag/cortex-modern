#include "PostProcessMan.h"

#include "CameraMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "Scene.h"
#include "ContentFile.h"
#include "Matrix.h"
#include "CheckpointArchive.h"
#include "GUICheckpoint.h"

#include "PresetMan.h"
#include "GLResourceMan.h"
#include "RenderTarget.h"

#include "GLCheck.h"
#include "glad/gl.h"
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

#include "tracy/Tracy.hpp"
#include "tracy/TracyOpenGL.hpp"
#include "raylib/raylib.h"

#include <array>
#include <iostream>
#include <map>
#include <set>

using namespace RTE;

PostProcessMan::PostProcessMan() {
	Clear();
}

PostProcessMan::~PostProcessMan() {
	Destroy();
}

void PostProcessMan::Clear() {
	m_PostScreenEffects.clear();
	m_PostSceneEffects.clear();
	m_YellowGlow = nullptr;
	m_YellowGlowHash = 0;
	m_RedGlow = nullptr;
	m_RedGlowHash = 0;
	m_BlueGlow = nullptr;
	m_BlueGlowHash = 0;
	m_TempEffectBitmaps.clear();
	m_CheckpointBitmaps.clear();
	m_BackBuffer8 = 0;
	m_Palette8Texture = 0;
	m_PostProcessFramebuffer = 0;
	m_VertexBuffer = 0;
	m_VertexArray = 0;
	for (int i = 0; i < c_MaxScreenCount; ++i) {
		m_ScreenRelativeEffects[i].clear();
	}
}

int PostProcessMan::Initialize() {
	InitializeGLPointers();
	CreateGLBackBuffers();

	m_Blit8 = std::make_unique<Shader>(g_PresetMan.GetFullModulePath("Base.rte/Shaders/Blit8.vert"), g_PresetMan.GetFullModulePath("Base.rte/Shaders/Blit8.frag"));
	m_PostProcessShader = std::make_unique<Shader>(g_PresetMan.GetFullModulePath("Base.rte/Shaders/PostProcess.vert"), g_PresetMan.GetFullModulePath("Base.rte/Shaders/PostProcess.frag"));
	// TODO: Make more robust and load more glows!
	ContentFile glowFile("Base.rte/Effects/Glows/YellowTiny.png");
	m_YellowGlow = glowFile.GetAsBitmap();
	m_YellowGlowHash = glowFile.GetHash();
	glowFile.SetDataPath("Base.rte/Effects/Glows/RedTiny.png");
	m_RedGlow = glowFile.GetAsBitmap();
	m_RedGlowHash = glowFile.GetHash();
	glowFile.SetDataPath("Base.rte/Effects/Glows/BlueTiny.png");
	m_BlueGlow = glowFile.GetAsBitmap();
	m_BlueGlowHash = glowFile.GetHash();

	// Create temporary bitmaps to rotate post effects in.
	for (int size: {16, 32, 64, 128, 256, 512}) {
		auto bitmap = std::shared_ptr<BITMAP>(create_bitmap(size, size), destroy_bitmap);
		clear_to_color(bitmap.get(), bitmap_mask_color(bitmap.get()));
		m_TempEffectBitmaps.emplace(size, std::move(bitmap));
	}

	return 0;
}

void PostProcessMan::InitializeGLPointers() {
	GL_CHECK(glGenTextures(1, &m_BackBuffer8));
	GL_CHECK(glGenTextures(1, &m_Palette8Texture));
	GL_CHECK(glGenVertexArrays(1, &m_VertexArray));
	GL_CHECK(glGenBuffers(1, &m_VertexBuffer));
}

void PostProcessMan::DestroyGLPointers() {
	GL_CHECK(glDeleteTextures(1, &m_BackBuffer8));
	GL_CHECK(glDeleteTextures(1, &m_Palette8Texture));
	GL_CHECK(glDeleteVertexArrays(1, &m_VertexArray));
	GL_CHECK(glDeleteBuffers(1, &m_VertexBuffer));
}

void PostProcessMan::CreateGLBackBuffers() {
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_BackBuffer8));
	GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_FrameMan.GetBackBuffer8()->w, g_FrameMan.GetBackBuffer8()->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_Palette8Texture));
	GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c_PaletteEntriesNumber, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	UpdatePalette();

	m_BlitFramebuffer = std::make_unique<RenderTarget>(FloatRect(0, 0 ,g_FrameMan.GetBackBuffer32()->w, g_FrameMan.GetBackBuffer32()->h), FloatRect(0, 0 ,g_FrameMan.GetBackBuffer32()->w, g_FrameMan.GetBackBuffer32()->h));
	m_PostProcessFramebuffer = std::make_unique<RenderTarget>(FloatRect(0, 0 ,g_FrameMan.GetBackBuffer32()->w, g_FrameMan.GetBackBuffer32()->h), FloatRect(0, 0 ,g_FrameMan.GetBackBuffer32()->w, g_FrameMan.GetBackBuffer32()->h));

	GL_CHECK(glActiveTexture(GL_TEXTURE0));
	m_ProjectionMatrix = std::make_unique<glm::mat4>(glm::ortho(0.0F, static_cast<float>(g_WindowMan.GetResX()), 0.0F, static_cast<float>(g_WindowMan.GetResY()), -1.0F, 1.0F));
}

void PostProcessMan::UpdatePalette() {
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_Palette8Texture));
	std::array<unsigned int, c_PaletteEntriesNumber> palette;
	for (int i = 0; i < c_PaletteEntriesNumber; ++i) {
		if (i == g_MaskColor) {
			palette[i] = 0;
			continue;
		}
		palette[i] = makeacol32(getr8(i), getg8(i), getb8(i), 255);
	}
	GL_CHECK(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c_PaletteEntriesNumber, 1, GL_RGBA, GL_UNSIGNED_BYTE, palette.data()));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
	GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
}

void PostProcessMan::Destroy() {
	DestroyGLPointers();
	ClearScreenPostEffects();
	ClearScenePostEffects();
	Clear();
}

std::string PostProcessMan::SaveCheckpoint() const {
	std::vector<const BITMAP*> bitmaps;
	std::unordered_map<const BITMAP*, size_t> bitmapIDs;
	auto bitmapID = [&](const BITMAP* bitmap) {
		if (!bitmap) return size_t{0};
		auto [entry, inserted] = bitmapIDs.emplace(bitmap, bitmaps.size() + 1);
		if (inserted) bitmaps.push_back(bitmap);
		return entry->second;
	};
	CheckpointWriter queues("PostProcessQueues1");
	auto effects = [&](const std::list<PostEffect>& values) {
		queues(values.size());
		for (const auto& effect: values) {
			const auto* attached = effect.m_AttachedToMOID >= 0 ? g_MovableMan.GetMOFromID(effect.m_AttachedToMOID) : nullptr;
			queues(bitmapID(effect.m_Bitmap), effect.m_BitmapHash, effect.m_Angle, effect.m_Strength, effect.m_Pos,
			       effect.m_AttachedToMOID, attached ? attached->GetUniqueID() : 0L);
		}
	};
	effects(m_PostSceneEffects); effects(m_PostScreenEffects);
	for (const auto& values: m_ScreenRelativeEffects) effects(values);
	queues(m_PostScreenGlowBoxes, m_GlowAreas.size());
	for (const auto& rect: m_GlowAreas) queues(rect.m_Left, rect.m_Top, rect.m_Right, rect.m_Bottom);
	const std::array<size_t, 3> glowIDs{bitmapID(m_YellowGlow), bitmapID(m_RedGlow), bitmapID(m_BlueGlow)};
	queues(glowIDs[0], m_YellowGlowHash, glowIDs[1], m_RedGlowHash, glowIDs[2], m_BlueGlowHash);
	const std::map<int, std::shared_ptr<BITMAP>> temporary(m_TempEffectBitmaps.begin(), m_TempEffectBitmaps.end());
	queues(temporary.size());
	for (const auto& [size, bitmap]: temporary) queues(size, bitmapID(bitmap.get()));
	CheckpointWriter writer("PostProcessMan2");
	writer(s_RegistrationSuppressed, bitmaps.size());
	for (const auto* bitmap: bitmaps) writer(GUICheckpoint::SaveSharedBitmap(bitmap));
	writer(queues.Text());
	return writer.Text();
}

bool PostProcessMan::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		struct EffectState { size_t bitmap, hash; float angle; int strength; Vector position; MOID attached; long uniqueID; };
		struct State {
			bool suppressed;
			std::vector<std::string> images;
			std::vector<std::shared_ptr<BITMAP>> bitmaps;
			std::array<std::vector<EffectState>, c_MaxScreenCount + 2> records;
			std::array<std::list<PostEffect>, c_MaxScreenCount + 2> effects;
			std::list<Box> boxes;
			std::list<IntRect> areas;
			std::array<size_t, 3> glow{}, hashes{};
			std::map<int, size_t> temporary;
			std::unordered_map<int, std::shared_ptr<BITMAP>> temporaryImages;
		};
		auto state = std::make_shared<State>();
		const bool legacy = text.starts_with("15 PostProcessMan1 ");
		CheckpointReader reader(text, legacy ? "PostProcessMan1" : "PostProcessMan2", validateOnly);
		reader.Value(state->suppressed); reader.Value(state->images);
		for (const auto& value: state->images) {
			if (legacy) GUICheckpoint::LoadBitmap(value, true);
			else GUICheckpoint::LoadSharedBitmap(value, true);
		}
		std::string queueText; reader.Value(queueText);
		CheckpointReader queue(queueText, "PostProcessQueues1", true);
		auto count = [&] { size_t value; queue.Value(value); if (value > queueText.size()) throw std::runtime_error("invalid post effect count"); return value; };
		auto image = [&](size_t index) { if (index > state->images.size()) throw std::runtime_error("invalid post effect bitmap reference"); };
		for (auto& records: state->records) {
			records.resize(count());
			for (auto& record: records) {
				queue.Value(record.bitmap); queue.Value(record.hash); queue.Value(record.angle); queue.Value(record.strength);
				queue.Value(record.position); queue.Value(record.attached); queue.Value(record.uniqueID);
				image(record.bitmap);
				if (record.attached < 0 || record.uniqueID < 0 || (record.uniqueID && (record.attached == 0 || record.attached == g_NoMOID)))
					throw std::runtime_error("invalid post effect attachment");
			}
		}
		queue.Value(state->boxes);
		for (size_t remaining = count(); remaining; --remaining) {
			IntRect area(0, 0, 0, 0);
			queue.Value(area.m_Left); queue.Value(area.m_Top); queue.Value(area.m_Right); queue.Value(area.m_Bottom);
			state->areas.push_back(area);
		}
		for (size_t index = 0; index < state->glow.size(); ++index) { queue.Value(state->glow[index]); queue.Value(state->hashes[index]); image(state->glow[index]); }
		for (size_t remaining = count(); remaining; --remaining) {
			int size; size_t index; queue.Value(size); queue.Value(index); image(index);
			if (size <= 0 || !index || !state->temporary.emplace(size, index).second) throw std::runtime_error("invalid temporary post effect image");
		}
		queue.Finish();
		if (!validateOnly) {
			for (const auto& value: state->images) {
				auto bitmap = legacy ? std::shared_ptr<BITMAP>(GUICheckpoint::LoadBitmap(value), destroy_bitmap) : GUICheckpoint::LoadSharedBitmap(value);
				if (!bitmap) throw std::runtime_error("null post effect image owner");
				state->bitmaps.push_back(std::move(bitmap));
			}
			auto bitmap = [state](size_t index) { return index ? state->bitmaps[index - 1].get() : nullptr; };
			for (size_t index = 0; index < state->records.size(); ++index) for (const auto& record: state->records[index]) {
				if (record.uniqueID) {
					const auto* attached = g_MovableMan.GetMOFromID(record.attached);
					if (!attached || attached->GetUniqueID() != record.uniqueID) throw std::runtime_error("unresolved post effect attachment");
				}
				state->effects[index].emplace_back(record.position, bitmap(record.bitmap), record.hash, record.strength, record.angle, record.attached);
			}
			for (const auto& [size, index]: state->temporary) state->temporaryImages.emplace(size, state->bitmaps[index - 1]);
			reader.OnCommit([this, state, bitmap] {
				m_PostSceneEffects.swap(state->effects[0]); m_PostScreenEffects.swap(state->effects[1]);
				for (size_t index = 0; index < m_ScreenRelativeEffects.size(); ++index) m_ScreenRelativeEffects[index].swap(state->effects[index + 2]);
				m_PostScreenGlowBoxes.swap(state->boxes); m_GlowAreas.swap(state->areas);
				m_YellowGlow = bitmap(state->glow[0]); m_RedGlow = bitmap(state->glow[1]); m_BlueGlow = bitmap(state->glow[2]);
				m_YellowGlowHash = state->hashes[0]; m_RedGlowHash = state->hashes[1]; m_BlueGlowHash = state->hashes[2];
				m_TempEffectBitmaps.swap(state->temporaryImages); m_CheckpointBitmaps.swap(state->bitmaps);
				s_RegistrationSuppressed = state->suppressed;
			});
		}
		reader.Finish();
		return true;
	} catch (const std::exception& error) {
		std::cerr << "[post-process-checkpoint] " << error.what() << std::endl;
		return false;
	}
}

bool PostProcessMan::RunCheckpointSelfTest() {
	const auto original = SaveCheckpoint();
	struct Restore { PostProcessMan& manager; const std::string& value; ~Restore() { manager.LoadCheckpoint(value); } } restore{*this, original};
	bool passed = true;
	auto check = [&](bool value, const char* name) { passed = value && passed; std::cout << "[post-process-checkpoint-selftest] " << (value ? "PASS " : "FAIL ") << name << std::endl; };
	const auto originalGlows = std::array<BITMAP*, 3>{m_YellowGlow, m_RedGlow, m_BlueGlow};
	check(LoadCheckpoint(original) && originalGlows == std::array<BITMAP*, 3>{m_YellowGlow, m_RedGlow, m_BlueGlow}, "content_cache_glow_aliases");
	ClearScenePostEffects(); ClearScreenPostEffects();
	for (auto& effects: m_ScreenRelativeEffects) effects.clear();
	auto first = std::shared_ptr<BITMAP>(create_bitmap_ex(32, 16, 16), destroy_bitmap);
	auto second = std::shared_ptr<BITMAP>(create_bitmap_ex(8, 7, 5), destroy_bitmap);
	clear_to_color(first.get(), makeacol32(29, 91, 173, 221)); clear_to_color(second.get(), 37);
	putpixel(first.get(), 4, 6, makeacol32(199, 12, 45, 177)); putpixel(second.get(), 3, 2, 49);
	m_PostSceneEffects.emplace_back(Vector(12.25F, 17.5F), first.get(), 137, 209, -0.75F);
	m_PostSceneEffects.emplace_back(Vector(25.5F, 13.75F), first.get(), 137, 143, 0.25F, g_NoMOID);
	m_PostScreenEffects.emplace_back(Vector(22.5F, 9.75F), second.get(), 941, 179, 0.5F);
	for (size_t index = 0; index < m_ScreenRelativeEffects.size(); ++index)
		m_ScreenRelativeEffects[index].emplace_back(Vector(10 + index, 20 + index), index % 2 ? second.get() : first.get(), 200 + index, 100 + index, 0.125F * index);
	m_PostScreenGlowBoxes.emplace_back(Vector(12.5F, 18.75F), 29.5F, 31.25F);
	m_GlowAreas.emplace_back(11, 13, 31, 37);
	m_YellowGlow = first.get(); m_RedGlow = second.get(); m_BlueGlow = first.get();
	m_YellowGlowHash = 31; m_RedGlowHash = 37; m_BlueGlowHash = 41;
	m_TempEffectBitmaps.clear(); m_TempEffectBitmaps.emplace(16, first); m_TempEffectBitmaps.emplace(512, first);
	s_RegistrationSuppressed = true;
	const auto checkpoint = SaveCheckpoint();
	check(LoadCheckpoint(checkpoint, true), "validate_full_queues");
	clear_to_color(first.get(), 0); clear_to_color(second.get(), 0); ClearScenePostEffects(); ClearScreenPostEffects();
	for (auto& effects: m_ScreenRelativeEffects) effects.clear();
	m_TempEffectBitmaps.clear(); s_RegistrationSuppressed = false;
	check(LoadCheckpoint(checkpoint) && SaveCheckpoint() == checkpoint, "all_queues_and_pixels_restored");
	check(m_PostSceneEffects.size() == 2 && m_PostScreenEffects.size() == 1 && m_GlowAreas.size() == 1 && m_PostScreenGlowBoxes.size() == 1, "independent_pending_queue_counts");
	check(m_PostSceneEffects.front().m_Bitmap == m_YellowGlow && m_BlueGlow == m_YellowGlow && GetTempEffectBitmap(m_YellowGlow) == m_YellowGlow && m_ScreenRelativeEffects[2].front().m_Bitmap == m_YellowGlow, "shared_bitmap_owner_aliases");
	check(getpixel(m_YellowGlow, 4, 6) == makeacol32(199, 12, 45, 177) && getpixel(m_RedGlow, 3, 2) == 49, "independent_owned_pixel_observations");
	std::list<PostEffect> selected;
	check(GetPostScreenEffects(0, 0, 100, 100, selected) && selected.size() == 2 && selected.front().m_Pos == Vector(12.25F, 17.5F) && selected.front().m_Strength == 209, "scene_effect_query_continuation");
	RegisterPostEffect(Vector(30, 40), m_YellowGlow, 149, 201, 0.75F);
	check(m_PostSceneEffects.size() == 2, "registration_suppression_restored");
	check(!LoadCheckpoint(checkpoint + "x") && SaveCheckpoint() == checkpoint, "trailing_data_rejection_atomic");
	check(!LoadCheckpoint(checkpoint.substr(0, checkpoint.size() - 1)) && SaveCheckpoint() == checkpoint, "truncated_data_rejection_atomic");
	ClearScenePostEffects();
	check(m_PostSceneEffects.empty() && m_GlowAreas.empty() && m_PostScreenEffects.size() == 1 && m_ScreenRelativeEffects[3].size() == 1, "queue_clear_boundaries");
	check(LoadCheckpoint(original) && SaveCheckpoint() == original, "original_manager_restored");
	return passed;
}

void PostProcessMan::AdjustEffectsPosToPlayerScreen(int playerScreen, BITMAP* targetBitmap, const Vector& targetBitmapOffset, std::list<PostEffect>& screenRelativeEffectsList, std::list<Box>& screenRelativeGlowBoxesList) {
	int screenOcclusionOffsetX = g_CameraMan.GetScreenOcclusion(playerScreen).GetFloorIntX();
	int screenOcclusionOffsetY = g_CameraMan.GetScreenOcclusion(playerScreen).GetFloorIntY();
	int occludedOffsetX = targetBitmap->w + screenOcclusionOffsetX;
	int occludedOffsetY = targetBitmap->h + screenOcclusionOffsetY;

	// Adjust for the player screen's position on the final buffer
	for (const PostEffect& postEffect: screenRelativeEffectsList) {
		// Make sure we won't be adding any effects to a part of the screen that is occluded by menus and such
		if (postEffect.m_Pos.GetFloorIntX() > screenOcclusionOffsetX && postEffect.m_Pos.GetFloorIntY() > screenOcclusionOffsetY && postEffect.m_Pos.GetFloorIntX() < occludedOffsetX && postEffect.m_Pos.GetFloorIntY() < occludedOffsetY) {
			m_PostScreenEffects.emplace_back(postEffect.m_Pos + targetBitmapOffset, postEffect.m_Bitmap, postEffect.m_BitmapHash, postEffect.m_Strength, postEffect.m_Angle);
		}
	}
	// Adjust glow areas for the player screen's position on the final buffer
	for (const Box& glowBox: screenRelativeGlowBoxesList) {
		m_PostScreenGlowBoxes.push_back(glowBox);
		// Adjust each added glow area for the player screen's position on the final buffer
		m_PostScreenGlowBoxes.back().m_Corner += targetBitmapOffset;
	}
}

void PostProcessMan::RegisterPostEffect(const Vector& effectPos, BITMAP* effect, size_t hash, int strength, float angle) {
	// These effects get applied when there's a drawn frame that followed one or more sim updates.
	// They are not only registered on drawn sim updates; flashes and stuff could be missed otherwise if they occur on undrawn sim updates.

	if (!s_RegistrationSuppressed && effect && g_TimerMan.SimUpdatesSinceDrawn() >= 0) {
		m_PostSceneEffects.push_back(PostEffect(effectPos, effect, hash, strength, angle));
	}
}

void PostProcessMan::RegisterPostEffect(const Vector& effectPos, BITMAP* effect, size_t hash, int strength, float angle, MOID attachedToMOID) {
	if (!s_RegistrationSuppressed && effect && g_TimerMan.SimUpdatesSinceDrawn() >= 0) {
		m_PostSceneEffects.push_back(PostEffect(effectPos, effect, hash, strength, angle, attachedToMOID));
	}
}

bool PostProcessMan::GetPostScreenEffectsWrapped(const Vector& boxPos, int boxWidth, int boxHeight, std::list<PostEffect>& effectsList, int team) {
	bool found = false;

	// Do the first unwrapped rect
	found = GetPostScreenEffects(boxPos, boxWidth, boxHeight, effectsList, team);

	int left = boxPos.GetFloorIntX();
	int top = boxPos.GetFloorIntY();
	int right = left + boxWidth;
	int bottom = top + boxHeight;

	if (g_SceneMan.SceneWrapsX()) {
		int sceneWidth = g_SceneMan.GetScene()->GetWidth();
		if (left < 0) {
			found = GetPostScreenEffects(left + sceneWidth, top, right + sceneWidth, bottom, effectsList, team) || found;
		}
		if (right >= sceneWidth) {
			found = GetPostScreenEffects(left - sceneWidth, top, right - sceneWidth, bottom, effectsList, team) || found;
		}
	}
	if (g_SceneMan.SceneWrapsY()) {
		int sceneHeight = g_SceneMan.GetScene()->GetHeight();
		if (top < 0) {
			found = GetPostScreenEffects(left, top + sceneHeight, right, bottom + sceneHeight, effectsList, team) || found;
		}
		if (bottom >= sceneHeight) {
			found = GetPostScreenEffects(left, top - sceneHeight, right, bottom - sceneHeight, effectsList, team) || found;
		}
	}
	return found;
}

BITMAP* PostProcessMan::GetTempEffectBitmap(BITMAP* bitmap) const {
	// Get the largest dimension of the bitmap and convert it to a multiple of 16, i.e. 16, 32, etc
	int bitmapSizeNeeded = static_cast<int>(std::ceil(static_cast<float>(std::max(bitmap->w, bitmap->h)) / 16.0F)) * 16;
	auto correspondingBitmapSizeEntry = m_TempEffectBitmaps.find(bitmapSizeNeeded);

	// If we didn't find a match then the bitmap size is greater than 512 but that's the biggest we've got, so return it
	if (correspondingBitmapSizeEntry == m_TempEffectBitmaps.end()) {
		correspondingBitmapSizeEntry = m_TempEffectBitmaps.find(512);
	}

	return correspondingBitmapSizeEntry->second.get();
}

void PostProcessMan::RegisterGlowDotEffect(const Vector& effectPos, DotGlowColor color, int strength) {
	if (s_RegistrationSuppressed) {
		return;
	}
	// These effects only apply only once per drawn sim update, and only on the first frame drawn after one or more sim updates
	if (color != NoDot && g_TimerMan.DrawnSimUpdate() && g_TimerMan.SimUpdatesSinceDrawn() >= 0) {
		RegisterPostEffect(effectPos, GetDotGlowEffect(color), GetDotGlowEffectHash(color), strength);
	}
}

bool PostProcessMan::GetGlowAreasWrapped(const Vector& boxPos, int boxWidth, int boxHeight, std::list<Box>& areaList) const {
	bool foundAny = false;
	Vector intRectPosRelativeToBox;

	// Account for wrapping in any registered glow IntRects, as well as on the box we're testing against
	std::list<IntRect> wrappedGlowRects;

	for (const IntRect& glowArea: m_GlowAreas) {
		g_SceneMan.WrapRect(glowArea, wrappedGlowRects);
	}
	std::list<IntRect> wrappedTestRects;
	g_SceneMan.WrapRect(IntRect(boxPos.GetFloorIntX(), boxPos.GetFloorIntY(), boxPos.GetFloorIntX() + boxWidth, boxPos.GetFloorIntY() + boxHeight), wrappedTestRects);

	// Check for intersections. If any are found, cut down the intersecting IntRect to the bounds of the IntRect we're testing against, then make and store a Box out of it
	for (IntRect& wrappedTestRect: wrappedTestRects) {
		for (const IntRect& wrappedGlowRect: wrappedGlowRects) {
			if (wrappedTestRect.Intersects(wrappedGlowRect)) {
				IntRect cutRect(wrappedGlowRect);
				cutRect.IntersectionCut(wrappedTestRect);
				intRectPosRelativeToBox = Vector(static_cast<float>(cutRect.m_Left) - boxPos.m_X, static_cast<float>(cutRect.m_Top) - boxPos.m_Y);
				areaList.push_back(Box(intRectPosRelativeToBox, static_cast<float>(cutRect.m_Right - cutRect.m_Left), static_cast<float>(cutRect.m_Bottom - cutRect.m_Top)));
				foundAny = true;
			}
		}
	}
	return foundAny;
}

bool PostProcessMan::GetPostScreenEffects(Vector boxPos, int boxWidth, int boxHeight, std::list<PostEffect>& effectsList, int team) {
	bool found = false;
	bool unseen = false;
	Vector postEffectPosRelativeToBox;

	if (g_SceneMan.GetScene()) {
		for (PostEffect& scenePostEffect: m_PostSceneEffects) {
			// If attached to an MO, look up its current render pos so the effect tracks the sprite (handles activity-paused placement teleports and the render-vs-sim interp delta).
			Vector effectScenePos = scenePostEffect.m_Pos;
			if (scenePostEffect.m_AttachedToMOID != g_NoMOID) {
				if (MovableObject* attachedMO = g_MovableMan.GetMOFromID(scenePostEffect.m_AttachedToMOID)) {
					effectScenePos = attachedMO->GetRenderPos();
				}
			}

			if (team != Activity::NoTeam) {
				unseen = g_SceneMan.IsUnseen(effectScenePos.GetFloorIntX(), effectScenePos.GetFloorIntY(), team);
			}

			if (WithinBox(effectScenePos, boxPos, static_cast<float>(boxWidth), static_cast<float>(boxHeight)) && !unseen) {
				found = true;
				postEffectPosRelativeToBox = effectScenePos - boxPos;
				effectsList.push_back(PostEffect(postEffectPosRelativeToBox, scenePostEffect.m_Bitmap, scenePostEffect.m_BitmapHash, scenePostEffect.m_Strength, scenePostEffect.m_Angle));
			}
		}
	}
	return found;
}

bool PostProcessMan::GetPostScreenEffects(int left, int top, int right, int bottom, std::list<PostEffect>& effectsList, int team) {
	bool found = false;
	bool unseen = false;
	Vector postEffectPosRelativeToBox;

	for (PostEffect& scenePostEffect: m_PostSceneEffects) {
		Vector effectScenePos = scenePostEffect.m_Pos;
		if (scenePostEffect.m_AttachedToMOID != g_NoMOID) {
			if (MovableObject* attachedMO = g_MovableMan.GetMOFromID(scenePostEffect.m_AttachedToMOID)) {
				effectScenePos = attachedMO->GetRenderPos();
			}
		}

		if (team != Activity::NoTeam) {
			unseen = g_SceneMan.IsUnseen(effectScenePos.GetFloorIntX(), effectScenePos.GetFloorIntY(), team);
		}

		if (WithinBox(effectScenePos, static_cast<float>(left), static_cast<float>(top), static_cast<float>(right), static_cast<float>(bottom)) && !unseen) {
			found = true;
			postEffectPosRelativeToBox = Vector(effectScenePos.m_X - static_cast<float>(left), effectScenePos.m_Y - static_cast<float>(top));
			effectsList.push_back(PostEffect(postEffectPosRelativeToBox, scenePostEffect.m_Bitmap, scenePostEffect.m_BitmapHash, scenePostEffect.m_Strength, scenePostEffect.m_Angle));
		}
	}
	return found;
}

BITMAP* PostProcessMan::GetDotGlowEffect(DotGlowColor whichColor) const {
	switch (whichColor) {
		case NoDot:
			return nullptr;
		case YellowDot:
			return m_YellowGlow;
		case RedDot:
			return m_RedGlow;
		case BlueDot:
			return m_BlueGlow;
		default:
			RTEAbort("Undefined glow dot color value passed in. See DotGlowColor enumeration for defined values.");
			return nullptr;
	}
}

size_t PostProcessMan::GetDotGlowEffectHash(DotGlowColor whichColor) const {
	switch (whichColor) {
		case NoDot:
			return 0;
		case YellowDot:
			return m_YellowGlowHash;
		case RedDot:
			return m_RedGlowHash;
		case BlueDot:
			return m_BlueGlowHash;
		default:
			RTEAbort("Undefined glow dot color value passed in. See DotGlowColor enumeration for defined values.");
			return 0;
	}
}

void PostProcessMan::PostProcess() {
	ZoneScoped;
	TracyGpuZone("PostProcess");
	UpdatePalette();

	// First copy the current 8bpp backbuffer to the 32bpp buffer; we'll add effects to it
	m_PostProcessFramebuffer->Begin(true);
	//m_Blit8->Begin();
	//int paletteUniform = m_Blit8->GetUniformLocation("rtePalette");
	//rlSetUniformSampler(paletteUniform, m_Palette8Texture);
	rlDisableColorBlend();
	rlDisableDepthTest();
	DrawTextureRec(g_FrameMan.GetBackBuffer()->GetColorTexture(), {0, 0, g_FrameMan.GetBackBuffer()->GetSize().w, -g_FrameMan.GetBackBuffer()->GetSize().h}, {0.0f, 0.0f}, {255, 255, 255, 255});
	//m_Blit8->End();

	// Set the screen blender mode for glows
	set_screen_blender(128, 128, 128, 128);
	rlEnableColorBlend();
	rlSetBlendFactorsSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD);
	rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
	m_PostProcessFramebuffer->End();
	m_PostProcessFramebuffer->Begin(false);

	m_PostProcessShader->Begin();

	DrawDotGlowEffects();
	DrawPostScreenEffects();

	// Clear the effects list for this frame
	m_PostScreenEffects.clear();
	m_PostProcessShader->End();
	m_PostProcessFramebuffer->End();
}

void PostProcessMan::DrawDotGlowEffects() {
	int startX = 0;
	int startY = 0;
	int endX = 0;
	int endY = 0;
	int testpixel = 0;

	Texture2D yellowGlow = g_GLResourceMan.GetStaticTextureFromBitmap(m_YellowGlow);

	// Randomly sample the entire backbuffer, looking for pixels to put a glow on.
	for (const Box& glowBox: m_PostScreenGlowBoxes) {
		startX = glowBox.m_Corner.GetFloorIntX();
		startY = glowBox.m_Corner.GetFloorIntY();
		endX = startX + static_cast<int>(glowBox.m_Width);
		endY = startY + static_cast<int>(glowBox.m_Height);

		// Sanity check a little at least
		if (startX < 0 || startX >= g_FrameMan.GetBackBuffer8()->w || startY < 0 || startY >= g_FrameMan.GetBackBuffer8()->h ||
		    endX < 0 || endX >= g_FrameMan.GetBackBuffer8()->w || endY < 0 || endY >= g_FrameMan.GetBackBuffer8()->h) {
			continue;
		}

#ifdef DEBUG_BUILD
		// Draw a rectangle around the glow box so we see it's position and size
		rect(g_FrameMan.GetBackBuffer32(), startX, startY, endX, endY, g_RedColor);
#endif

		for (int y = startY; y < endY; ++y) {
			for (int x = startX; x < endX; ++x) {
				testpixel = _getpixel(g_FrameMan.GetBackBuffer8(), x, y);

				// YELLOW
				if ((testpixel == g_YellowGlowColor && g_RenderRNG.RandomNum() < 0.9F) || testpixel == 98 || (testpixel == 120 && g_RenderRNG.RandomNum() < 0.7F)) {
					DrawTexture(yellowGlow, x - yellowGlow.width / 2, y - yellowGlow.height / 2, {255, 255, 255, 255});
				}
				// TODO: Enable and add more colors once we actually have something that needs these.
				// RED
				/*
				if (testpixel == 13) {
				    draw_trans_sprite(m_BackBuffer32, m_RedGlow, x - 2, y - 2);
				}
				// BLUE
				if (testpixel == 166) {
				    draw_trans_sprite(g_FrameMan.GetBackBuffer32(), m_BlueGlow, x - 2, y - 2);
				}
				*/
			}
		}
	}
}

void PostProcessMan::DrawPostScreenEffects() {
	int effectPosX = 0;
	int effectPosY = 0;
	unsigned char effectStrength = 0;

	GL_CHECK(glActiveTexture(GL_TEXTURE0));
	GL_CHECK(glBindVertexArray(m_VertexArray));
	m_PostProcessShader->SetInt(m_PostProcessShader->GetTextureUniform(), 0);
	m_PostProcessShader->SetMatrix4f(m_PostProcessShader->GetProjectionUniform(), *m_ProjectionMatrix);
	m_PostProcessShader->SetVector4f(m_PostProcessShader->GetColorUniform(), glm::vec4(1.0f));

	for (const PostEffect& postEffect: m_PostScreenEffects) {
		if (postEffect.m_Bitmap) {
			effectStrength = postEffect.m_Strength;
			effectPosX = postEffect.m_Pos.GetFloorIntX();
			effectPosY = postEffect.m_Pos.GetFloorIntY();
			DrawTexturePro(
			    g_GLResourceMan.GetStaticTextureFromBitmap(postEffect.m_Bitmap),
			    Rectangle(0, 0, postEffect.m_Bitmap->w, postEffect.m_Bitmap->h),
			    Rectangle(effectPosX, effectPosY, postEffect.m_Bitmap->w, postEffect.m_Bitmap->h),
				Vector2(postEffect.m_Bitmap->w / 2, postEffect.m_Bitmap->h / 2),
			    postEffect.m_Angle,
			    {.r=effectStrength, .g=effectStrength, .b=effectStrength, .a=255});
		}
	}
}
