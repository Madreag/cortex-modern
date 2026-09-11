#include "FrameMan.h"
#include "CheckpointArchive.h"

#include "SDL3/SDL_surface.h"
#include "WindowMan.h"
#include "PostProcessMan.h"
#include "PresetMan.h"
#include "PrimitiveMan.h"
#include "PerformanceMan.h"
#include "ActivityMan.h"
#include "CameraMan.h"
#include "ConsoleMan.h"
#include "SettingsMan.h"
#include "ThreadMan.h"
#include "UInputMan.h"
#include "GLResourceMan.h"

#include "SLTerrain.h"
#include "SLBackground.h"
#include "Scene.h"
#include "System.h"

#include "RenderTarget.h"

#include "GUI.h"
#include "GUICheckpoint.h"
#include "AllegroBitmap.h"
#include "AllegroScreen.h"
#include "AllegroTools.h"
#include "allegro/internal/aintern.h"

#include "GLCheck.h"
#include "glad/gl.h"
#include "Draw.h"

#include "tracy/Tracy.hpp"
#include "tracy/TracyOpenGL.hpp"
#include <SDL3_image/SDL_image.h>

#include <array>
#include <fstream>
#include <iostream>
#include <cstring>
#include <map>

using namespace RTE;

void BitmapDeleter::operator()(BITMAP* bitmap) const { destroy_bitmap(bitmap); }
void SurfaceDeleter::operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }

const std::array<std::function<void(int r, int g, int b, int a)>, DrawBlendMode::BlendModeCount> FrameMan::c_BlenderSetterFunctions = {
    nullptr, // NoBlend obviously has no blender, but we want to keep the indices matching with the enum.
    &set_burn_blender,
    &set_color_blender,
    &set_difference_blender,
    &set_dissolve_blender,
    &set_dodge_blender,
    &set_invert_blender,
    &set_luminance_blender,
    &set_multiply_blender,
    &set_saturation_blender,
    &set_screen_blender,
    nullptr // Transparency does not rely on the blender setting, it creates a map with the dedicated function instead of with the generic one.
};

FrameMan::FrameMan() {
	Clear();
}

FrameMan::~FrameMan() {
	Destroy();
}

void FrameMan::Clear() {
	m_HSplit = false;
	m_VSplit = false;
	m_TwoPlayerVSplit = false;
	m_PlayerScreen.reset();
	m_PlayerScreenWidth = 0;
	m_PlayerScreenHeight = 0;
	m_ScreenDumpBuffer.reset();
	m_WorldDumpBuffer.reset();
	m_ScenePreviewDumpGradient.reset();
	m_ScreenDumpNamePlaceholder.reset();
	m_BackBuffer8.reset();
	m_BackBuffer32.reset();
	m_OverlayBitmap32.reset();
	m_PaletteFile = ContentFile("Base.rte/palette.bmp");
	std::memset(m_Palette, 0, sizeof(m_Palette));
	std::memset(m_DefaultPalette, 0, sizeof(m_DefaultPalette));
	std::memset(&m_RGBTable, 0, sizeof(m_RGBTable));
	for (auto& table: m_ColorTables) {
		for (auto& [key, entry]: table) if (color_map == &entry.first) color_map = nullptr;
		table.clear();
	}
	if (color_map == m_CheckpointColorTable.get()) color_map = nullptr;
	m_CheckpointColorTable.reset();
	m_CurrentAlpha = 255;
	m_BlackColor = 245;
	m_AlmostBlackColor = 245;
	m_ColorTablePruneTimer.Reset();
	m_GUIScreens.fill(nullptr);
	m_LargeFonts.fill(nullptr);
	m_SmallFonts.fill(nullptr);
	m_TextBlinkTimer.Reset();

	for (int screenCount = 0; screenCount < c_MaxScreenCount; ++screenCount) {
		m_ScreenText[screenCount].clear();
		m_TextDuration[screenCount] = -1;
		m_TextDurationTimer[screenCount].Reset();
		m_TextBlinking[screenCount] = 0;
		m_TextCentered[screenCount] = false;
		m_HUDDisabled[screenCount] = false;
		m_FlashScreenColor[screenCount] = -1;
		m_FlashedLastFrame[screenCount] = false;
		m_FlashTimer[screenCount].Reset();
	}
}

int FrameMan::Initialize() {
	set_color_depth(c_BPP);
	// Sets the allowed color conversions when loading bitmaps from files
	set_color_conversion(COLORCONV_MOST);

	LoadPalette(m_PaletteFile.GetDataPath());

	// Store the default palette for re-use when creating new color tables for different blend modes because the palette can be changed via scripts, and handling per-palette per-mode color tables is too much headache.
	get_palette(m_DefaultPalette);

	CreatePresetColorTables();
	SetTransTableFromPreset(TransparencyPreset::HalfTrans);
	CreateBackBuffers();

	ContentFile scenePreviewGradientFile("Base.rte/GUIs/PreviewSkyGradient.png");
	m_ScenePreviewDumpGradient = std::unique_ptr<BITMAP, BitmapDeleter>(scenePreviewGradientFile.GetAsBitmap(COLORCONV_8_TO_32, false));

	m_ScreenDumpNamePlaceholder = std::unique_ptr<BITMAP, BitmapDeleter>(create_bitmap_ex(24, 1, 1));
	clear_bitmap(m_ScreenDumpNamePlaceholder.get());

	// Use fastest compression in save_png().
	_png_compression_level = 1;

	return 0;
}

int FrameMan::CreateBackBuffers() {
	int resX = g_WindowMan.GetResX();
	int resY = g_WindowMan.GetResY();

	// Create the back buffer, this is still in 8bpp, we will do any post-processing on the PostProcessing bitmap
	m_BackBuffer8 = std::unique_ptr<BITMAP, BitmapDeleter>(create_bitmap_ex(8, resX, resY));
	ClearBackBuffer8();

	// Create the post-processing buffer, it'll be used for glow effects etc
	m_BackBuffer32 = std::unique_ptr<BITMAP, BitmapDeleter>(create_bitmap_ex(c_BPP, resX, resY));
	ClearBackBuffer32();

	m_BackBuffer = std::make_unique<RenderTarget>(FloatRect(0, 0, resX, resY), FloatRect(0, 0, resX, resY));

	m_OverlayBitmap32 = std::unique_ptr<BITMAP, BitmapDeleter>(create_bitmap_ex(c_BPP, resX, resY));
	clear_to_color(m_OverlayBitmap32.get(), 0);

	m_PlayerScreenWidth = m_BackBuffer8->w;
	m_PlayerScreenHeight = m_BackBuffer8->h;

	// Create the splitscreen buffer
	if (m_HSplit || m_VSplit) {
		m_PlayerScreen8 = std::unique_ptr<BITMAP, BitmapDeleter>(create_bitmap_ex(8, resX / (m_VSplit ? 2 : 1), resY / (m_HSplit ? 2 : 1)));
		clear_to_color(m_PlayerScreen8.get(), 0);
		set_clip_state(m_PlayerScreen8.get(), 1);

		m_PlayerScreen = std::make_unique<RenderTarget>(FloatRect(0, 0, resX / (m_VSplit ? 2 : 1), resY / (m_HSplit ? 2 : 1)), FloatRect(0, 0, resX / (m_VSplit ? 2 : 1), resY / (m_HSplit ? 2 : 1)));

		// Update these to represent the split screens
		m_PlayerScreenWidth = m_PlayerScreen->GetSize().w;
		m_PlayerScreenHeight = m_PlayerScreen->GetSize().h;
	} else {
		m_PlayerScreen8 = m_BackBuffer8;
		m_PlayerScreen = m_BackBuffer;
	}

	m_ScreenDumpBuffer = std::unique_ptr<SDL_Surface, SurfaceDeleter>(SDL_CreateSurface(m_BackBuffer8->w, m_BackBuffer8->h, SDL_PIXELFORMAT_RGB24));

	return 0;
}

void FrameMan::CreatePresetColorTables() {
	// Create RGB lookup table that supposedly speeds up calculation of other color tables.
	// create_rgb_table(&m_RGBTable, m_DefaultPalette, nullptr);
	// rgb_map = &m_RGBTable;

	// Create transparency color tables. Tables for other blend modes will be created on demand.
	int transparencyPresetCount = BlendAmountLimits::MaxBlend / c_BlendAmountStep;
	for (int index = 0; index <= transparencyPresetCount; ++index) {
		int presetBlendAmount = index * c_BlendAmountStep;
		std::array<int, 4> colorChannelBlendAmounts = {presetBlendAmount, presetBlendAmount, presetBlendAmount, BlendAmountLimits::MinBlend};
		int adjustedBlendAmount = 255 - (static_cast<int>(255.0F * (1.0F / static_cast<float>(transparencyPresetCount) * static_cast<float>(index))));

		m_ColorTables.at(DrawBlendMode::BlendTransparency).try_emplace(colorChannelBlendAmounts);
		create_trans_table(&m_ColorTables[DrawBlendMode::BlendTransparency].at(colorChannelBlendAmounts).first, m_DefaultPalette, adjustedBlendAmount, adjustedBlendAmount, adjustedBlendAmount, nullptr);
		m_ColorTables[DrawBlendMode::BlendTransparency].at(colorChannelBlendAmounts).second = -1;
	}
}

void FrameMan::Destroy() {
	for (const GUIScreen* guiScreen: m_GUIScreens) {
		delete guiScreen;
	}
	for (GUIFont* guiFont: m_LargeFonts) {
		if (guiFont) { guiFont->Destroy(); delete guiFont; }
	}
	for (GUIFont* guiFont: m_SmallFonts) {
		if (guiFont) { guiFont->Destroy(); delete guiFont; }
	}
	Clear();
}

void FrameMan::Update() {
	// Remove all scheduled primitives, those will be re-added by updates from other entities.
	// This needs to happen here, otherwise if there are multiple sim updates during a single frame duplicates will be added to the primitive queue.
	g_PrimitiveMan.ClearPrimitivesQueue();

	// Prune unused color tables every 5 real minutes to prevent ridiculous memory usage over time.
	if (m_ColorTablePruneTimer.IsPastRealMS(300000)) {
		long long currentTime = g_TimerMan.GetAbsoluteTime() / 10000;
		for (std::unordered_map<std::array<int, 4>, std::pair<COLOR_MAP, long long>>& colorTableMap: m_ColorTables) {
			if (colorTableMap.size() >= 100) {
				std::vector<std::array<int, 4>> markedForDelete;
				markedForDelete.reserve(colorTableMap.size());
				for (const auto& [tableKey, tableData]: colorTableMap) {
					long long lastAccessTime = tableData.second;
					// Mark tables that haven't been accessed in the last minute for deletion. Avoid marking the transparency table presets, those will have lastAccessTime set to -1.
					if (&tableData.first != color_map && lastAccessTime != -1 && (currentTime - lastAccessTime > 60)) {
						markedForDelete.emplace_back(tableKey);
					}
				}
				for (const std::array<int, 4>& keyToDelete: markedForDelete) {
					colorTableMap.erase(keyToDelete);
				}
			}
		}
		m_ColorTablePruneTimer.Reset();
	}

	// Update redundantly in sim update to ensure our values are exactly precise for the purposes of script GetOffset()
	int screenCount = (m_HSplit ? 2 : 1) * (m_VSplit ? 2 : 1);
	for (int playerScreen = 0; playerScreen < screenCount; ++playerScreen) {
		g_CameraMan.Update(playerScreen);
	}
}

void FrameMan::ResetSplitScreens(bool hSplit, bool vSplit) {
	// Override screen splitting according to settings if needed
	if ((hSplit || vSplit) && !(hSplit && vSplit) && m_TwoPlayerVSplit) {
		hSplit = false;
		vSplit = m_TwoPlayerVSplit;
	}

	m_HSplit = hSplit;
	m_VSplit = vSplit;

	// Create the splitscreen buffer
	if (m_HSplit || m_VSplit) {
		m_PlayerScreen8 = std::unique_ptr<BITMAP, BitmapDeleter>(create_bitmap_ex(8, g_WindowMan.GetResX() / (m_VSplit ? 2 : 1), g_WindowMan.GetResY() / (m_HSplit ? 2 : 1)));
		clear_to_color(m_PlayerScreen8.get(), 0);
		set_clip_state(m_PlayerScreen8.get(), 1);

		m_PlayerScreen = std::make_unique<RenderTarget>(FloatRect(0, 0, g_WindowMan.GetResX() / (m_VSplit ? 2 : 1), g_WindowMan.GetResY() / (m_HSplit ? 2 : 1)), FloatRect(0, 0, g_WindowMan.GetResX() / (m_VSplit ? 2 : 1), g_WindowMan.GetResY() / (m_HSplit ? 2 : 1)));

		// Update these to represent the split screens
		m_PlayerScreenWidth = m_PlayerScreen->GetSize().w;
		m_PlayerScreenHeight = m_PlayerScreen->GetSize().h;
	} else {
		m_PlayerScreen8 = m_BackBuffer8;
		m_PlayerScreen = m_BackBuffer;
		// No splits, so set the screen dimensions equal to the back buffer
		m_PlayerScreenWidth = m_BackBuffer8->w;
		m_PlayerScreenHeight = m_BackBuffer8->h;
	}
	for (int i = 0; i < c_MaxScreenCount; ++i) {
		m_FlashScreenColor[i] = -1;
		m_FlashedLastFrame[i] = false;
	}
}

float FrameMan::GetResolutionMultiplier() const {
	return g_WindowMan.GetResMultiplier();
}

Vector FrameMan::GetMiddleOfPlayerScreen(int whichPlayer) {
	Vector middleOfPlayerScreen;

	if (whichPlayer == -1) {
		middleOfPlayerScreen.SetXY(static_cast<float>(g_WindowMan.GetResX() / 2), static_cast<float>(g_WindowMan.GetResY() / 2));
	} else {
		int playerScreen = g_ActivityMan.GetActivity()->ScreenOfPlayer(whichPlayer);

		middleOfPlayerScreen.SetXY(static_cast<float>(m_PlayerScreenWidth / 2), static_cast<float>(m_PlayerScreenHeight / 2));
		if ((playerScreen == 1 && g_FrameMan.GetVSplit()) || playerScreen == 3) {
			middleOfPlayerScreen.SetX(middleOfPlayerScreen.GetX() + static_cast<float>(m_PlayerScreenWidth));
		}
		if ((playerScreen == 1 && g_FrameMan.GetHSplit()) || playerScreen >= 2) {
			middleOfPlayerScreen.SetY(middleOfPlayerScreen.GetY() + static_cast<float>(m_PlayerScreenHeight));
		}
	}
	return middleOfPlayerScreen;
}

int FrameMan::GetPlayerFrameBufferWidth(int whichPlayer) const {
	return m_PlayerScreenWidth;
}

int FrameMan::GetPlayerFrameBufferHeight(int whichPlayer) const {
	return m_PlayerScreenHeight;
}

int FrameMan::CalculateTextHeight(const std::string& text, int maxWidth, bool isSmall) {
	return isSmall ? GetSmallFont()->CalculateHeight(text, maxWidth) : GetLargeFont()->CalculateHeight(text, maxWidth);
}

std::string FrameMan::SplitStringToFitWidth(const std::string& stringToSplit, int widthLimit, bool useSmallFont) {
	GUIFont* fontToUse = GetFont(useSmallFont, false);
	auto SplitSingleLineAsNeeded = [this, &widthLimit, &fontToUse](std::string& lineToSplitAsNeeded) {
		int numberOfScreenWidthsForText = static_cast<int>(std::ceil(static_cast<float>(fontToUse->CalculateWidth(lineToSplitAsNeeded)) / static_cast<float>(widthLimit)));
		if (numberOfScreenWidthsForText > 1) {
			int splitInterval = static_cast<int>(std::ceil(static_cast<float>(lineToSplitAsNeeded.size()) / static_cast<float>(numberOfScreenWidthsForText)));
			for (int i = 1; i <= numberOfScreenWidthsForText; i++) {
				size_t newLineCharacterPosition = std::min(static_cast<size_t>(i * splitInterval + (i - 1)), lineToSplitAsNeeded.size());
				if (newLineCharacterPosition == lineToSplitAsNeeded.size()) {
					break;
				}
				lineToSplitAsNeeded.insert(newLineCharacterPosition, "\n");
			}
		}
	};

	std::string splitString;
	size_t previousNewLinePos = 0;
	size_t nextNewLinePos = stringToSplit.find("\n");
	if (nextNewLinePos != std::string::npos) {
		while (nextNewLinePos != std::string::npos) {
			nextNewLinePos = stringToSplit.find("\n", previousNewLinePos);
			std::string currentLine = stringToSplit.substr(previousNewLinePos, nextNewLinePos - previousNewLinePos);
			previousNewLinePos = nextNewLinePos + 1;

			SplitSingleLineAsNeeded(currentLine);
			splitString += currentLine;
			if (nextNewLinePos != std::string::npos) {
				splitString += "\n";
			}
		}
	} else {
		splitString = stringToSplit;
		SplitSingleLineAsNeeded(splitString);
	}

	return splitString;
}

int FrameMan::CalculateTextWidth(const std::string& text, bool isSmall) {
	return isSmall ? GetSmallFont()->CalculateWidth(text) : GetLargeFont()->CalculateWidth(text);
}

void FrameMan::SetScreenText(const std::string& message, int whichScreen, int blinkInterval, int displayDuration, bool centered) {
	// See if we can overwrite the previous message
	if (whichScreen >= 0 && whichScreen < c_MaxScreenCount && m_TextDurationTimer[whichScreen].IsPastRealMS(m_TextDuration[whichScreen])) {
		m_ScreenText[whichScreen] = message;
		m_TextDuration[whichScreen] = displayDuration;
		m_TextDurationTimer[whichScreen].Reset();
		m_TextBlinking[whichScreen] = blinkInterval;
		m_TextCentered[whichScreen] = centered;
	}
}

void FrameMan::ClearScreenText(int whichScreen) {
	if (whichScreen >= 0 && whichScreen < c_MaxScreenCount) {
		m_ScreenText[whichScreen].clear();
		m_TextDuration[whichScreen] = -1;
		m_TextDurationTimer[whichScreen].Reset();
		m_TextBlinking[whichScreen] = 0;
	}
}

void FrameMan::SetBlendMode(DrawBlendMode blendMode) {
	GLint invertLoc = rlGetLocationUniformCurrent("rteBlendInvert");
	glUniform1i(invertLoc, 0);
	switch (blendMode) {
		case BlendTransparency: {
			rlSetBlendMode(RL_BLEND_ALPHA);
			break;
		}
		case BlendScreen: {
			rlSetBlendMode(RL_BLEND_SCREEN);
			break;
		}
		case BlendDifference: {
			rlSetBlendMode(RL_BLEND_SUBTRACT_COLORS);
			break;
		}
		case BlendMultiply: {
			rlSetBlendMode(RL_BLEND_MULTIPLIED);
			break;
		}
		case BlendBurn: {
			rlSetBlendMode(RL_BLEND_BURN);
			break;
		}
		case BlendDodge: {
			rlSetBlendMode(RL_BLEND_DODGE);
			break;
		}
		case BlendColor: {
			rlSetBlendMode(RL_BLEND_HSL_COLOR);
			break;
		}
		case BlendLuminance: {
			rlSetBlendMode(RL_BLEND_HSL_LUMINOSITY);
			break;
		}
		case BlendSaturation: {
			rlSetBlendMode(RL_BLEND_HSL_SATURATION);
			break;
		}
		case BlendInvert: {
			rlSetBlendMode(RL_BLEND_ALPHA);
			glUniform1i(invertLoc, 1);
			break;
		}
		case BlendDissolve: {
			rlSetBlendMode(RL_BLEND_ALPHA);
			const Shader* dissolve = dynamic_cast<const Shader*>(g_PresetMan.GetEntityPreset("Shader", "Dissolve"));
			dissolve->Begin();
			GLint paletteLoc = dissolve->GetUniformLocation("rtePalette");
			rlSetUniformSampler(paletteLoc, g_PostProcessMan.GetPaletteTexture());
			break;
		}
		default: {
			rlSetBlendMode(RL_BLEND_ALPHA);
			RTEAssert(blendMode < BlendModeCount, "Unkown blend mode selected!");
			break;
		}
	}
}

void FrameMan::SetColorTable(DrawBlendMode blendMode, std::array<int, 4> colorChannelBlendAmounts) {
	RTEAssert(blendMode > DrawBlendMode::NoBlend && blendMode < DrawBlendMode::BlendModeCount, "Invalid DrawBlendMode or DrawBlendMode::NoBlend passed into FrameMan::SetColorTable. See DrawBlendMode enumeration for defined values.");

	for (int& colorChannelBlendAmount: colorChannelBlendAmounts) {
		colorChannelBlendAmount = RoundToNearestMultiple(std::clamp(colorChannelBlendAmount, static_cast<int>(BlendAmountLimits::MinBlend), static_cast<int>(BlendAmountLimits::MaxBlend)), c_BlendAmountStep);
	}

	bool usedPresetTransparencyTable = false;

	switch (blendMode) {
		case DrawBlendMode::NoBlend:
			RTEAbort("Somehow ended up attempting to set a color table for DrawBlendMode::NoBlend in FrameMan::SetColorTable! This should never happen!");
			return;
		case DrawBlendMode::BlendInvert:
		case DrawBlendMode::BlendDissolve:
			// Invert and Dissolve do nothing with the RGB channels values, so set all channels to Alpha channel value to avoid creating pointless variants.
			colorChannelBlendAmounts.fill(colorChannelBlendAmounts[3]);
			break;
		case DrawBlendMode::BlendTransparency:
			// Indexed transparency has dedicated maps that don't use alpha, so min it to attempt to load one of the presets, and in case there isn't one avoid creating a variant for each alpha value.
			colorChannelBlendAmounts[3] = BlendAmountLimits::MinBlend;
			usedPresetTransparencyTable = (colorChannelBlendAmounts[0] == colorChannelBlendAmounts[1]) && (colorChannelBlendAmounts[0] == colorChannelBlendAmounts[2]);
			break;
		default:
			break;
	}

	// New color tables will be created using the default palette loaded at FrameMan initialization because handling per-palette per-mode color tables is too much headache, even if it may possibly produce better blending results.
	if (m_ColorTables[blendMode].find(colorChannelBlendAmounts) == m_ColorTables[blendMode].end()) {
		m_ColorTables[blendMode].try_emplace(colorChannelBlendAmounts);

		std::array<int, 4> adjustedColorChannelBlendAmounts = {BlendAmountLimits::MinBlend, BlendAmountLimits::MinBlend, BlendAmountLimits::MinBlend, BlendAmountLimits::MinBlend};
		for (int index = 0; index < adjustedColorChannelBlendAmounts.size(); ++index) {
			adjustedColorChannelBlendAmounts[index] = 255 - (static_cast<int>(255.0F * 0.01F * static_cast<float>(colorChannelBlendAmounts[index])));
		}

		if (blendMode == DrawBlendMode::BlendTransparency) {
			// Paletted transparency has dedicated tables so better create one instead of generic for best result. Alpha is ignored here.
			create_trans_table(&m_ColorTables[DrawBlendMode::BlendTransparency].at(colorChannelBlendAmounts).first, m_DefaultPalette, adjustedColorChannelBlendAmounts[0], adjustedColorChannelBlendAmounts[1], adjustedColorChannelBlendAmounts[2], nullptr);
		} else {
			c_BlenderSetterFunctions[blendMode](adjustedColorChannelBlendAmounts[0], adjustedColorChannelBlendAmounts[1], adjustedColorChannelBlendAmounts[2], adjustedColorChannelBlendAmounts[3]);
			create_blender_table(&m_ColorTables[blendMode].at(colorChannelBlendAmounts).first, m_DefaultPalette, nullptr);
			// Reset the blender to avoid potentially screwing some true-color draw operation. Hopefully.
			c_BlenderSetterFunctions[blendMode](255, 255, 255, 255);
		}
	}
	color_map = &m_ColorTables[blendMode].at(colorChannelBlendAmounts).first;
	m_ColorTables[blendMode].at(colorChannelBlendAmounts).second = usedPresetTransparencyTable ? -1 : (g_TimerMan.GetAbsoluteTime() / 10000);
}

void FrameMan::SetTransTableFromPreset(TransparencyPreset transPreset) {
	RTEAssert(transPreset == TransparencyPreset::LessTrans || transPreset == TransparencyPreset::HalfTrans || transPreset == TransparencyPreset::MoreTrans, "Undefined transparency preset value passed in. See TransparencyPreset enumeration for defined values.");
	std::array<int, 4> colorChannelBlendAmounts = {transPreset, transPreset, transPreset, BlendAmountLimits::MinBlend};
	if (m_ColorTables[DrawBlendMode::BlendTransparency].find(colorChannelBlendAmounts) != m_ColorTables[DrawBlendMode::BlendTransparency].end()) {
		color_map = &m_ColorTables[DrawBlendMode::BlendTransparency].at(colorChannelBlendAmounts).first;
		m_ColorTables[DrawBlendMode::BlendTransparency].at(colorChannelBlendAmounts).second = -1;
	}
	constexpr int transparencyPresetCount = BlendAmountLimits::MaxBlend / c_BlendAmountStep;
	m_CurrentAlpha = 255 - (static_cast<int>(255.0F * ((1.0F / static_cast<float>(transparencyPresetCount)) * static_cast<float>(transPreset / c_BlendAmountStep))));
}

bool FrameMan::LoadPalette(const std::string& palettePath) {
	const std::string fullPalettePath = g_PresetMan.GetFullModulePath(palettePath);
	SDL_Surface* paletteImage = IMG_Load(palettePath.c_str());
	RTEAssert(paletteImage && SDL_GetSurfacePalette(paletteImage), ("Failed to load palette from bitmap with following path:\n\n" + fullPalettePath).c_str());

	SDL_Palette* palette = SDL_GetSurfacePalette(paletteImage);
	for (size_t i = 0; i < 256; i++) {
		m_Palette[i] = {
		    palette->colors[i].r,
		    palette->colors[i].g,
		    palette->colors[i].b,
		    0};
	}
	SDL_DestroySurface(paletteImage);

	set_palette(m_Palette);

	// Update what black is now with the loaded palette
	m_BlackColor = bestfit_color(m_Palette, 0, 0, 0);
	m_AlmostBlackColor = bestfit_color(m_Palette, 5, 5, 5);

	return true;
}

int FrameMan::SaveBitmap(SaveBitmapMode modeToSave, const std::string& nameBase, BITMAP* bitmapToSave) {
	if ((modeToSave == WorldDump || modeToSave == ScenePreviewDump) && !g_ActivityMan.ActivityRunning()) {
		return -1;
	}
	if (nameBase.empty() || nameBase.size() <= 0) {
		return -1;
	}
	set_palette(m_DefaultPalette);

	// TODO: Remove this once GCC13 is released and switched to. std::format and std::chrono::time_zone are not part of latest libstdc++.
#if defined(__GNUC__) && (__GNUC__ < 13 || defined(__APPLE__)) // FIXME: macOS for some reason builds with incorrect iconv in CI which breaks format, could not debug.
	std::chrono::time_point now = std::chrono::system_clock::now();
	time_t currentTime = std::chrono::system_clock::to_time_t(now);
	tm* localCurrentTime = std::localtime(&currentTime);
	std::array<char, 32> formattedTimeAndDate = {};
	std::strftime(formattedTimeAndDate.data(), sizeof(formattedTimeAndDate), "%F_%H-%M-%S", localCurrentTime);

	std::array<char, 128> fullFileNameBuffer = {};
	// We can't get sub-second precision from timeBuffer so we'll append absolute time to not overwrite the same file when dumping multiple times per second.
	std::snprintf(fullFileNameBuffer.data(), sizeof(fullFileNameBuffer), "%s/%s_%s.%lli.png", System::GetScreenshotDirectory().c_str(), nameBase.c_str(), formattedTimeAndDate.data(), g_TimerMan.GetAbsoluteTime());

	std::string fullFileName(fullFileNameBuffer.data());
#else
	std::string fullFileName = std::format("{}{}_{:%F_%H-%M-%S}.png", System::GetScreenshotDirectory(), nameBase, std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
#endif

	bool saveSuccess = false;

	switch (modeToSave) {
		case SingleBitmap:
			if (bitmapToSave && save_png(nameBase.c_str(), bitmapToSave, m_Palette) == 0) {
				g_ConsoleMan.PrintString("SYSTEM: Bitmap was dumped to: " + nameBase);
				saveSuccess = true;
			}
			break;
		case ScreenDump:
			if (m_ScreenDumpBuffer) {
				SaveScreenToBitmap();

				// The next frame may overwrite the screen buffer while this copy is saved.
				std::shared_ptr<SDL_Surface> saveSurface(SDL_ConvertSurface(m_ScreenDumpBuffer.get(), m_ScreenDumpBuffer->format), SurfaceDeleter());
				if (!saveSurface) {
					break;
				}
				g_ThreadMan.GetBackgroundThreadPool().push_task([fullFileName, saveSurface]() mutable {
					const auto surface = std::move(saveSurface);
					if (IMG_SavePNG(surface.get(), fullFileName.c_str())) {
						g_ConsoleMan.PrintString("SYSTEM: Screen was dumped to: " + fullFileName);
					} else {
						g_ConsoleMan.PrintString("ERROR: Unable to save bitmap to: " + fullFileName);
					}
				});

				saveSuccess = true;
			}
			break;
		case ScenePreviewDump:
		case WorldDump:
			if (!m_WorldDumpBuffer || (m_WorldDumpBuffer->w != g_SceneMan.GetSceneWidth() || m_WorldDumpBuffer->h != g_SceneMan.GetSceneHeight())) {
				m_WorldDumpBuffer = std::unique_ptr<BITMAP, BitmapDeleter>(create_bitmap_ex(c_BPP, g_SceneMan.GetSceneWidth(), g_SceneMan.GetSceneHeight()));
			}
			if (!m_WorldDumpBuffer) break;
			if (modeToSave == ScenePreviewDump) {
				DrawWorldDump(true);

				std::unique_ptr<BITMAP, BitmapDeleter> scenePreviewDumpBuffer(create_bitmap_ex(c_BPP, c_ScenePreviewWidth, c_ScenePreviewHeight));
				if (!scenePreviewDumpBuffer) break;
				blit(m_ScenePreviewDumpGradient.get(), scenePreviewDumpBuffer.get(), 0, 0, 0, 0, scenePreviewDumpBuffer->w, scenePreviewDumpBuffer->h);
				masked_stretch_blit(m_WorldDumpBuffer.get(), scenePreviewDumpBuffer.get(), 0, 0, m_WorldDumpBuffer->w, m_WorldDumpBuffer->h, 0, 0, scenePreviewDumpBuffer->w, scenePreviewDumpBuffer->h);

				if (SaveIndexedPNG(fullFileName.c_str(), scenePreviewDumpBuffer.get()) == 0) {
					g_ConsoleMan.PrintString("SYSTEM: Scene Preview was dumped to: " + fullFileName);
					saveSuccess = true;
				}
			} else {
				DrawWorldDump();

				std::unique_ptr<BITMAP, BitmapDeleter> depthConvertBitmap(create_bitmap_ex(24, m_WorldDumpBuffer->w, m_WorldDumpBuffer->h));
				if (!depthConvertBitmap) break;
				blit(m_WorldDumpBuffer.get(), depthConvertBitmap.get(), 0, 0, 0, 0, m_WorldDumpBuffer->w, m_WorldDumpBuffer->h);
				const int pitch = depthConvertBitmap->h > 1 ? static_cast<int>(depthConvertBitmap->line[1] - depthConvertBitmap->line[0]) : depthConvertBitmap->w * 3;
				std::unique_ptr<SDL_Surface, SurfaceDeleter> saveSurface(SDL_CreateSurfaceFrom(
				    depthConvertBitmap->w,
				    depthConvertBitmap->h,
				    SDL_PIXELFORMAT_RGB24,
				    depthConvertBitmap->line[0],
				    pitch));

				if (saveSurface && IMG_SavePNG(saveSurface.get(), fullFileName.c_str())) {
					g_ConsoleMan.PrintString("SYSTEM: World was dumped to: " + fullFileName);
					saveSuccess = true;
				}
			}
			break;
		default:
			g_ConsoleMan.PrintString("ERROR: Wrong bitmap save mode passed in, no bitmap was saved!");
			return -1;
	}
	if (!saveSuccess) {
		g_ConsoleMan.PrintString("ERROR: Unable to save bitmap to: " + fullFileName);
		return -1;
	} else {
		return 0;
	}
}

void FrameMan::SaveScreenToBitmap() {
	if (!m_ScreenDumpBuffer) {
		return;
	}

	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	GL_CHECK(glBindTexture(GL_TEXTURE_2D, g_WindowMan.GetScreenBuffer()->GetColorTexture().id));
	GL_CHECK(glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, m_ScreenDumpBuffer->pixels));

	// Flip the pixels
	std::vector<char*> temp(m_ScreenDumpBuffer->pitch);
	char* pixels = reinterpret_cast<char*>(m_ScreenDumpBuffer->pixels);
	size_t pitch = m_ScreenDumpBuffer->pitch;
	for (size_t y = 0; y < m_ScreenDumpBuffer->h / 2; ++y) {
		std::swap_ranges(pixels + y * pitch, pixels + (y + 1) * pitch, pixels + (m_ScreenDumpBuffer->h - y - 1) * pitch);
	}
}

int FrameMan::SaveIndexedPNG(const char* fileName, BITMAP* bitmapToSave) const {
	if (!bitmapToSave) return -1;
	std::unique_ptr<BITMAP, BitmapDeleter> indexed;
	if (bitmap_color_depth(bitmapToSave) != 8) {
		set_palette(m_DefaultPalette);
		indexed.reset(create_bitmap_ex(8, bitmapToSave->w, bitmapToSave->h));
		if (!indexed) return -1;
		blit(bitmapToSave, indexed.get(), 0, 0, 0, 0, bitmapToSave->w, bitmapToSave->h);
		bitmapToSave = indexed.get();
	}
	std::vector<unsigned char> png;
	if (!ContentFile::EncodeIndexedPNG(bitmapToSave, png)) return -1;
	std::ofstream output(fileName, std::ios::binary);
	output.write(reinterpret_cast<const char*>(png.data()), png.size());
	output.close();
	return output.good() ? 0 : -1;
}

bool FrameMan::RunBitmapSaveSelfTest() {
	bool pass = true;
	const auto check = [&](const std::string& name, bool result) {
		std::cout << "[bitmap-save-selftest] " << name << ": " << (result ? "PASS" : "FAIL") << std::endl;
		pass = pass && result;
	};
	const auto writePixels = [&](const std::string& name, BITMAP* bitmap) {
		std::ofstream out(System::GetScreenshotDirectory() + name + ".ppm", std::ios::binary);
		out << "P6\n" << bitmap->w << " " << bitmap->h << "\n255\n";
		const int depth = bitmap_color_depth(bitmap);
		for (int y = 0; y < bitmap->h; ++y) {
			for (int x = 0; x < bitmap->w; ++x) {
				const int color = getpixel(bitmap, x, y);
				const RGB rgb = depth == 8 ? m_DefaultPalette[color] : RGB{
				    static_cast<unsigned char>(getr_depth(depth, color)), static_cast<unsigned char>(getg_depth(depth, color)),
				    static_cast<unsigned char>(getb_depth(depth, color)), 0};
				out.put(static_cast<char>(rgb.r));
				out.put(static_cast<char>(rgb.g));
				out.put(static_cast<char>(rgb.b));
			}
		}
		check(name + " source pixels", out.good());
	};
	for (int depth: {8, 24, 32}) {
		for (int height: {1, 7}) {
			const std::string name = "bitmap_indexed_" + std::to_string(depth) + "_" + std::to_string(height);
			std::unique_ptr<BITMAP, BitmapDeleter> parent(create_bitmap_ex(depth, 19, height + 2));
			std::unique_ptr<BITMAP, BitmapDeleter> bitmap(parent ? create_sub_bitmap(parent.get(), 3, 1, 13, height) : nullptr);
			if (!bitmap) {
				check(name + " allocation", false);
				continue;
			}
			for (int y = 0; y < height; ++y) {
				for (int x = 0; x < bitmap->w; ++x) {
					const int index = 20 + (x * 7 + y * 13) % 200;
					const RGB& rgb = m_DefaultPalette[index];
					putpixel(bitmap.get(), x, y, depth == 8 ? index : makecol_depth(depth, rgb.r, rgb.g, rgb.b));
				}
			}
			writePixels(name, bitmap.get());
			const std::string file = System::GetScreenshotDirectory() + name + ".png";
			check(name + " save", SaveIndexedPNG(file.c_str(), bitmap.get()) == 0);
			const std::string missing = System::GetScreenshotDirectory() + "missing/" + name + ".png";
			check(name + " failed save", SaveIndexedPNG(missing.c_str(), bitmap.get()) < 0);
		}
	}
	check("world save", SaveWorldToPNG("bitmap_world") == 0);
	if (m_WorldDumpBuffer) writePixels("bitmap_world", m_WorldDumpBuffer.get());
	else check("world source pixels", false);
	check("preview save", SaveWorldPreviewToPNG("bitmap_preview") == 0);
	check("world failed save", SaveWorldToPNG("missing/bitmap_world") < 0);
	check("preview failed save", SaveWorldPreviewToPNG("missing/bitmap_preview") < 0);
	BITMAP* materials = g_SceneMan.GetScene()->GetTerrain()->GetMaterialBitmap();
	for (int x = 0; x < 256; ++x) putpixel(materials, x, 0, x);
	const bool saved = g_ActivityMan.SaveCurrentGame("bitmap_indices") && g_ActivityMan.WaitForSaveGameTask();
	check("material save", saved);
	if (saved && g_ActivityMan.LoadAndLaunchGame("bitmap_indices")) {
		materials = g_SceneMan.GetScene()->GetTerrain()->GetMaterialBitmap();
		bool exact = true;
		for (int x = 0; x < 256; ++x) {
			const int restored = getpixel(materials, x, 0);
			if (restored != x) {
				std::cout << "[bitmap-save-selftest] material " << x << " restored " << restored << std::endl;
				exact = false;
			}
		}
		check("material load indices", exact);
	} else {
		check("material load", false);
	}
	g_ActivityMan.EndActivity();
	check("world outside activity", SaveWorldToPNG("outside_world") < 0);
	check("preview outside activity", SaveWorldPreviewToPNG("outside_preview") < 0);
	std::cout << "[bitmap-save-selftest] " << (pass ? "PASS" : "FAIL") << std::endl;
	return pass;
}

int FrameMan::SharedDrawLine(BITMAP* bitmap, const Vector& start, const Vector& end, int color, int altColor, int skip, int skipStart, bool shortestWrap, bool drawDot, BITMAP* dot) const {
	RTEAssert(bitmap, "Trying to draw line to null Bitmap");
	if (drawDot) {
		RTEAssert(dot, "Trying to draw line of dots without specifying a dot Bitmap");
	}

	int error = 0;
	int dom = 0;
	int sub = 0;
	int domSteps = 0;
	int skipped = skip + (skipStart - skip);
	int intPos[2];
	int delta[2];
	int delta2[2];
	int increment[2];
	bool drawAlt = false;

	int dotHeight = drawDot ? dot->h : 0;
	int dotWidth = drawDot ? dot->w : 0;

	// acquire_bitmap(bitmap);

	// Just make the alt the same color as the main one if no one was specified
	if (altColor == 0) {
		altColor = color;
	}

	intPos[X] = start.GetFloorIntX();
	intPos[Y] = start.GetFloorIntY();

	// Wrap line around the scene if it makes it shorter
	if (shortestWrap) {
		Vector deltaVec = g_SceneMan.ShortestDistance(start, end, false);
		delta[X] = deltaVec.GetFloorIntX();
		delta[Y] = deltaVec.GetFloorIntY();
	} else {
		delta[X] = end.GetFloorIntX() - intPos[X];
		delta[Y] = end.GetFloorIntY() - intPos[Y];
	}
	if (delta[X] == 0 && delta[Y] == 0) {
		return 0;
	}

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
	error = delta2[sub] - delta[dom];

	// Bresenham's line drawing algorithm execution
	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only draw pixel if we're not due to skip any
		if (++skipped > skip) {
			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			if (drawDot) {
				masked_blit(dot, bitmap, 0, 0, intPos[X] - (dotWidth / 2), intPos[Y] - (dotHeight / 2), dot->w, dot->h);
			} else {
				putpixel(bitmap, intPos[X], intPos[Y], drawAlt ? color : altColor);
			}
			drawAlt = !drawAlt;
			skipped = 0;
		}
	}

	// Return the end phase state of the skipping
	return skipped;
}

GUIFont* FrameMan::GetFont(bool isSmall, bool trueColor) {
	size_t colorIndex = trueColor ? 1 : 0;

	if (!m_GUIScreens[colorIndex]) {
		m_GUIScreens[colorIndex] = new AllegroScreen(trueColor ? m_BackBuffer32.get() : m_BackBuffer8.get());
	}

	if (isSmall) {
		if (!m_SmallFonts[colorIndex]) {
			std::string fontName = "SmallFont";
			std::string fontPath = "Base.rte/GUIs/Skins/FontSmall.png";

			if (trueColor) {
				fontName = "SmallFont32";
				fontPath = "Base.rte/GUIs/Skins/Menus/FontSmall.png";
			}
			m_SmallFonts[colorIndex] = new GUIFont(fontName);
			m_SmallFonts[colorIndex]->Load(m_GUIScreens[colorIndex], fontPath);
		}
		return m_SmallFonts[colorIndex];
	}
	if (!m_LargeFonts[colorIndex]) {
		std::string fontName = "FatFont";
		std::string fontPath = "Base.rte/GUIs/Skins/FontLarge.png";

		if (trueColor) {
			fontName = "FatFont32";
			fontPath = "Base.rte/GUIs/Skins/Menus/FontLarge.png";
		}
		m_LargeFonts[colorIndex] = new GUIFont(fontName);
		m_LargeFonts[colorIndex]->Load(m_GUIScreens[colorIndex], fontPath);
	}
	return m_LargeFonts[colorIndex];
}

void FrameMan::UpdateScreenOffsetForSplitScreen(int playerScreen, Vector& screenOffset) const {
	switch (playerScreen) {
		case Players::PlayerTwo:
			// If both splits, or just VSplit, then in upper right quadrant
			if ((m_VSplit && !m_HSplit) || (m_VSplit && m_HSplit)) {
				screenOffset.SetXY(g_WindowMan.GetResX() / 2, 0);
			} else {
				// If only HSplit, then lower left quadrant
				screenOffset.SetXY(0, g_WindowMan.GetResY() / 2);
			}
			break;
		case Players::PlayerThree:
			// Always lower left quadrant
			screenOffset.SetXY(0, g_WindowMan.GetResY() / 2);
			break;
		case Players::PlayerFour:
			// Always lower right quadrant
			screenOffset.SetXY(g_WindowMan.GetResX() / 2, g_WindowMan.GetResY() / 2);
			break;
		default:
			// Always upper left corner
			screenOffset.SetXY(0, 0);
			break;
	}
}

void FrameMan::Draw() {
	ZoneScopedN("Draw");
	TracyGpuZone("FrameMan::Draw");

	// rlSetShader(rlGetShaderIdDefault(), rlGetShaderLocsDefault());
	Shader backgroundShader;
	g_PresetMan.GetEntityPreset("Shader", "Background")->Clone(&backgroundShader);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	clear_to_color(m_BackBuffer8.get(), 0);
	m_BackBuffer->Begin(true);

	// Count how many split screens we'll need
	int screenCount = (m_HSplit ? 2 : 1) * (m_VSplit ? 2 : 1);
	RTEAssert(screenCount <= 1 || m_PlayerScreen, "Splitscreen surface not ready when needed!");

	g_PostProcessMan.ClearScreenPostEffects();

	// These accumulate the effects for each player's screen area, and are then transferred to the post-processing lists with the player screen offset applied
	std::list<PostEffect> screenRelativeEffects;
	std::list<Box> screenRelativeGlowBoxes;

	const Activity* pActivity = g_ActivityMan.GetActivity();

	for (int playerScreen = 0; playerScreen < screenCount; ++playerScreen) {
		screenRelativeEffects.clear();
		screenRelativeGlowBoxes.clear();
		rlEnableColorBlend();
		rlSetBlendMode(RL_BLEND_ALPHA);
		rlEnableDepthTest();

		m_PlayerScreen->Begin(true, 1.0f);
		backgroundShader.Begin();
		backgroundShader.Enable();
		rlSetUniformSampler(backgroundShader.GetUniformLocation("rtePalette"), g_PostProcessMan.GetPaletteTexture());
		backgroundShader.SetInt("drawMasked", 1);

		// rlSetUniformSampler(backgroundShader.GetUniformLocation("rtePalette"), g_PostProcessMan.GetPaletteTexture());
		BITMAP* drawScreen = (screenCount == 1) ? m_BackBuffer8.get() : m_PlayerScreen8.get();
		BITMAP* drawScreenGUI = (screenCount == 1) ? m_BackBuffer8.get() : m_PlayerScreen8.get();
		// Need to clear the backbuffers because Scene background layers can be too small to fill the whole backbuffer or drawn masked resulting in artifacts from the previous frame.
		clear_to_color(drawScreenGUI, ColorKeys::g_MaskColor);
		// If in online multiplayer mode clear to mask color otherwise the scene background layers will get drawn over.
		clear_to_color(drawScreen, 0);

		AllegroBitmap playerGUIBitmap(drawScreenGUI);

		// Update the scene view to line up with a specific screen and then draw it onto the intermediate screen
		g_CameraMan.Update(playerScreen);
		g_SceneMan.Update(playerScreen);

		Vector targetPos = g_CameraMan.GetRenderOffset(playerScreen);

		// Adjust the drawing position on the target screen for if the target screen is larger than the scene in non-wrapping dimension.
		// Scene needs to be displayed centered on the target bitmap then, and that has to be adjusted for when drawing to the screen
		if (!g_SceneMan.SceneWrapsX() && drawScreen->w > g_SceneMan.GetSceneWidth()) {
			targetPos.m_X += (drawScreen->w - g_SceneMan.GetSceneWidth()) / 2;
		}
		if (!g_SceneMan.SceneWrapsY() && drawScreen->h > g_SceneMan.GetSceneHeight()) {
			targetPos.m_Y += (drawScreen->h - g_SceneMan.GetSceneHeight()) / 2;
		}

		// Draw the scene
		g_SceneMan.Draw(drawScreen, drawScreenGUI, targetPos);

		g_PrimitiveMan.DrawPrimitives(playerScreen, drawScreenGUI, targetPos);

		// Get only the scene-relative post effects that affect this player's screen
		if (pActivity) {
			g_PostProcessMan.GetPostScreenEffectsWrapped(targetPos, drawScreen->w, drawScreen->h, screenRelativeEffects, pActivity->GetTeamOfPlayer(pActivity->PlayerOfScreen(playerScreen)));
			g_PostProcessMan.GetGlowAreasWrapped(targetPos, drawScreen->w, drawScreen->h, screenRelativeGlowBoxes);
		}

		// TODO: Find out what keeps disabling the clipping on the draw bitmap
		// Enable clipping on the draw bitmap
		set_clip_state(drawScreen, 1);

		DrawScreenText(playerScreen, playerGUIBitmap);

		// The position of the current draw screen on the backbuffer
		Vector screenOffset;

		// If we are dealing with split screens, then deal with the fact that we need to draw the player screens to different locations on the final buffer
		if (screenCount > 1) {
			UpdateScreenOffsetForSplitScreen(playerScreen, screenOffset);
		}

		DrawScreenFlash(playerScreen, drawScreenGUI);

		// Draw the intermediate draw splitscreen to the appropriate spot on the back buffer
		blit(drawScreen, m_BackBuffer8.get(), 0, 0, screenOffset.GetFloorIntX(), screenOffset.GetFloorIntY(), drawScreen->w, drawScreen->h);
		m_PlayerScreen->End();
		backgroundShader.End();
		if (screenCount > 1) {
			m_BackBuffer->Begin(false);
			DrawTextureRec(m_PlayerScreen->GetColorTexture(), {0, 0, static_cast<float>(m_PlayerScreen8->w), -static_cast<float>(m_PlayerScreen8->h)}, {screenOffset.m_X, screenOffset.m_Y}, {255, 255, 255, 255});
			m_BackBuffer->End();
		}
		g_PostProcessMan.AdjustEffectsPosToPlayerScreen(playerScreen, drawScreen, screenOffset, screenRelativeEffects, screenRelativeGlowBoxes);
	}

	// Clears the pixels that have been revealed from the unseen layers
	g_SceneMan.ClearSeenPixels();

	// Draw separating lines for split-screens
	if (m_HSplit) {
		hline(m_BackBuffer8.get(), 0, (m_BackBuffer8->h / 2) - 1, m_BackBuffer8->w - 1, m_AlmostBlackColor);
		hline(m_BackBuffer8.get(), 0, (m_BackBuffer8->h / 2), m_BackBuffer8->w - 1, m_AlmostBlackColor);
	}
	if (m_VSplit) {
		vline(m_BackBuffer8.get(), (m_BackBuffer8->w / 2) - 1, 0, m_BackBuffer8->h - 1, m_AlmostBlackColor);
		vline(m_BackBuffer8.get(), (m_BackBuffer8->w / 2), 0, m_BackBuffer8->h - 1, m_AlmostBlackColor);
	}

	rlEnableDepthTest();
	rlZDepth(c_GuiDepth - 1.0f);
	g_GLResourceMan.UpdateDynamicBitmap(m_BackBuffer8.get(), true);
	backgroundShader.Begin();
	backgroundShader.Enable();
	rlSetUniformSampler(backgroundShader.GetUniformLocation("rtePalette"), g_PostProcessMan.GetPaletteTexture());
	backgroundShader.SetInt("drawMasked", 1);
	m_BackBuffer->Begin(false);
	DrawTexture(g_GLResourceMan.GetStaticTextureFromBitmap(m_BackBuffer8.get()), 0.0f, 0.0f, {255, 255, 255, 255});
	m_BackBuffer->End();
	backgroundShader.End();
	rlZDepth(0);
	if (g_ActivityMan.IsInActivity()) {
		g_PostProcessMan.PostProcess();
	}

	// Draw the performance stats and console on top of everything.
	g_PerformanceMan.Draw(m_BackBuffer32.get());
	g_ConsoleMan.Draw(m_BackBuffer32.get());

#ifdef DEBUG_BUILD
	// Draw scene seam
	vline(m_BackBuffer8.get(), 0, 0, g_SceneMan.GetSceneHeight(), 5);
#endif
}

void FrameMan::DrawScreenText(int playerScreen, AllegroBitmap playerGUIBitmap) {
	int textPosY = 0;
	// Only draw screen text to actual human players
	if (playerScreen < g_ActivityMan.GetActivity()->GetHumanCount()) {
		textPosY += 12;

		if (!m_ScreenText[playerScreen].empty()) {
			int bufferOrScreenWidth = GetPlayerScreenWidth();
			int bufferOrScreenHeight = GetPlayerScreenHeight();

			if (m_TextCentered[playerScreen]) {
				textPosY = (bufferOrScreenHeight / 2) - 52;
			}

			int screenOcclusionOffsetX = g_CameraMan.GetScreenOcclusion(playerScreen).GetRoundIntX();
			// If there's really no room to offset the text into, then don't
			if (GetPlayerScreenWidth() <= g_WindowMan.GetResX() / 2) {
				screenOcclusionOffsetX = 0;
			}

			std::string screenTextToDraw = m_ScreenText[playerScreen];
			if (m_TextBlinking[playerScreen] && m_TextBlinkTimer.AlternateReal(m_TextBlinking[playerScreen])) {
				screenTextToDraw = ">>> " + screenTextToDraw + " <<<";
			}
			screenTextToDraw = SplitStringToFitWidth(screenTextToDraw, bufferOrScreenWidth, false);
			GetLargeFont()->DrawAligned(&playerGUIBitmap, (bufferOrScreenWidth + screenOcclusionOffsetX) / 2, textPosY, screenTextToDraw, GUIFont::Centre);
			textPosY += 12;
		}

		// Draw info text when in MOID or material layer draw mode
		switch (g_SceneMan.GetLayerDrawMode()) {
			case g_LayerTerrainMatter:
				GetSmallFont()->DrawAligned(&playerGUIBitmap, GetPlayerScreenWidth() / 2, GetPlayerScreenHeight() - 12, "Viewing terrain material layer\nHit Ctrl+M to cycle modes", GUIFont::Centre, GUIFont::Bottom);
				break;
			default:
				break;
		}
	} else {
		// If superfluous screen (as in a three-player match), make the fourth the Observer one
		GetLargeFont()->DrawAligned(&playerGUIBitmap, GetPlayerScreenWidth() / 2, textPosY, "- Observer View -", GUIFont::Centre);
	}
}

void FrameMan::DrawScreenFlash(int playerScreen, BITMAP* playerGUIBitmap) {
	if (m_FlashScreenColor[playerScreen] != -1) {
		// If set to flash for a period of time, first be solid and then start flashing slower
		double timeTillLimit = m_FlashTimer[playerScreen].LeftTillRealTimeLimitMS();

		if (timeTillLimit < 10 || m_FlashTimer[playerScreen].AlternateReal(50)) {
			if (m_FlashedLastFrame[playerScreen]) {
				m_FlashedLastFrame[playerScreen] = false;
			} else {
				rlZDepth(c_GuiDepth);
				rlBegin(RL_QUADS);

				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 50);
				rlVertex2f(playerGUIBitmap->w * .25f, playerGUIBitmap->h * .25f);
				rlVertex2f(playerGUIBitmap->w - playerGUIBitmap->w * .25f, playerGUIBitmap->h * 0.25f);
				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 255);
				rlVertex2f(playerGUIBitmap->w, 0.0f);
				rlVertex2f(0.0f, 0.0f);

				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 50);
				rlVertex2f(playerGUIBitmap->w * .25f, playerGUIBitmap->h - playerGUIBitmap->h * .25f);
				rlVertex2f(playerGUIBitmap->w * .25f, playerGUIBitmap->h * 0.25f);
				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 255);
				rlVertex2f(0.0f, 0.0f);
				rlVertex2f(0.0f, playerGUIBitmap->h);

				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 50);
				rlVertex2f(playerGUIBitmap->w - playerGUIBitmap->w * .25f, playerGUIBitmap->h - playerGUIBitmap->h * .25f);
				rlVertex2f(playerGUIBitmap->w * .25f, playerGUIBitmap->h - playerGUIBitmap->h * .25f);
				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 255);
				rlVertex2f(0.0f, playerGUIBitmap->h);
				rlVertex2f(playerGUIBitmap->w, playerGUIBitmap->h);

				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 50);
				rlVertex2f(playerGUIBitmap->w - playerGUIBitmap->w * .25f, playerGUIBitmap->h * .25f);
				rlVertex2f(playerGUIBitmap->w - playerGUIBitmap->w * .25f, playerGUIBitmap->h - playerGUIBitmap->h * .25f);
				rlColor4ub(m_FlashScreenColor[playerScreen], 0, 0, 255);
				rlVertex2f(playerGUIBitmap->w, playerGUIBitmap->h);
				rlVertex2f(playerGUIBitmap->w, 0.0f);

				rlEnd();
				rlZDepth(c_DefaultDrawDepth);
				m_FlashedLastFrame[playerScreen] = true;
			}
		}
		if (m_FlashTimer[playerScreen].IsPastRealTimeLimit()) {
			m_FlashScreenColor[playerScreen] = -1;
		}
	}
}

void FrameMan::DrawWorldDump(bool drawForScenePreview) const {
	float worldBitmapWidth = static_cast<float>(m_WorldDumpBuffer->w);
	float worldBitmapHeight = static_cast<float>(m_WorldDumpBuffer->h);

	// Draw sky gradient if we're not dumping a scene preview
	if (!drawForScenePreview) {
		clear_to_color(m_WorldDumpBuffer.get(), makecol32(132, 192, 252)); // Light blue color
		for (int i = 0; i < m_WorldDumpBuffer->h; i++) {
			int lineColor = makecol32(64 + ((static_cast<float>(i) / worldBitmapHeight) * (128 - 64)), 64 + ((static_cast<float>(i) / worldBitmapHeight) * (192 - 64)), 96 + ((static_cast<float>(i) / worldBitmapHeight) * (255 - 96)));
			hline(m_WorldDumpBuffer.get(), 0, i, worldBitmapWidth - 1, lineColor);
		}
	} else {
		clear_to_color(m_WorldDumpBuffer.get(), makecol32(255, 0, 255)); // Magenta
	}

	// Draw scene
	draw_sprite(m_WorldDumpBuffer.get(), g_SceneMan.GetTerrain()->GetBGColorBitmap(), 0, 0);
	draw_sprite(m_WorldDumpBuffer.get(), g_SceneMan.GetTerrain()->GetFGColorBitmap(), 0, 0);

	// If we're not dumping a scene preview, draw objects and post-effects.
	if (!drawForScenePreview) {
		std::list<PostEffect> postEffectsList;
		BITMAP* effectBitmap = nullptr;
		int effectPosX = 0;
		int effectPosY = 0;
		int effectStrength = 0;
		Vector targetPos(0, 0);

		// Draw objects
		draw_sprite(m_WorldDumpBuffer.get(), g_SceneMan.GetMOColorBitmap(), 0, 0);

		// Draw post-effects
		g_PostProcessMan.GetPostScreenEffectsWrapped(targetPos, worldBitmapWidth, worldBitmapHeight, postEffectsList, -1);

		for (const PostEffect& postEffect: postEffectsList) {
			effectBitmap = postEffect.m_Bitmap;
			effectStrength = postEffect.m_Strength;
			set_screen_blender(effectStrength, effectStrength, effectStrength, effectStrength);
			effectPosX = postEffect.m_Pos.GetFloorIntX() - (effectBitmap->w / 2);
			effectPosY = postEffect.m_Pos.GetFloorIntY() - (effectBitmap->h / 2);

			if (postEffect.m_Angle == 0.0F) {
				draw_trans_sprite(m_WorldDumpBuffer.get(), effectBitmap, effectPosX, effectPosY);
			} else {
				BITMAP* targetBitmap = g_PostProcessMan.GetTempEffectBitmap(effectBitmap);
				clear_to_color(targetBitmap, 0);

				Matrix newAngle(postEffect.m_Angle);
				rotate_sprite(targetBitmap, effectBitmap, 0, 0, ftofix(newAngle.GetAllegroAngle()));
				draw_trans_sprite(m_WorldDumpBuffer.get(), targetBitmap, effectPosX, effectPosY);
			}
		}
	}
}

namespace {
	const std::vector<std::pair<std::string, BLENDER_FUNC>>& CheckpointBlenders() {
		static const std::vector<std::pair<std::string, BLENDER_FUNC>> functions = {
			{"", nullptr}, {"black", _blender_black}, {"true_alpha", RTE::TrueAlphaBlender},
#define CHECKPOINT_BLENDER(name) {#name, _blender_##name}
#ifdef ALLEGRO_COLOR16
			CHECKPOINT_BLENDER(trans15), CHECKPOINT_BLENDER(add15), CHECKPOINT_BLENDER(burn15), CHECKPOINT_BLENDER(color15),
			CHECKPOINT_BLENDER(difference15), CHECKPOINT_BLENDER(dissolve15), CHECKPOINT_BLENDER(dodge15), CHECKPOINT_BLENDER(hue15),
			CHECKPOINT_BLENDER(invert15), CHECKPOINT_BLENDER(luminance15), CHECKPOINT_BLENDER(multiply15), CHECKPOINT_BLENDER(saturation15), CHECKPOINT_BLENDER(screen15),
			CHECKPOINT_BLENDER(trans16), CHECKPOINT_BLENDER(add16), CHECKPOINT_BLENDER(burn16), CHECKPOINT_BLENDER(color16),
			CHECKPOINT_BLENDER(difference16), CHECKPOINT_BLENDER(dissolve16), CHECKPOINT_BLENDER(dodge16), CHECKPOINT_BLENDER(hue16),
			CHECKPOINT_BLENDER(invert16), CHECKPOINT_BLENDER(luminance16), CHECKPOINT_BLENDER(multiply16), CHECKPOINT_BLENDER(saturation16), CHECKPOINT_BLENDER(screen16),
#endif
#if defined(ALLEGRO_COLOR24) || defined(ALLEGRO_COLOR32)
			CHECKPOINT_BLENDER(trans24), CHECKPOINT_BLENDER(add24), CHECKPOINT_BLENDER(burn24), CHECKPOINT_BLENDER(color24),
			CHECKPOINT_BLENDER(difference24), CHECKPOINT_BLENDER(dissolve24), CHECKPOINT_BLENDER(dodge24), CHECKPOINT_BLENDER(hue24),
			CHECKPOINT_BLENDER(invert24), CHECKPOINT_BLENDER(luminance24), CHECKPOINT_BLENDER(multiply24), CHECKPOINT_BLENDER(saturation24), CHECKPOINT_BLENDER(screen24),
#endif
			CHECKPOINT_BLENDER(alpha15), CHECKPOINT_BLENDER(alpha16), CHECKPOINT_BLENDER(alpha24), CHECKPOINT_BLENDER(alpha32), CHECKPOINT_BLENDER(write_alpha)
#undef CHECKPOINT_BLENDER
		};
		return functions;
	}
	std::string CheckpointBlenderName(BLENDER_FUNC function) {
		for (const auto& [name, value]: CheckpointBlenders()) if (value == function) return name;
		throw std::runtime_error("unregistered native color blender");
	}
	BLENDER_FUNC CheckpointBlenderFunction(const std::string& name) {
		for (const auto& [candidate, value]: CheckpointBlenders()) if (name == candidate) return value;
		throw std::runtime_error("invalid color blender checkpoint");
	}
}

std::string FrameMan::SavePaletteCheckpoint() const {
	static_assert(sizeof(RGB) == 4);
	CheckpointWriter writer("FramePalette1");
	auto bytes = [](const auto& value) { return std::string(reinterpret_cast<const char*>(&value), sizeof(value)); };
	PALETTE current; get_palette(current);
	writer(m_PaletteFile, bytes(m_Palette), bytes(m_DefaultPalette), bytes(current), bytes(m_RGBTable), m_BlackColor, m_AlmostBlackColor, m_CurrentAlpha, m_ColorTablePruneTimer);
	int selectedMode = color_map ? -2 : -1;
	std::array<int, 4> selectedKey{};
	for (size_t mode = 0; mode < m_ColorTables.size(); ++mode) {
		std::map<std::array<int, 4>, std::pair<std::string, long long>> entries;
		for (const auto& [key, value]: m_ColorTables[mode]) {
			entries.emplace(key, std::make_pair(bytes(value.first), value.second));
			if (color_map == &value.first) { selectedMode = mode; selectedKey = key; }
		}
		writer(entries);
	}
	writer(selectedMode, selectedKey, selectedMode == -2 ? bytes(*color_map) : std::string{});
	for (const auto function: {_blender_func15, _blender_func16, _blender_func24, _blender_func32, _blender_func15x, _blender_func16x, _blender_func24x}) writer(CheckpointBlenderName(function));
	writer(_blender_col_15, _blender_col_16, _blender_col_24, _blender_col_32, _blender_alpha);
	return writer.Text();
}

bool FrameMan::LoadPaletteCheckpoint(std::string_view text, bool validateOnly) {
	try {
		struct State {
			std::string file, palette, defaultPalette, currentPalette, rgb;
			int black, almostBlack, alpha, selectedMode;
			Timer prune;
			std::array<std::unordered_map<std::array<int, 4>, std::pair<COLOR_MAP, long long>>, DrawBlendMode::BlendModeCount> tables;
			std::array<int, 4> selectedKey;
			std::unique_ptr<COLOR_MAP> external;
			std::array<BLENDER_FUNC, 7> blenders;
			std::array<int, 5> blendValues;
		};
		auto state = std::make_shared<State>();
		CheckpointReader reader(text, "FramePalette1", validateOnly);
		reader.Value(state->file);
		if (!m_PaletteFile.LoadCheckpoint(state->file, true)) return false;
		auto bytes = [&](std::string& value, size_t size) { reader.Value(value); if (value.size() != size) throw std::runtime_error("invalid palette byte count"); };
		bytes(state->palette, sizeof(PALETTE)); bytes(state->defaultPalette, sizeof(PALETTE)); bytes(state->currentPalette, sizeof(PALETTE)); bytes(state->rgb, sizeof(RGB_MAP));
		reader.Value(state->black); reader.Value(state->almostBlack); reader.Value(state->alpha); reader.Value(state->prune);
		for (auto& target: state->tables) {
			std::map<std::array<int, 4>, std::pair<std::string, long long>> records; reader.Value(records);
			for (const auto& [key, value]: records) {
				if (value.first.size() != sizeof(COLOR_MAP)) throw std::runtime_error("invalid color table byte count");
				if (!validateOnly) { auto& entry = target[key]; std::memcpy(&entry.first, value.first.data(), sizeof(COLOR_MAP)); entry.second = value.second; }
				else target.try_emplace(key);
			}
		}
		reader.Value(state->selectedMode); reader.Value(state->selectedKey);
		std::string external; reader.Value(external);
		if (state->selectedMode < -2 || state->selectedMode >= DrawBlendMode::BlendModeCount || (state->selectedMode == -2 ? external.size() != sizeof(COLOR_MAP) : !external.empty()) ||
		    (state->selectedMode >= 0 && !state->tables[state->selectedMode].contains(state->selectedKey))) throw std::runtime_error("invalid active color table reference");
		if (state->selectedMode == -2 && !validateOnly) { state->external = std::make_unique<COLOR_MAP>(); std::memcpy(state->external.get(), external.data(), sizeof(COLOR_MAP)); }
		for (auto& function: state->blenders) { std::string name; reader.Value(name); function = CheckpointBlenderFunction(name); }
		reader.Value(state->blendValues);
		reader.OnCommit([this, state] {
			if (!m_PaletteFile.LoadCheckpoint(state->file)) throw std::runtime_error("could not restore palette file");
			std::memcpy(m_Palette, state->palette.data(), sizeof(PALETTE)); std::memcpy(m_DefaultPalette, state->defaultPalette.data(), sizeof(PALETTE));
			PALETTE current; std::memcpy(current, state->currentPalette.data(), sizeof(PALETTE)); set_palette(current);
			std::memcpy(&m_RGBTable, state->rgb.data(), sizeof(RGB_MAP));
			m_BlackColor = state->black; m_AlmostBlackColor = state->almostBlack; m_CurrentAlpha = state->alpha; m_ColorTablePruneTimer = state->prune;
			m_ColorTables.swap(state->tables); m_CheckpointColorTable.swap(state->external);
			color_map = state->selectedMode == -1 ? nullptr : state->selectedMode == -2 ? m_CheckpointColorTable.get() : &m_ColorTables[state->selectedMode].at(state->selectedKey).first;
			_blender_func15 = state->blenders[0]; _blender_func16 = state->blenders[1]; _blender_func24 = state->blenders[2]; _blender_func32 = state->blenders[3];
			_blender_func15x = state->blenders[4]; _blender_func16x = state->blenders[5]; _blender_func24x = state->blenders[6];
			_blender_col_15 = state->blendValues[0]; _blender_col_16 = state->blendValues[1]; _blender_col_24 = state->blendValues[2]; _blender_col_32 = state->blendValues[3]; _blender_alpha = state->blendValues[4];
		});
		reader.Finish();
		return true;
	} catch (const std::exception& error) { std::cerr << "[palette-checkpoint] " << error.what() << std::endl; return false; }
}

bool FrameMan::RunPaletteCheckpointSelfTest() {
	const auto original = SavePaletteCheckpoint();
	struct Restore { FrameMan& manager; const std::string& value; ~Restore() { manager.LoadPaletteCheckpoint(value, false); } } restore{*this, original};
	bool passed = true;
	auto check = [&](bool value, const char* name) { passed = value && passed; std::cout << "[palette-checkpoint-selftest] " << (value ? "PASS " : "FAIL ") << name << std::endl; };
	PALETTE current; get_palette(current);
	current[17] = {11, 23, 37, 0}; set_palette(current);
	m_Palette[19] = {41, 53, 61, 0}; m_DefaultPalette[21] = {17, 29, 43, 0};
	m_BlackColor = 31; m_AlmostBlackColor = 37; m_CurrentAlpha = 151; m_RGBTable.data[7][11][13] = 179;
	m_ColorTablePruneTimer.SetSimTimeLimitTicks(791); m_ColorTablePruneTimer.SetRealTimeLimitTicks(919);
	const std::array<int, 4> key{20, 30, 40, 50};
	auto& table = m_ColorTables[DrawBlendMode::BlendScreen][key];
	for (int first = 0; first < 256; ++first) for (int second = 0; second < 256; ++second) table.first.data[first][second] = (first * 13 + second * 7) & 255;
	table.second = 1234567; color_map = &table.first;
	set_screen_blender(37, 59, 83, 107);
	const auto checkpoint = SavePaletteCheckpoint();
	const auto blend = _blender_func24(makecol24(91, 123, 177), makecol24(33, 71, 119), _blender_alpha);
	check(LoadPaletteCheckpoint(checkpoint, true), "validate_palette_and_tables");
	set_palette(black_palette); m_Palette[19] = {}; m_DefaultPalette[21] = {}; m_RGBTable.data[7][11][13] = 0;
	m_BlackColor = m_AlmostBlackColor = 0; m_CurrentAlpha = 255; table.first.data[17][31] = 0; table.second = -1; set_trans_blender(199, 181, 163, 145);
	check(LoadPaletteCheckpoint(checkpoint, false) && SavePaletteCheckpoint() == checkpoint, "all_palette_state_restored");
	RGB observed; get_color(17, &observed);
	check(observed.r == 11 && observed.g == 23 && observed.b == 37 && m_Palette[19].r == 41 && m_DefaultPalette[21].g == 29 && m_RGBTable.data[7][11][13] == 179, "palette_arrays_and_rgb_observation");
	check(color_map == &m_ColorTables[DrawBlendMode::BlendScreen].at(key).first && color_map->data[17][31] == ((17 * 13 + 31 * 7) & 255) && m_CurrentAlpha == 151, "active_table_alias_and_alpha");
	check(_blender_func24(makecol24(91, 123, 177), makecol24(33, 71, 119), _blender_alpha) == blend && _blender_alpha == 107, "blender_continuation");
	check(!LoadPaletteCheckpoint(checkpoint + "x", false) && SavePaletteCheckpoint() == checkpoint, "trailing_palette_rejection_atomic");
	check(!LoadPaletteCheckpoint(checkpoint.substr(0, checkpoint.size() - 1), false) && SavePaletteCheckpoint() == checkpoint, "truncated_palette_rejection_atomic");
	COLOR_MAP external{}; external.data[29][43] = 137; color_map = &external; SetTrueAlphaBlender();
	const auto externalState = SavePaletteCheckpoint(); external.data[29][43] = 0; color_map = nullptr; set_alpha_blender();
	check(LoadPaletteCheckpoint(externalState, false) && color_map && color_map->data[29][43] == 137 && _blender_func32 == TrueAlphaBlender && SavePaletteCheckpoint() == externalState, "external_table_and_true_alpha_owner");
	check(LoadPaletteCheckpoint(original, false) && SavePaletteCheckpoint() == original, "original_palette_restored");
	return passed;
}

std::string FrameMan::SaveCheckpoint() const {
	CheckpointWriter writer("FrameMan3");
	writer(m_HSplit, m_VSplit);
	VisitCheckpoint(writer, *this);
	for (const auto* font: m_SmallFonts) writer(font ? GUICheckpoint::SaveFont(*font) : std::string{});
	for (const auto* font: m_LargeFonts) writer(font ? GUICheckpoint::SaveFont(*font) : std::string{});
	writer(SavePaletteCheckpoint());
	return writer.Text();
}

std::string FrameMan::SaveNetLocalState() const {
	CheckpointWriter writer("FrameManLocal1");
	writer(m_HSplit, m_VSplit);
	VisitCheckpoint(writer, *this);
	return writer.Text();
}

bool FrameMan::LoadNetLocalState(std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "FrameManLocal1", validateOnly);
		bool hsplit, vsplit;
		reader.Value(hsplit); reader.Value(vsplit);
		reader.OnCommit([this, hsplit, vsplit] { if (m_HSplit != hsplit || m_VSplit != vsplit) ResetSplitScreens(hsplit, vsplit); });
		VisitCheckpoint(reader, *this);
		reader.Finish();
		return true;
	} catch (const std::exception&) { return false; }
}

bool FrameMan::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		const bool legacy = text.starts_with("9 FrameMan1 ");
		const bool version2 = text.starts_with("9 FrameMan2 ");
		CheckpointReader reader(text, legacy ? "FrameMan1" : version2 ? "FrameMan2" : "FrameMan3", validateOnly);
		bool hsplit, vsplit;
		reader.Value(hsplit); reader.Value(vsplit);
		reader.OnCommit([this, hsplit, vsplit] { if (m_HSplit != hsplit || m_VSplit != vsplit) ResetSplitScreens(hsplit, vsplit); });
		VisitCheckpoint(reader, *this);
		if (!legacy) {
			std::array<std::string, 4> fonts;
			reader.Value(fonts);
			for (const auto& state: fonts) if (!state.empty()) {
				GUIFont validator("");
				if (!GUICheckpoint::LoadFont(validator, state, true)) return false;
			}
			if (!validateOnly) {
				struct FontDeleter { void operator()(GUIFont* font) const { if (font) { font->Destroy(); delete font; } } };
				using OwnedFont = std::unique_ptr<GUIFont, FontDeleter>;
				auto replacements = std::make_shared<std::array<OwnedFont, 4>>();
				auto screens = std::make_shared<std::array<std::unique_ptr<AllegroScreen>, 2>>();
				for (size_t index = 0; index < fonts.size(); ++index) if (!fonts[index].empty()) {
					const size_t depth = index % 2;
					if (!m_GUIScreens[depth] && !(*screens)[depth]) (*screens)[depth] = std::make_unique<AllegroScreen>(depth ? m_BackBuffer32.get() : m_BackBuffer8.get());
					auto& font = (*replacements)[index];
					font.reset(new GUIFont(""));
					font->m_Screen = m_GUIScreens[depth] ? m_GUIScreens[depth] : (*screens)[depth].get();
					if (!GUICheckpoint::LoadFont(*font, fonts[index], false)) return false;
				}
				reader.OnCommit([this, replacements, screens] {
					for (size_t depth = 0; depth < screens->size(); ++depth) if ((*screens)[depth]) m_GUIScreens[depth] = (*screens)[depth].release();
					for (size_t index = 0; index < replacements->size(); ++index) {
						auto*& target = index < 2 ? m_SmallFonts[index] : m_LargeFonts[index - 2];
						auto& candidate = (*replacements)[index];
						if (target && candidate) std::swap(*target, *candidate);
						else { FontDeleter{}(target); target = candidate.release(); }
					}
				});
			}
		}
		if (!legacy && !version2) {
			std::string palette; reader.Value(palette);
			if (!LoadPaletteCheckpoint(palette, true)) return false;
			reader.OnCommit([this, palette] { if (!LoadPaletteCheckpoint(palette, false)) throw std::runtime_error("could not restore palette checkpoint"); });
		}
		reader.Finish();
		return true;
	} catch (const std::exception& error) { std::cerr << "[frame-checkpoint] " << error.what() << std::endl; return false; }
}
