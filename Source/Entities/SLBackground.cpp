#include "SLBackground.h"
#include "GUICheckpoint.h"
#include "CheckpointArchive.h"
#include "BigTexture.h"
#include "FrameMan.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include <algorithm>

#include "raylib/raylib.h"
#include "raylib/rlgl.h"

using namespace RTE;

ConcreteClassInfo(SLBackground, StaticSceneLayer, 0);

SLBackground::SLBackground() {
	Clear();
}

SLBackground::~SLBackground() {
	Destroy(true);
}

void SLBackground::Clear() {
	m_CheckpointBitmaps.clear();
	m_Bitmaps.clear();
	m_FrameCount = 1;
	m_Frame = 0;
	m_SpriteAnimMode = SpriteAnimMode::NOANIM;
	m_SpriteAnimDuration = 1000;
	m_SpriteAnimIsReversingFrames = false;
	m_SpriteAnimTimer.Reset();
	m_IsAnimatedManually = false;
	m_CanAutoScrollX = false;
	m_CanAutoScrollY = false;
	m_AutoScrollStep.Reset();
	m_AutoScrollStepInterval = 0;
	m_AutoScrollStepTimer.Reset();
	m_AutoScrollOffset.Reset();
	m_FillColorLeft = ColorKeys::g_MaskColor;
	m_FillColorRight = ColorKeys::g_MaskColor;
	m_FillColorUp = ColorKeys::g_MaskColor;
	m_FillColorDown = ColorKeys::g_MaskColor;
	m_ZOrder = c_BackgroundDepth;

	m_IgnoreAutoScale = false;
}

int SLBackground::Create() {
	StaticSceneLayer::Create();

	m_Bitmaps.clear();
	m_BitmapFile.GetAsAnimation(m_Bitmaps, m_FrameCount);
	m_MainBitmap = m_Bitmaps[0];

	if (m_FrameCount == 1) {
		m_SpriteAnimMode = SpriteAnimMode::NOANIM;
	} else if (m_FrameCount == 2 && m_SpriteAnimMode != SpriteAnimMode::NOANIM) {
		m_SpriteAnimMode = SpriteAnimMode::ALWAYSLOOP;
	}

	if (!m_WrapX) {
		m_FillColorLeft = _getpixel(m_MainBitmap, 0, m_MainBitmap->h / 2);
		m_FillColorRight = _getpixel(m_MainBitmap, m_MainBitmap->w - 1, m_MainBitmap->h / 2);
	}
	if (!m_WrapY) {
		m_FillColorUp = _getpixel(m_MainBitmap, m_MainBitmap->w / 2, 0);
		m_FillColorDown = _getpixel(m_MainBitmap, m_MainBitmap->w / 2, m_MainBitmap->h - 1);
	}
	return 0;
}

int SLBackground::Create(const SLBackground& reference) {
	StaticSceneLayer::Create(reference);

	// The main bitmap is created and owned by SceneLayer because it can be modified. We need to destroy it to avoid a leak because the bitmaps we'll be using here are owned by ContentFile static maps and are unmodifiable.
	destroy_bitmap(m_MainBitmap);
	m_MainBitmapOwned = false;

	m_Bitmaps.clear();
	m_Bitmaps = reference.m_Bitmaps;
	m_CheckpointBitmaps = reference.m_CheckpointBitmaps;
	m_MainBitmap = reference.m_MainBitmap;

	m_FillColorLeft = reference.m_FillColorLeft;
	m_FillColorRight = reference.m_FillColorRight;
	m_FillColorUp = reference.m_FillColorUp;
	m_FillColorDown = reference.m_FillColorDown;

	m_FrameCount = reference.m_FrameCount;
	m_SpriteAnimMode = reference.m_SpriteAnimMode;
	m_SpriteAnimDuration = reference.m_SpriteAnimDuration;
	m_Frame = reference.m_Frame;
	m_SpriteAnimIsReversingFrames = reference.m_SpriteAnimIsReversingFrames;
	m_SpriteAnimTimer = reference.m_SpriteAnimTimer;
	m_IsAnimatedManually = reference.m_IsAnimatedManually;

	m_CanAutoScrollX = reference.m_CanAutoScrollX;
	m_CanAutoScrollY = reference.m_CanAutoScrollY;
	m_AutoScrollStep = reference.m_AutoScrollStep;
	m_AutoScrollStepInterval = reference.m_AutoScrollStepInterval;
	m_AutoScrollStepTimer = reference.m_AutoScrollStepTimer;
	m_AutoScrollOffset = reference.m_AutoScrollOffset;

	m_IgnoreAutoScale = reference.m_IgnoreAutoScale;

	return 0;
}

int SLBackground::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return StaticSceneLayer::ReadProperty(propName, reader));

	MatchProperty("FrameCount", { reader >> m_FrameCount; });
	MatchProperty("SpriteAnimMode", {
		m_SpriteAnimMode = static_cast<SpriteAnimMode>(std::stoi(reader.ReadPropValue()));
		if (m_SpriteAnimMode < SpriteAnimMode::NOANIM || m_SpriteAnimMode > SpriteAnimMode::ALWAYSPINGPONG) {
			reader.ReportError("Invalid SLBackground sprite animation mode!");
		}
	});
	MatchProperty("SpriteAnimDuration", { reader >> m_SpriteAnimDuration; });
	MatchProperty("IsAnimatedManually", { reader >> m_IsAnimatedManually; });
	MatchProperty("DrawTransparent", { reader >> m_DrawMasked; });
	MatchProperty("ScrollRatio", {
		// Actually read the ScrollInfo, not the ratio. The ratios will be initialized later.
		reader >> m_ScrollInfo;
	});
	MatchProperty("ScaleFactor", {
		reader >> m_ScaleFactor;
		SetScaleFactor(m_ScaleFactor);
	});
	MatchProperty("IgnoreAutoScaling", { reader >> m_IgnoreAutoScale; });
	MatchProperty("OriginPointOffset", { reader >> m_OriginOffset; });
	MatchProperty("CanAutoScrollX", { reader >> m_CanAutoScrollX; });
	MatchProperty("CanAutoScrollY", { reader >> m_CanAutoScrollY; });
	MatchProperty("AutoScrollStepInterval", { reader >> m_AutoScrollStepInterval; });
	MatchProperty("AutoScrollStep", { reader >> m_AutoScrollStep; });

	EndPropertyList;
}

int SLBackground::Save(Writer& writer) const {
	StaticSceneLayer::Save(writer);

	writer.NewPropertyWithValue("FrameCount", m_FrameCount);
	writer.NewPropertyWithValue("SpriteAnimMode", m_SpriteAnimMode);
	writer.NewPropertyWithValue("SpriteAnimDuration", m_SpriteAnimDuration);
	writer.NewPropertyWithValue("IsAnimatedManually", m_IsAnimatedManually);
	writer.NewPropertyWithValue("DrawTransparent", m_DrawMasked);
	writer.NewPropertyWithValue("ScrollRatio", m_ScrollInfo);
	writer.NewPropertyWithValue("ScaleFactor", m_ScaleFactor);
	writer.NewPropertyWithValue("IgnoreAutoScaling", m_IgnoreAutoScale);
	writer.NewPropertyWithValue("OriginPointOffset", m_OriginOffset);
	writer.NewPropertyWithValue("CanAutoScrollX", m_CanAutoScrollX);
	writer.NewPropertyWithValue("CanAutoScrollY", m_CanAutoScrollY);
	writer.NewPropertyWithValue("AutoScrollStepInterval", m_AutoScrollStepInterval);
	writer.NewPropertyWithValue("AutoScrollStep", m_AutoScrollStep);

	return 0;
}

void SLBackground::InitScaleFactors() {
	if (!m_IgnoreAutoScale) {
		float fitScreenScaleFactor = std::clamp(static_cast<float>(std::min(g_SceneMan.GetSceneHeight(), g_FrameMan.GetPlayerScreenHeight())) / static_cast<float>(m_MainBitmap->h), 1.0F, 2.0F);

		switch (g_SettingsMan.GetSceneBackgroundAutoScaleMode()) {
			case LayerAutoScaleMode::FitScreen:
				SetScaleFactor(Vector(fitScreenScaleFactor, fitScreenScaleFactor));
				break;
			case LayerAutoScaleMode::AlwaysUpscaled:
				SetScaleFactor(Vector(2.0F, 2.0F));
				break;
			default:
				SetScaleFactor(m_ScaleFactor);
				break;
		}
		m_ScrollInfo *= m_ScaleFactor;
		InitScrollRatios();
	}
}

void SLBackground::Update() {
	if (!m_IsAnimatedManually && m_SpriteAnimMode != SpriteAnimMode::NOANIM) {
		int prevFrame = m_Frame;

		if (m_SpriteAnimTimer.GetElapsedSimTimeMS() > (m_SpriteAnimDuration / m_FrameCount)) {
			switch (m_SpriteAnimMode) {
				case SpriteAnimMode::ALWAYSLOOP:
					m_Frame = (m_Frame + 1) % m_FrameCount;
					break;
				case SpriteAnimMode::ALWAYSRANDOM:
					while (m_Frame == prevFrame) {
						m_Frame = RandomNum(0, m_FrameCount - 1);
					}
					break;
				case SpriteAnimMode::ALWAYSPINGPONG:
					if (m_Frame == m_FrameCount - 1) {
						m_SpriteAnimIsReversingFrames = true;
					} else if (m_Frame == 0) {
						m_SpriteAnimIsReversingFrames = false;
					}
					m_SpriteAnimIsReversingFrames ? m_Frame-- : m_Frame++;
					break;
				default:
					break;
			}
			m_SpriteAnimTimer.Reset();
		}
	}
	m_MainBitmap = m_Bitmaps.at(m_Frame);

	if (IsAutoScrolling()) {
		if (m_AutoScrollStepTimer.GetElapsedSimTimeMS() > m_AutoScrollStepInterval) {
			if (m_WrapX && m_CanAutoScrollX) {
				m_AutoScrollOffset.SetX(m_AutoScrollOffset.GetX() + m_AutoScrollStep.GetX());
			}
			if (m_WrapY && m_CanAutoScrollY) {
				m_AutoScrollOffset.SetY(m_AutoScrollOffset.GetY() + m_AutoScrollStep.GetY());
			}
			WrapPosition(m_AutoScrollOffset);
			m_AutoScrollStepTimer.Reset();
		}
		m_Offset.SetXY(std::floor((m_Offset.GetX() * m_ScrollRatio.GetX()) + m_AutoScrollOffset.GetX()), std::floor((m_Offset.GetY() * m_ScrollRatio.GetY()) + m_AutoScrollOffset.GetY()));
	}
}

void SLBackground::Draw(const Box& targetDimensions, Box& targetBox, bool offsetNeedsScrollRatioAdjustment) {
	StaticSceneLayer::Draw(targetDimensions, targetBox, !IsAutoScrolling());

	int bitmapWidth = m_ScaledDimensions.GetFloorIntX();
	int bitmapHeight = m_ScaledDimensions.GetFloorIntY();
	int targetBoxCornerX = targetBox.GetCorner().GetFloorIntX();
	int targetBoxCornerY = targetBox.GetCorner().GetFloorIntY();
	int targetBoxWidth = static_cast<int>(targetBox.GetWidth());
	int targetBoxHeight = static_cast<int>(targetBox.GetHeight());

	rlZDepth(m_ZOrder);
	// Detect if non-wrapping layer dimensions can't cover the whole target area with its main bitmap. If so, fill in the gap with appropriate solid color sampled from the hanging edge.
	if (!m_WrapX && bitmapWidth <= targetBoxWidth) {
		if (m_FillColorLeft != ColorKeys::g_MaskColor && m_Offset.GetFloorIntX() != 0) {
			DrawRectangle(targetBoxCornerX, targetBoxCornerY, -m_Offset.m_X, targetBoxHeight, {static_cast<unsigned char>(m_FillColorLeft), 0, 0, 255});
		}
		if (m_FillColorRight != ColorKeys::g_MaskColor) {
			DrawRectangle(targetBoxCornerX + bitmapWidth - m_Offset.m_X, targetBoxCornerY, targetBoxWidth - bitmapWidth + m_Offset.m_X, targetBoxHeight, {static_cast<unsigned char>(m_FillColorRight), 0, 0, 255});
		}
	}
	if (!m_WrapY && bitmapHeight <= targetBoxHeight) {
		if (m_FillColorUp != ColorKeys::g_MaskColor && m_Offset.GetFloorIntY() != 0) {
			DrawRectangle(targetBoxCornerX, targetBoxCornerY, targetBoxWidth, - m_Offset.m_Y, {static_cast<unsigned char>(m_FillColorUp), 0, 0, 255});
		}
		if (m_FillColorDown != ColorKeys::g_MaskColor) {
			DrawRectangle(targetBoxCornerX, targetBoxCornerY + bitmapHeight - m_Offset.m_Y, targetBoxWidth, targetBoxHeight - bitmapHeight + m_Offset.m_Y, {static_cast<unsigned char>(m_FillColorDown), 0, 0, 255});
		}
	}
	rlZDepth(c_DefaultDrawDepth);
}

std::string SLBackground::SaveCheckpoint() const {
	if (m_BitmapClearTask.valid()) m_BitmapClearTask.wait();
	CheckpointWriter writer("SLBackground2");
	writer(m_BitmapFile, m_FrameCount, m_Frame, m_SpriteAnimMode, m_SpriteAnimDuration, m_SpriteAnimIsReversingFrames, m_SpriteAnimTimer,
		m_IsAnimatedManually, m_CanAutoScrollX, m_CanAutoScrollY, m_AutoScrollStep, m_AutoScrollStepInterval, m_AutoScrollStepTimer, m_AutoScrollOffset,
		m_FillColorLeft, m_FillColorRight, m_FillColorUp, m_FillColorDown, m_IgnoreAutoScale, m_LastClearColor, m_MainBitmapUpdated, m_DrawMasked,
		m_WrapX, m_WrapY, m_OriginOffset, m_Offset, m_ZOrder, m_ScrollInfo, m_ScrollRatio, m_ScaleFactor, m_ScaledDimensions);
	writer(m_Drawings.size());
	for (const auto& rectangle: m_Drawings) writer(rectangle.m_Left, rectangle.m_Top, rectangle.m_Right, rectangle.m_Bottom);
	std::vector<std::string> frames;
	int mainIndex = -1;
	for (size_t index = 0; index < m_Bitmaps.size(); ++index) {
		frames.push_back(GUICheckpoint::SaveBitmap(m_Bitmaps[index]));
		if (m_Bitmaps[index] == m_MainBitmap) mainIndex = static_cast<int>(index);
	}
	writer(frames, mainIndex, GUICheckpoint::SaveBitmap(mainIndex < 0 ? m_MainBitmap : nullptr), GUICheckpoint::SaveBitmap(m_BackBitmap));
	return writer.Text();
}

bool SLBackground::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		if (!validateOnly && !LoadCheckpoint(text, true)) return false;
		const bool legacy = text.starts_with("13 SLBackground1 ");
		CheckpointReader reader(text, legacy ? "SLBackground1" : "SLBackground2", validateOnly);
		std::string path;
		int frameCount, frame;
		if (legacy) reader.Value(path); else reader(m_BitmapFile);
		reader.Value(frameCount); reader.Value(frame);
		reader(m_SpriteAnimMode, m_SpriteAnimDuration, m_SpriteAnimIsReversingFrames, m_SpriteAnimTimer,
			m_IsAnimatedManually, m_CanAutoScrollX, m_CanAutoScrollY, m_AutoScrollStep, m_AutoScrollStepInterval, m_AutoScrollStepTimer, m_AutoScrollOffset,
			m_FillColorLeft, m_FillColorRight, m_FillColorUp, m_FillColorDown, m_IgnoreAutoScale, m_LastClearColor, m_MainBitmapUpdated, m_DrawMasked,
			m_WrapX, m_WrapY, m_OriginOffset, m_Offset, m_ZOrder, m_ScrollInfo, m_ScrollRatio, m_ScaleFactor, m_ScaledDimensions);
		size_t count;
		reader.Value(count);
		if (count > text.size()) return false;
		std::vector<IntRect> drawings(count, IntRect(0, 0, 0, 0));
		for (auto& rectangle: drawings) { reader.Value(rectangle.m_Left); reader.Value(rectangle.m_Top); reader.Value(rectangle.m_Right); reader.Value(rectangle.m_Bottom); }
		std::vector<std::string> frames;
		int mainIndex;
		std::string mainBitmap, backBitmap;
		reader.Value(frames); reader.Value(mainIndex); reader.Value(mainBitmap); reader.Value(backBitmap);
		if (frameCount < 1 || frame < 0 || frame >= frameCount || mainIndex < -1 || mainIndex >= static_cast<int64_t>(frames.size()) || (!frames.empty() && frames.size() != static_cast<size_t>(frameCount))) return false;
		for (const auto& image: frames) GUICheckpoint::LoadBitmap(image, true);
		GUICheckpoint::LoadBitmap(mainBitmap, true); GUICheckpoint::LoadBitmap(backBitmap, true);
		if (validateOnly) { reader.Finish(); return true; }
		std::vector<std::shared_ptr<BITMAP>> owned;
		std::vector<BITMAP*> bitmaps;
		for (const auto& image: frames) {
			owned.emplace_back(GUICheckpoint::LoadBitmap(image), destroy_bitmap);
			if (!owned.back()) return false;
			bitmaps.push_back(owned.back().get());
		}
		BITMAP* main;
		if (mainIndex < 0) { owned.emplace_back(GUICheckpoint::LoadBitmap(mainBitmap), destroy_bitmap); main = owned.back().get(); }
		else main = bitmaps[mainIndex];
		std::unique_ptr<BITMAP, void(*)(BITMAP*)> back(GUICheckpoint::LoadBitmap(backBitmap), destroy_bitmap);
		if (m_BitmapClearTask.valid()) m_BitmapClearTask.wait();
		reader.Finish();
		if (m_MainBitmapOwned) destroy_bitmap(m_MainBitmap);
		destroy_bitmap(m_BackBitmap);
		m_MainTexture.reset();
		if (legacy) { if (path.empty()) m_BitmapFile.Reset(); else m_BitmapFile.SetDataPath(path); }
		m_FrameCount = frameCount; m_Frame = frame;
		m_Drawings = std::move(drawings); m_MainBitmapOwned = false;
		m_CheckpointBitmaps = std::move(owned); m_Bitmaps = std::move(bitmaps); m_MainBitmap = main; m_BackBitmap = back.release();
		return true;
	} catch (const std::exception&) { return false; }
}
