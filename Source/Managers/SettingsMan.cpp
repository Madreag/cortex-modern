#include "SettingsMan.h"
#include "ConsoleMan.h"
#include "CameraMan.h"
#include "MovableMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "PostProcessMan.h"
#include "AudioMan.h"
#include "PerformanceMan.h"
#include "UInputMan.h"
#include "NetMatchService.h"
#include "NetMatchConfig.h"
#include "System.h"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <map>
#include <random>
#include <unordered_set>
#include <utility>

using namespace RTE;

namespace {

	int g_UnknownEnumWarnings = 0;

	std::string LowerAscii(std::string text) {
		for (char& character: text) {
			if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
		}
		return text;
	}

	std::string TrimCopy(const std::string& text) {
		const size_t begin = text.find_first_not_of(" \t\r\n");
		return begin == std::string::npos ? std::string() : text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
	}

	void WarnUnknownOnce(const char* key, const std::string& value) {
		static std::unordered_set<std::string> warned;
		if (!warned.insert(key).second) return;
		++g_UnknownEnumWarnings;
		const std::string message = std::string("WARNING: Unknown ") + key + " value '" + value + "'; using default";
		if (ConsoleMan::IsConstructed()) g_ConsoleMan.PrintString(message);
		else std::cout << message << std::endl;
	}

	template <typename Enum> Enum ParseEnum(const std::string& raw, std::initializer_list<std::pair<const char*, Enum>> table, Enum fallback, const char* key) {
		const std::string value = LowerAscii(TrimCopy(raw));
		for (const auto& [name, parsed]: table) {
			if (value == name) return parsed;
		}
		WarnUnknownOnce(key, raw);
		return fallback;
	}

	bool ValidUtf8DisplayName(const std::string& name) {
		if (name.empty() || name.size() > 64) return false;
		size_t characters = 0;
		for (size_t index = 0; index < name.size();) {
			const unsigned char lead = static_cast<unsigned char>(name[index]);
			size_t need = 0;
			if (lead < 0x80) {
				if (lead < 0x20 || lead == 0x7F) return false;
			} else if (lead < 0xC2) {
				return false;
			} else if (lead < 0xE0) {
				need = 1;
			} else if (lead < 0xF0) {
				need = 2;
			} else if (lead < 0xF5) {
				need = 3;
			} else {
				return false;
			}
			if (index + 1 + need > name.size()) return false;
			for (size_t trail = 1; trail <= need; ++trail) {
				if ((static_cast<unsigned char>(name[index + trail]) & 0xC0) != 0x80) return false;
			}
			if (need >= 1) {
				const unsigned char next = static_cast<unsigned char>(name[index + 1]);
				if (lead == 0xE0 && next < 0xA0) return false;
				if (lead == 0xED && next >= 0xA0) return false;
				if (lead == 0xF0 && next < 0x90) return false;
				if (lead == 0xF4 && next >= 0x90) return false;
			}
			if (++characters > 24) return false;
			index += 1 + need;
		}
		return true;
	}

	bool ValidDiagnosticsDirectory(const std::string& directory) {
		for (unsigned char character: directory) {
			if (character < 0x20 || character == 0x7F) return false;
		}
		return true;
	}

	using MatchMode = SettingsMan::NetworkMatchStatusMode;
	using ChatScope = SettingsMan::NetworkChatDefaultScope;
	using ChatSize = SettingsMan::NetworkChatTextSize;
	using DelayPolicy = SettingsMan::NetworkHostDelayPolicy;
	using Visibility = SettingsMan::NetworkHostVisibility;

	MatchMode ParseMatchStatusMode(const std::string& raw) { return ParseEnum(raw, {{"off", MatchMode::Off}, {"auto", MatchMode::Auto}, {"always", MatchMode::Always}}, MatchMode::Auto, "NetworkMatchStatusMode"); }
	ChatScope ParseChatDefaultScope(const std::string& raw) { return ParseEnum(raw, {{"all", ChatScope::All}, {"team", ChatScope::Team}}, ChatScope::All, "NetworkChatDefaultScope"); }
	ChatSize ParseChatTextSize(const std::string& raw) { return ParseEnum(raw, {{"small", ChatSize::Small}, {"large", ChatSize::Large}}, ChatSize::Small, "NetworkChatTextSize"); }
	DelayPolicy ParseHostDelayPolicy(const std::string& raw) { return ParseEnum(raw, {{"auto", DelayPolicy::Auto}, {"fixed", DelayPolicy::Fixed}}, DelayPolicy::Auto, "NetworkHostDelayPolicy"); }
	Visibility ParseHostVisibility(const std::string& raw) { return ParseEnum(raw, {{"lan", Visibility::LAN}, {"listed", Visibility::Listed}, {"unlisted", Visibility::Unlisted}}, Visibility::LAN, "NetworkHostVisibility"); }

	const char* MatchStatusText(MatchMode mode) { return mode == MatchMode::Off ? "Off" : (mode == MatchMode::Always ? "Always" : "Auto"); }
	const char* ChatScopeText(ChatScope scope) { return scope == ChatScope::Team ? "Team" : "All"; }
	const char* ChatSizeText(ChatSize size) { return size == ChatSize::Large ? "Large" : "Small"; }
	const char* DelayPolicyText(DelayPolicy policy) { return policy == DelayPolicy::Fixed ? "Fixed" : "Auto"; }
	const char* VisibilityText(Visibility visibility) { return visibility == Visibility::Listed ? "Listed" : (visibility == Visibility::Unlisted ? "Unlisted" : "LAN"); }

}

const std::string SettingsMan::c_ClassName = "SettingsMan";

void SettingsMan::Clear() {
	m_SettingsPath = System::GetUserdataDirectory() + "Settings.ini";
	m_SettingsNeedOverwrite = false;

	m_FlashOnBrainDamage = true;
	m_BlipOnRevealUnseen = false;
	m_UnheldItemsHUDDisplayRange = 25 * c_PPM;
	m_AlwaysDisplayUnheldItemsInStrategicMode = true;
	m_SubPieMenuHoverOpenDelay = 1000;
	m_EndlessMetaGameMode = false;
	m_EnableCrabBombs = false;
	m_CrabBombThreshold = 42;
	m_ShowEnemyHUD = true;
	m_EnableSmartBuyMenuNavigation = true;
	m_AutomaticGoldDeposit = true;
	m_BrainlessHumansSpectate = true;

	m_NetworkServerAddress = "127.0.0.1:8000";
	m_PlayerNetworkName = "Dummy";
	m_NATServiceAddress = "127.0.0.1:61111";
	m_NATServerName = "DefaultServerName";
	m_NATServerPassword = "DefaultServerPassword";
	m_UseExperimentalMultiplayerSpeedBoosts = true;

	m_AllowSavingToBase = false;
	m_ShowForeignItems = true;
	m_ShowMetaScenes = false;

	m_DisableLuaJIT = false;
	m_EnableLuaDebugging = false;
	m_RecommendedMOIDCount = 512;
	m_SceneBackgroundAutoScaleMode = 1;
	m_DisableFactionBuyMenuThemes = false;
	m_DisableFactionBuyMenuThemeCursors = false;
	m_PathFinderGridNodeSize = SCENEGRIDSIZE;
	m_AIUpdateInterval = 2;
	m_NetworkInputDelayFrames = 0;
	SetAutosaveSeconds(0);
	m_SessionDirectoryUrl.clear();
	m_SessionDirectoryInstallKey.clear();
	m_SessionDirectoryCertSha256.clear();
	m_NetworkPortMapEnable = false;
	m_NetworkPortMapEnableOverride = -1;
	m_NetworkIceEnable = false;
	m_NetworkStunServers.clear();
	m_NetworkTurnServers.clear();
	m_NetworkTurnUser.clear();
	m_NetworkTurnPass.clear();
	m_LocalPrediction = true;
	m_LocalPredictionMaxTicks = 20;
	m_NetworkDisplayName = "Player";
	m_NetworkDiagnosticsDirectory.clear();
	m_NetworkMatchStatusMode = NetworkMatchStatusMode::Auto;
	m_NetworkChatDefaultScope = NetworkChatDefaultScope::All;
	m_NetworkChatTextSize = NetworkChatTextSize::Small;
	m_NetworkHostDelayPolicy = NetworkHostDelayPolicy::Auto;
	m_NetworkHostVisibility = NetworkHostVisibility::LAN;
	m_NetworkToastsEnabled = m_NetworkChatVisible = m_NetworkChatNotify = m_NetworkAutoReconnect = m_NetworkOfferStoredRejoin = m_NetworkRecordReplays = m_NetworkHostAutoRepair = true;
	m_NetworkChatSound = false;
	m_NetworkHostIdleWaitMinutes = 10;
	m_NumberOfLuaStatesOverride = -1;
	m_ForceImmediatePathingRequestCompletion = false;

	m_SkipIntro = false;
	m_ShowToolTips = true;
	m_DisableLoadingScreenProgressReport = true;
	m_LoadingScreenProgressReportPrecision = 100;
	m_MenuTransitionDurationMultiplier = 1.0F;

	m_DrawAtomGroupVisualizations = false;
	m_DrawHandAndFootGroupVisualizations = false;
	m_DrawLimbPathVisualizations = false;
	m_PrintDebugInfo = false;
	m_MeasureModuleLoadTime = false;

	m_DisabledMods.clear();
	m_EnabledGlobalScripts.clear();
}

int SettingsMan::Initialize() {
	if (const char* settingsTempPath = std::getenv("CCCP_SETTINGSPATH")) {
		m_SettingsPath = std::string(settingsTempPath);
	}

	Reader settingsReader(m_SettingsPath, false, nullptr, true, true);

	if (!settingsReader.ReaderOK()) {
		Writer settingsWriter(m_SettingsPath);
		RTEAssert(settingsWriter.WriterOK(), "After failing to open the " + m_SettingsPath + ", could not then even create a new one to save settings to!\nAre you trying to run the game from a read-only disk?\nYou need to install the game to a writable area before running it!");

		// Settings file doesn't need to be populated with anything right now besides this manager's ClassName for serialization. It will be overwritten with the full list of settings with default values from all the managers before modules start loading.
		settingsWriter.ObjectStart(GetClassName());
		settingsWriter.EndWrite();

		m_SettingsNeedOverwrite = true;

		Reader newSettingsReader(m_SettingsPath, false, nullptr, false, true);
		return Serializable::Create(newSettingsReader);
	}

	int failureCode = Serializable::Create(settingsReader);

	if (GetAnyExperimentalSettingsEnabled()) {
		// Show a message box to annoy people as much as possible while they're using experimental settings, so they can't leave it on accidentally
		RTEError::ShowMessageBox("Experimental settings are enabled!\nThis may break mods, crash the game, corrupt saves or worse.\nUse at your own risk.");
	}

	return failureCode;
}

void SettingsMan::SetAutosaveSeconds(uint32_t seconds) {
	m_AutosaveSeconds = seconds;
	NetMatchService::SetAutosaveSecondsSetting(seconds);
}

void SettingsMan::GenerateSessionDirectoryInstallKey() {
	// A rate-limit identity, not a sim draw: non-sim entropy so lockstep runs stay byte-identical.
	static const char hex[] = "0123456789abcdef";
	std::random_device device;
	m_SessionDirectoryInstallKey.resize(32);
	for (char& ch : m_SessionDirectoryInstallKey) {
		ch = hex[device() & 0x0F];
	}
}

const std::string& SettingsMan::GetOrCreateSessionDirectoryInstallKey() {
	if (m_SessionDirectoryInstallKey.empty()) {
		GenerateSessionDirectoryInstallKey();
		UpdateSettingsFile();
	}
	return m_SessionDirectoryInstallKey;
}

void SettingsMan::UpdateSettingsFile() const {
	Writer settingsWriter(m_SettingsPath);
	g_SettingsMan.Save(settingsWriter);
}

std::string SettingsMan::GetEnabledGlobalScriptsCSV() const {
	// Sorted so every machine renders the same Settings map as the same string.
	const std::map<std::string, bool> sortedGlobalScripts(m_EnabledGlobalScripts.begin(), m_EnabledGlobalScripts.end());
	std::string csv;
	for (const auto& [scriptName, enabled]: sortedGlobalScripts) {
		if (enabled) {
			csv += (csv.empty() ? "" : ",") + scriptName;
		}
	}
	return csv;
}

int SettingsMan::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("PaletteFile", { reader >> g_FrameMan.m_PaletteFile; });
	MatchProperty("ResolutionX", { reader >> g_WindowMan.m_ResX; });
	MatchProperty("ResolutionY", { reader >> g_WindowMan.m_ResY; });
	MatchProperty("ResolutionMultiplier", { reader >> g_WindowMan.m_ResMultiplier; });
	MatchProperty("EnableVSync", { reader >> g_WindowMan.m_EnableVSync; });
	MatchProperty("Fullscreen", { reader >> g_WindowMan.m_Fullscreen; });
	MatchProperty("UseMultiDisplays", { reader >> g_WindowMan.m_UseMultiDisplays; });
	MatchProperty("TwoPlayerSplitscreenVertSplit", { reader >> g_FrameMan.m_TwoPlayerVSplit; });
	MatchProperty("MasterVolume", { g_AudioMan.SetMasterVolume(std::stof(reader.ReadPropValue()) / 100.0F); });
	MatchProperty("MuteMaster", { reader >> g_AudioMan.m_MuteMaster; });
	MatchProperty("MusicVolume", { g_AudioMan.SetMusicVolume(std::stof(reader.ReadPropValue()) / 100.0F); });
	MatchProperty("MuteMusic", { reader >> g_AudioMan.m_MuteMusic; });
	MatchProperty("SoundVolume", { g_AudioMan.SetSoundsVolume(std::stof(reader.ReadPropValue()) / 100.0F); });
	MatchProperty("MuteSounds", { reader >> g_AudioMan.m_MuteSounds; });
	MatchProperty("MuteAudioOnFocusLoss", { reader >> g_AudioMan.m_MuteAudioOnFocusLoss; });
	MatchProperty("SoundPanningEffectStrength", {
		reader >> g_AudioMan.m_SoundPanningEffectStrength;

		//////////////////////////////////////////////////
		// TODO These need to be removed when our soundscape is sorted out. They're only here temporarily to allow for easier tweaking by pawnis.
	});
	MatchProperty("ListenerZOffset", { reader >> g_AudioMan.m_ListenerZOffset; });
	MatchProperty("MinimumDistanceForPanning", {
		reader >> g_AudioMan.m_MinimumDistanceForPanning;
		//////////////////////////////////////////////////
	});
	MatchProperty("ShowForeignItems", { reader >> m_ShowForeignItems; });
	MatchProperty("FlashOnBrainDamage", { reader >> m_FlashOnBrainDamage; });
	MatchProperty("BlipOnRevealUnseen", { reader >> m_BlipOnRevealUnseen; });
	MatchProperty("MaxUnheldItems", { reader >> g_MovableMan.m_MaxDroppedItems; });
	MatchProperty("UnheldItemsHUDDisplayRange", { SetUnheldItemsHUDDisplayRange(std::stof(reader.ReadPropValue())); });
	MatchProperty("AlwaysDisplayUnheldItemsInStrategicMode", { reader >> m_AlwaysDisplayUnheldItemsInStrategicMode; });
	MatchProperty("SubPieMenuHoverOpenDelay", { reader >> m_SubPieMenuHoverOpenDelay; });
	MatchProperty("EndlessMode", { reader >> m_EndlessMetaGameMode; });
	MatchProperty("EnableCrabBombs", { reader >> m_EnableCrabBombs; });
	MatchProperty("CrabBombThreshold", { reader >> m_CrabBombThreshold; });
	MatchProperty("ShowEnemyHUD", { reader >> m_ShowEnemyHUD; });
	MatchProperty("SmartBuyMenuNavigation", { reader >> m_EnableSmartBuyMenuNavigation; });
	MatchProperty("ScrapCompactingHeight", { reader >> g_SceneMan.m_ScrapCompactingHeight; });
	MatchProperty("AutomaticGoldDeposit", { reader >> m_AutomaticGoldDeposit; });
	MatchProperty("BrainlessHumansSpectate", { reader >> m_BrainlessHumansSpectate; });
	MatchProperty("ScreenShakeStrength", { reader >> g_CameraMan.m_ScreenShakeStrength; });
	MatchProperty("ScreenShakeDecay", { reader >> g_CameraMan.m_ScreenShakeDecay; });
	MatchProperty("MaxScreenShakeTime", { reader >> g_CameraMan.m_MaxScreenShakeTime; });
	MatchProperty("DefaultShakePerUnitOfGibEnergy", { reader >> g_CameraMan.m_DefaultShakePerUnitOfGibEnergy; });
	MatchProperty("DefaultShakePerUnitOfRecoilEnergy", { reader >> g_CameraMan.m_DefaultShakePerUnitOfRecoilEnergy; });
	MatchProperty("DefaultShakeFromRecoilMaximum", { reader >> g_CameraMan.m_DefaultShakeFromRecoilMaximum; });
	MatchProperty("LaunchIntoActivity", { reader >> g_ActivityMan.m_LaunchIntoActivity; });
	MatchProperty("DefaultActivityType", { reader >> g_ActivityMan.m_DefaultActivityType; });
	MatchProperty("DefaultActivityName", { reader >> g_ActivityMan.m_DefaultActivityName; });
	MatchProperty("DefaultSceneName", { reader >> g_SceneMan.m_DefaultSceneName; });
	MatchProperty("DisableLuaJIT", { reader >> m_DisableLuaJIT; });
	MatchProperty("EnableLuaDebugging", { reader >> m_EnableLuaDebugging; });
	MatchProperty("RecommendedMOIDCount", { reader >> m_RecommendedMOIDCount; });
	MatchProperty("SceneBackgroundAutoScaleMode", { SetSceneBackgroundAutoScaleMode(std::stoi(reader.ReadPropValue())); });
	MatchProperty("DisableFactionBuyMenuThemes", { reader >> m_DisableFactionBuyMenuThemes; });
	MatchProperty("DisableFactionBuyMenuThemeCursors", { reader >> m_DisableFactionBuyMenuThemeCursors; });
	MatchProperty("PathFinderGridNodeSize", { reader >> m_PathFinderGridNodeSize; });
	MatchProperty("AIUpdateInterval", { reader >> m_AIUpdateInterval; });
	MatchProperty("NetworkInputDelayFrames", { reader >> m_NetworkInputDelayFrames; });
	MatchProperty("AutosaveSeconds", {
		const std::string value = reader.ReadPropValue();
		uint32_t seconds = 0;
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
		if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
			reader.ReportError("AutosaveSeconds requires a nonnegative 32-bit integer");
			return -1;
		}
		SetAutosaveSeconds(seconds);
	});
	MatchProperty("SessionDirectoryUrl", { reader >> m_SessionDirectoryUrl; });
	MatchProperty("SessionDirectoryInstallKey", { reader >> m_SessionDirectoryInstallKey; });
	MatchProperty("SessionDirectoryCertSha256", { reader >> m_SessionDirectoryCertSha256; });
	MatchProperty("NetworkPortMapEnable", { reader >> m_NetworkPortMapEnable; });
	MatchProperty("NetworkIceEnable", { reader >> m_NetworkIceEnable; });
	MatchProperty("NetworkStunServers", { reader >> m_NetworkStunServers; });
	MatchProperty("NetworkTurnServers", { reader >> m_NetworkTurnServers; });
	MatchProperty("NetworkTurnUser", { reader >> m_NetworkTurnUser; });
	MatchProperty("NetworkTurnPass", { reader >> m_NetworkTurnPass; });
	MatchProperty("LocalPrediction", { reader >> m_LocalPrediction; });
	MatchProperty("LocalPredictionMaxTicks", { reader >> m_LocalPredictionMaxTicks; });
	MatchProperty("NetworkDisplayName", { SetNetworkDisplayName(reader.ReadPropValue()); });
	MatchProperty("NetworkMatchStatusMode", { m_NetworkMatchStatusMode = ParseMatchStatusMode(reader.ReadPropValue()); });
	MatchProperty("NetworkToastsEnabled", { reader >> m_NetworkToastsEnabled; });
	MatchProperty("NetworkChatVisible", { reader >> m_NetworkChatVisible; });
	MatchProperty("NetworkChatDefaultScope", { m_NetworkChatDefaultScope = ParseChatDefaultScope(reader.ReadPropValue()); });
	MatchProperty("NetworkChatNotify", { reader >> m_NetworkChatNotify; });
	MatchProperty("NetworkChatSound", { reader >> m_NetworkChatSound; });
	MatchProperty("NetworkChatTextSize", { m_NetworkChatTextSize = ParseChatTextSize(reader.ReadPropValue()); });
	MatchProperty("NetworkAutoReconnect", { reader >> m_NetworkAutoReconnect; });
	MatchProperty("NetworkOfferStoredRejoin", { reader >> m_NetworkOfferStoredRejoin; });
	MatchProperty("NetworkDiagnosticsDirectory", { SetNetworkDiagnosticsDirectory(reader.ReadPropValue()); });
	MatchProperty("NetworkRecordReplays", { reader >> m_NetworkRecordReplays; });
	MatchProperty("NetworkHostDelayPolicy", { m_NetworkHostDelayPolicy = ParseHostDelayPolicy(reader.ReadPropValue()); });
	MatchProperty("NetworkHostAutoRepair", { reader >> m_NetworkHostAutoRepair; });
	MatchProperty("NetworkHostIdleWaitMinutes", { int minutes = m_NetworkHostIdleWaitMinutes; reader >> minutes; SetNetworkHostIdleWaitMinutes(minutes); });
	MatchProperty("NetworkHostVisibility", { m_NetworkHostVisibility = ParseHostVisibility(reader.ReadPropValue()); });
	MatchProperty("NumberOfLuaStatesOverride", { reader >> m_NumberOfLuaStatesOverride; });
	MatchProperty("ForceImmediatePathingRequestCompletion", { reader >> m_ForceImmediatePathingRequestCompletion; });
	MatchProperty("EnableParticleSettling", { reader >> g_MovableMan.m_SettlingEnabled; });
	MatchProperty("EnableMOSubtraction", { reader >> g_MovableMan.m_MOSubtractionEnabled; });
	MatchProperty("DeltaTime", { g_TimerMan.SetDeltaTimeSecs(std::stof(reader.ReadPropValue())); });
	MatchProperty("AllowSavingToBase", { reader >> m_AllowSavingToBase; });
	MatchProperty("ShowMetaScenes", { reader >> m_ShowMetaScenes; });
	MatchProperty("SkipIntro", { reader >> m_SkipIntro; });
	MatchProperty("ShowToolTips", { reader >> m_ShowToolTips; });
	MatchProperty("CaseSensitiveFilePaths", { System::EnableFilePathCaseSensitivity(std::stoi(reader.ReadPropValue())); });
	MatchProperty("DisableLoadingScreenProgressReport", { reader >> m_DisableLoadingScreenProgressReport; });
	MatchProperty("LoadingScreenProgressReportPrecision", { reader >> m_LoadingScreenProgressReportPrecision; });
	MatchProperty("ConsoleScreenRatio", { g_ConsoleMan.SetConsoleScreenSize(std::stof(reader.ReadPropValue())); });
	MatchProperty("ConsoleUseMonospaceFont", { reader >> g_ConsoleMan.m_ConsoleUseMonospaceFont; });
	MatchProperty("AdvancedPerformanceStats", { reader >> g_PerformanceMan.m_AdvancedPerfStats; });
	MatchProperty("MenuTransitionDurationMultiplier", { SetMenuTransitionDurationMultiplier(std::stof(reader.ReadPropValue())); });
	MatchProperty("DrawAtomGroupVisualizations", { reader >> m_DrawAtomGroupVisualizations; });
	MatchProperty("DrawHandAndFootGroupVisualizations", { reader >> m_DrawHandAndFootGroupVisualizations; });
	MatchProperty("DrawLimbPathVisualizations", { reader >> m_DrawLimbPathVisualizations; });
	MatchProperty("DrawRaycastVisualizations", { reader >> g_SceneMan.m_DrawRayCastVisualizations; });
	MatchProperty("DrawPixelCheckVisualizations", { reader >> g_SceneMan.m_DrawPixelCheckVisualizations; });
	MatchProperty("PrintDebugInfo", { reader >> m_PrintDebugInfo; });
	MatchProperty("MeasureModuleLoadTime", { reader >> m_MeasureModuleLoadTime; });
	MatchProperty("VisibleAssemblyGroup", { m_VisibleAssemblyGroupsList.push_back(reader.ReadPropValue()); });
	MatchProperty("DisableMod", { m_DisabledMods.try_emplace(reader.ReadPropValue(), true); });
	MatchProperty("EnableGlobalScript", { m_EnabledGlobalScripts.try_emplace(reader.ReadPropValue(), true); });
	MatchProperty("ForceDisableMultimouse", { reader >> g_UInputMan.m_ForceDisableMultiMouseKeyboard; });
	MatchProperty("MouseSensitivity", { reader >> g_UInputMan.m_MouseSensitivity; });
	MatchForwards("Player1Scheme") MatchForwards("Player2Scheme") MatchForwards("Player3Scheme") MatchProperty("Player4Scheme", {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
			std::string playerNum = std::to_string(player + 1);
			if (propName == "Player" + playerNum + "Scheme") {
				g_UInputMan.m_ControlScheme[player].Reset();
				reader >> g_UInputMan.m_ControlScheme[player];
				break;
			}
		}
	});

	EndPropertyList;
}

int SettingsMan::Save(Writer& writer) const {
	Serializable::Save(writer);

	writer.NewDivider(false);
	writer.NewLineString("// Display Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("PaletteFile", g_FrameMan.m_PaletteFile);
	writer.NewPropertyWithValue("ResolutionX", g_WindowMan.m_ResX);
	writer.NewPropertyWithValue("ResolutionY", g_WindowMan.m_ResY);
	writer.NewPropertyWithValue("ResolutionMultiplier", g_WindowMan.m_ResMultiplier);
	writer.NewPropertyWithValue("Fullscreen", g_WindowMan.m_Fullscreen);
	writer.NewPropertyWithValue("EnableVSync", g_WindowMan.m_EnableVSync);
	writer.NewPropertyWithValue("UseMultiDisplays", g_WindowMan.m_UseMultiDisplays);
	writer.NewPropertyWithValue("TwoPlayerSplitscreenVertSplit", g_FrameMan.m_TwoPlayerVSplit);

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Audio Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("MasterVolume", g_AudioMan.m_MasterVolume * 100);
	writer.NewPropertyWithValue("MuteMaster", g_AudioMan.m_MuteMaster);
	writer.NewPropertyWithValue("MusicVolume", g_AudioMan.m_MusicVolume * 100);
	writer.NewPropertyWithValue("MuteMusic", g_AudioMan.m_MuteMusic);
	writer.NewPropertyWithValue("SoundVolume", g_AudioMan.m_SoundsVolume * 100);
	writer.NewPropertyWithValue("MuteSounds", g_AudioMan.m_MuteSounds);
	writer.NewPropertyWithValue("MuteAudioOnFocusLoss", g_AudioMan.m_MuteAudioOnFocusLoss);
	writer.NewPropertyWithValue("SoundPanningEffectStrength", g_AudioMan.m_SoundPanningEffectStrength);

	//////////////////////////////////////////////////
	// TODO These need to be removed when our soundscape is sorted out. They're only here temporarily to allow for easier tweaking.
	writer.NewPropertyWithValue("ListenerZOffset", g_AudioMan.m_ListenerZOffset);
	writer.NewPropertyWithValue("MinimumDistanceForPanning", g_AudioMan.m_MinimumDistanceForPanning);
	//////////////////////////////////////////////////

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Gameplay Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("ShowForeignItems", m_ShowForeignItems);
	writer.NewPropertyWithValue("FlashOnBrainDamage", m_FlashOnBrainDamage);
	writer.NewPropertyWithValue("BlipOnRevealUnseen", m_BlipOnRevealUnseen);
	writer.NewPropertyWithValue("MaxUnheldItems", g_MovableMan.m_MaxDroppedItems);
	writer.NewPropertyWithValue("UnheldItemsHUDDisplayRange", m_UnheldItemsHUDDisplayRange);
	writer.NewPropertyWithValue("AlwaysDisplayUnheldItemsInStrategicMode", m_AlwaysDisplayUnheldItemsInStrategicMode);
	writer.NewPropertyWithValue("SubPieMenuHoverOpenDelay", m_SubPieMenuHoverOpenDelay);
	writer.NewPropertyWithValue("EndlessMetaGameMode", m_EndlessMetaGameMode);
	writer.NewPropertyWithValue("EnableCrabBombs", m_EnableCrabBombs);
	writer.NewPropertyWithValue("CrabBombThreshold", m_CrabBombThreshold);
	writer.NewPropertyWithValue("ShowEnemyHUD", m_ShowEnemyHUD);
	writer.NewPropertyWithValue("SmartBuyMenuNavigation", m_EnableSmartBuyMenuNavigation);
	writer.NewPropertyWithValue("ScrapCompactingHeight", g_SceneMan.m_ScrapCompactingHeight);
	writer.NewPropertyWithValue("AutomaticGoldDeposit", m_AutomaticGoldDeposit);
	writer.NewPropertyWithValue("BrainlessHumansSpectate", m_BrainlessHumansSpectate);

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Screen Shake Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("ScreenShakeStrength", g_CameraMan.m_ScreenShakeStrength);
	writer.NewPropertyWithValue("ScreenShakeDecay", g_CameraMan.m_ScreenShakeDecay);
	writer.NewPropertyWithValue("MaxScreenShakeTime", g_CameraMan.m_MaxScreenShakeTime);
	writer.NewPropertyWithValue("DefaultShakePerUnitOfGibEnergy", g_CameraMan.m_DefaultShakePerUnitOfGibEnergy);
	writer.NewPropertyWithValue("DefaultShakePerUnitOfRecoilEnergy", g_CameraMan.m_DefaultShakePerUnitOfRecoilEnergy);
	writer.NewPropertyWithValue("DefaultShakeFromRecoilMaximum", g_CameraMan.m_DefaultShakeFromRecoilMaximum);

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Default Activity Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("LaunchIntoActivity", g_ActivityMan.m_LaunchIntoActivity);
	writer.NewPropertyWithValue("DefaultActivityType", g_ActivityMan.m_DefaultActivityType);
	writer.NewPropertyWithValue("DefaultActivityName", g_ActivityMan.m_DefaultActivityName);
	writer.NewPropertyWithValue("DefaultSceneName", g_SceneMan.m_DefaultSceneName);

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Engine Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("DisableLuaJIT", m_DisableLuaJIT);
	writer.NewPropertyWithValue("EnableLuaDebugging", m_EnableLuaDebugging);
	writer.NewPropertyWithValue("RecommendedMOIDCount", m_RecommendedMOIDCount);
	writer.NewPropertyWithValue("SceneBackgroundAutoScaleMode", m_SceneBackgroundAutoScaleMode);
	writer.NewPropertyWithValue("DisableFactionBuyMenuThemes", m_DisableFactionBuyMenuThemes);
	writer.NewPropertyWithValue("DisableFactionBuyMenuThemeCursors", m_DisableFactionBuyMenuThemeCursors);
	writer.NewPropertyWithValue("PathFinderGridNodeSize", m_PathFinderGridNodeSize);
	writer.NewPropertyWithValue("AIUpdateInterval", m_AIUpdateInterval);
	writer.NewPropertyWithValue("NetworkInputDelayFrames", m_NetworkInputDelayFrames);
	writer.NewPropertyWithValue("AutosaveSeconds", m_AutosaveSeconds);
	writer.NewPropertyWithValue("SessionDirectoryUrl", m_SessionDirectoryUrl);
	writer.NewPropertyWithValue("SessionDirectoryInstallKey", m_SessionDirectoryInstallKey);
	writer.NewPropertyWithValue("SessionDirectoryCertSha256", m_SessionDirectoryCertSha256);
	writer.NewPropertyWithValue("NetworkPortMapEnable", m_NetworkPortMapEnable);
	writer.NewPropertyWithValue("NetworkIceEnable", m_NetworkIceEnable);
	writer.NewPropertyWithValue("NetworkStunServers", m_NetworkStunServers);
	writer.NewPropertyWithValue("NetworkTurnServers", m_NetworkTurnServers);
	writer.NewPropertyWithValue("NetworkTurnUser", m_NetworkTurnUser);
	writer.NewPropertyWithValue("NetworkTurnPass", m_NetworkTurnPass);
	writer.NewPropertyWithValue("LocalPrediction", m_LocalPrediction);
	writer.NewPropertyWithValue("LocalPredictionMaxTicks", m_LocalPredictionMaxTicks);
	WriteNetworkPreferences(writer);
	writer.NewPropertyWithValue("NumberOfLuaStatesOverride", m_NumberOfLuaStatesOverride);
	writer.NewPropertyWithValue("ForceImmediatePathingRequestCompletion", m_ForceImmediatePathingRequestCompletion);
	writer.NewPropertyWithValue("EnableParticleSettling", g_MovableMan.m_SettlingEnabled);
	writer.NewPropertyWithValue("EnableMOSubtraction", g_MovableMan.m_MOSubtractionEnabled);
	writer.NewPropertyWithValue("DeltaTime", g_TimerMan.GetDeltaTimeSecs());

	// No experimental settings right now :)
	// writer.NewLine(false, 2);
	// writer.NewDivider(false);
	// writer.NewLineString("// Engine Settings - EXPERIMENTAL", false);
	// writer.NewLineString("// These settings are experimental! They may break mods, crash the game, corrupt saves or worse. Use at your own risk.", false);
	// writer.NewLine(false);

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Editor Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("AllowSavingToBase", m_AllowSavingToBase);
	writer.NewPropertyWithValue("ShowMetaScenes", m_ShowMetaScenes);

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Misc Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("SkipIntro", m_SkipIntro);
	writer.NewPropertyWithValue("ShowToolTips", m_ShowToolTips);
	writer.NewPropertyWithValue("CaseSensitiveFilePaths", System::FilePathsCaseSensitive());
	writer.NewPropertyWithValue("DisableLoadingScreenProgressReport", m_DisableLoadingScreenProgressReport);
	writer.NewPropertyWithValue("LoadingScreenProgressReportPrecision", m_LoadingScreenProgressReportPrecision);
	writer.NewPropertyWithValue("ConsoleScreenRatio", g_ConsoleMan.m_ConsoleScreenRatio);
	writer.NewPropertyWithValue("ConsoleUseMonospaceFont", g_ConsoleMan.m_ConsoleUseMonospaceFont);
	writer.NewPropertyWithValue("AdvancedPerformanceStats", g_PerformanceMan.m_AdvancedPerfStats);
	writer.NewPropertyWithValue("MenuTransitionDurationMultiplier", m_MenuTransitionDurationMultiplier);

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Modder Debug Settings", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("DrawAtomGroupVisualizations", m_DrawAtomGroupVisualizations);
	writer.NewPropertyWithValue("DrawHandAndFootGroupVisualizations", m_DrawHandAndFootGroupVisualizations);
	writer.NewPropertyWithValue("DrawLimbPathVisualizations", m_DrawLimbPathVisualizations);
	writer.NewPropertyWithValue("DrawRaycastVisualizations", g_SceneMan.m_DrawRayCastVisualizations);
	writer.NewPropertyWithValue("DrawPixelCheckVisualizations", g_SceneMan.m_DrawPixelCheckVisualizations);
	writer.NewPropertyWithValue("PrintDebugInfo", m_PrintDebugInfo);
	writer.NewPropertyWithValue("MeasureModuleLoadTime", m_MeasureModuleLoadTime);

	if (!m_VisibleAssemblyGroupsList.empty()) {
		writer.NewLine(false, 2);
		writer.NewDivider(false);
		writer.NewLineString("// Enabled Bunker Assembly Groups", false);
		writer.NewLine(false);
		for (const std::string& visibleAssembly: m_VisibleAssemblyGroupsList) {
			writer.NewPropertyWithValue("VisibleAssemblyGroup", visibleAssembly);
		}
	}

	if (!m_DisabledMods.empty()) {
		writer.NewLine(false, 2);
		writer.NewDivider(false);
		writer.NewLineString("// Disabled Mods", false);
		writer.NewLine(false);
		for (const auto& [modPath, modDisabled]: m_DisabledMods) {
			if (modDisabled) {
				writer.NewPropertyWithValue("DisableMod", modPath);
			}
		}
	}

	if (!m_EnabledGlobalScripts.empty()) {
		writer.NewLine(false, 2);
		writer.NewDivider(false);
		writer.NewLineString("// Enabled Global Scripts", false);
		writer.NewLine(false);
		for (const auto& [scriptPresetName, scriptEnabled]: m_EnabledGlobalScripts) {
			if (scriptEnabled) {
				writer.NewPropertyWithValue("EnableGlobalScript", scriptPresetName);
			}
		}
	}

	writer.NewLine(false, 2);
	writer.NewDivider(false);
	writer.NewLineString("// Input Mapping", false);
	writer.NewLine(false);
	writer.NewPropertyWithValue("ForceDisableMultimouse", g_UInputMan.m_ForceDisableMultiMouseKeyboard);
	writer.NewPropertyWithValue("MouseSensitivity", g_UInputMan.m_MouseSensitivity);

	writer.NewLine(false);
	writer.NewLineString("// Input Devices:  0 = Keyboard Only, 1 = Mouse + Keyboard, 2 = Gamepad One, 3 = Gamepad Two, , 4 = Gamepad Three, 5 = Gamepad Four");
	writer.NewLineString("// Scheme Presets: 0 = No Preset, 1 = Arrow Keys, 2 = WASD Keys, 3 = Mouse + WASD Keys, 4 = Generic DPad, 5 = Generic Dual Analog, 6 = SNES, 7 = DualShock 4, 8 = XBox 360");

	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
		std::string playerNum = std::to_string(player + 1);
		writer.NewLine(false, 2);
		writer.NewDivider(false);
		writer.NewLineString("// Player " + playerNum, false);
		writer.NewLine(false);
		writer.NewPropertyWithValue("Player" + playerNum + "Scheme", g_UInputMan.m_ControlScheme[player]);
	}

	writer.ObjectEnd();

	return 0;
}

void SettingsMan::SetNetworkDisplayName(const std::string& newName) {
	const std::string trimmed = TrimCopy(newName);
	if (ValidUtf8DisplayName(trimmed)) m_NetworkDisplayName = trimmed;
}

void SettingsMan::SetNetworkDiagnosticsDirectory(const std::string& directory) {
	if (ValidDiagnosticsDirectory(directory)) m_NetworkDiagnosticsDirectory = directory;
}

void SettingsMan::SetNetworkHostIdleWaitMinutes(int minutes) {
	if (minutes >= 0 && minutes <= 60) m_NetworkHostIdleWaitMinutes = minutes;
}

void SettingsMan::WriteNetworkPreferences(Writer& writer) const {
	writer.NewPropertyWithValue("NetworkDisplayName", m_NetworkDisplayName);
	writer.NewPropertyWithValue("NetworkMatchStatusMode", MatchStatusText(m_NetworkMatchStatusMode));
	writer.NewPropertyWithValue("NetworkToastsEnabled", m_NetworkToastsEnabled);
	writer.NewPropertyWithValue("NetworkChatVisible", m_NetworkChatVisible);
	writer.NewPropertyWithValue("NetworkChatDefaultScope", ChatScopeText(m_NetworkChatDefaultScope));
	writer.NewPropertyWithValue("NetworkChatNotify", m_NetworkChatNotify);
	writer.NewPropertyWithValue("NetworkChatSound", m_NetworkChatSound);
	writer.NewPropertyWithValue("NetworkChatTextSize", ChatSizeText(m_NetworkChatTextSize));
	writer.NewPropertyWithValue("NetworkAutoReconnect", m_NetworkAutoReconnect);
	writer.NewPropertyWithValue("NetworkOfferStoredRejoin", m_NetworkOfferStoredRejoin);
	writer.NewPropertyWithValue("NetworkDiagnosticsDirectory", m_NetworkDiagnosticsDirectory);
	writer.NewPropertyWithValue("NetworkRecordReplays", m_NetworkRecordReplays);
	writer.NewPropertyWithValue("NetworkHostDelayPolicy", DelayPolicyText(m_NetworkHostDelayPolicy));
	writer.NewPropertyWithValue("NetworkHostAutoRepair", m_NetworkHostAutoRepair);
	writer.NewPropertyWithValue("NetworkHostIdleWaitMinutes", m_NetworkHostIdleWaitMinutes);
	writer.NewPropertyWithValue("NetworkHostVisibility", VisibilityText(m_NetworkHostVisibility));
}

int SettingsMan::RunNetworkPreferencesSelfTest() {
	constexpr const char* Tag = "[settings-preferences-selftest]";
	if (!IsConstructed()) {
		Construct();
	}
	SettingsMan& settings = Instance();
	settings.Clear();
	settings.SetNetworkDisplayName("AlphaPilot");
	settings.SetNetworkMatchStatusMode(NetworkMatchStatusMode::Always);
	settings.SetNetworkChatDefaultScope(NetworkChatDefaultScope::Team);
	settings.SetNetworkChatTextSize(NetworkChatTextSize::Large);
	settings.SetNetworkDiagnosticsDirectory("D:/tmp/telemetry-alt");
	settings.SetNetworkHostDelayPolicy(NetworkHostDelayPolicy::Fixed);
	settings.SetNetworkHostVisibility(NetworkHostVisibility::Unlisted);
	settings.SetNetworkToastsEnabled(false);
	settings.SetNetworkChatVisible(false);
	settings.SetNetworkChatNotify(false);
	settings.SetNetworkChatSound(true);
	settings.SetNetworkAutoReconnect(false);
	settings.SetNetworkOfferStoredRejoin(false);
	settings.SetNetworkRecordReplays(false);
	settings.SetNetworkHostAutoRepair(false);
	settings.SetNetworkHostIdleWaitMinutes(0);

	const std::string path = (std::filesystem::temp_directory_path() / "cccp-settings-preferences-selftest.ini").string();
	const auto writeRead = [&](auto&& fill) {
		Writer writer(path);
		writer.ObjectStart(settings.GetClassName());
		fill(writer);
		writer.ObjectEnd();
		writer.EndWrite();
		Reader reader(path, false, nullptr, true, true);
		settings.Create(reader);
	};
	int failures = 0;
	const auto check = [&](const char* label, bool ok) {
		if (!ok) {
			std::cout << Tag << " FAIL " << label << std::endl;
			++failures;
		}
	};
	{
		Writer writer(path);
		writer.ObjectStart(settings.GetClassName());
		settings.WriteNetworkPreferences(writer);
		writer.ObjectEnd();
		writer.EndWrite();
		settings.Clear();
		if (settings.GetNetworkDisplayName() != "Player") {
			std::cout << Tag << " FAIL roundtrip-cleared" << std::endl;
			return 1;
		}
		const char* broken = std::getenv("CCCP_SETTINGS_PREFERENCES_SELFTEST_BROKEN");
		Reader reader(broken ? broken : path, false, nullptr, true, true);
		if (!reader.ReaderOK()) {
			std::cout << Tag << " FAIL reader" << std::endl;
			return 1;
		}
		settings.Create(reader);
	}
	check("roundtrip", settings.GetNetworkDisplayName() == "AlphaPilot" && settings.GetNetworkMatchStatusMode() == NetworkMatchStatusMode::Always && !settings.GetNetworkToastsEnabled() && !settings.GetNetworkChatVisible() && settings.GetNetworkChatDefaultScope() == NetworkChatDefaultScope::Team && !settings.GetNetworkChatNotify() && settings.GetNetworkChatSound() && settings.GetNetworkChatTextSize() == NetworkChatTextSize::Large && !settings.GetNetworkAutoReconnect() && !settings.GetNetworkOfferStoredRejoin() && settings.GetNetworkDiagnosticsDirectory() == "D:/tmp/telemetry-alt" && !settings.GetNetworkRecordReplays() && settings.GetNetworkHostDelayPolicy() == NetworkHostDelayPolicy::Fixed && !settings.GetNetworkHostAutoRepair() && settings.GetNetworkHostIdleWaitMinutes() == 0 && settings.GetNetworkHostVisibility() == NetworkHostVisibility::Unlisted);
	if (failures != 0) {
		return 1;
	}

	const std::string kept = settings.GetNetworkDisplayName();
	settings.SetNetworkDisplayName("");
	settings.SetNetworkDisplayName("   ");
	settings.SetNetworkDisplayName(std::string(25, 'A'));
	settings.SetNetworkDisplayName(std::string("Bad\nName"));
	settings.SetNetworkDisplayName("\xFF\xFE");
	settings.SetNetworkDisplayName("\x80" "abc");
	settings.SetNetworkDisplayName("caf\xC3");
	settings.SetNetworkDisplayName("\xC0\xAF");
	settings.SetNetworkDisplayName("\xED\xA0\x80");
	check("invalid-name rejection", settings.GetNetworkDisplayName() == kept);
	settings.SetNetworkDisplayName("caf\xC3\xA9");
	check("invalid-name rejection", settings.GetNetworkDisplayName() == "caf\xC3\xA9");
	std::string twentyFour;
	twentyFour.reserve(48);
	for (int i = 0; i < 24; ++i) {
		twentyFour += "\xC3\xA9";
	}
	settings.SetNetworkDisplayName(twentyFour);
	check("invalid-name rejection", settings.GetNetworkDisplayName() == twentyFour);
	settings.SetNetworkDisplayName(twentyFour + "\xC3\xA9");
	check("invalid-name rejection", settings.GetNetworkDisplayName() == twentyFour);
	std::string overBytes;
	overBytes.reserve(66);
	for (int i = 0; i < 33; ++i) {
		overBytes += "\xC3\xA9";
	}
	settings.SetNetworkDisplayName(overBytes);
	check("invalid-name rejection", settings.GetNetworkDisplayName() == twentyFour);
	settings.SetNetworkHostIdleWaitMinutes(61);
	settings.SetNetworkHostIdleWaitMinutes(-1);
	check("idle-wait range", settings.GetNetworkHostIdleWaitMinutes() == 0);

	const int warningsBefore = g_UnknownEnumWarnings;
	// Unknown-enum and case-insensitive arms leave a non-default set so the read is what changes it.
	settings.SetNetworkMatchStatusMode(NetworkMatchStatusMode::Always);
	writeRead([&](Writer& writer) { writer.NewPropertyWithValue("NetworkMatchStatusMode", "Banana"); });
	check("unknown-enum fallback", settings.GetNetworkMatchStatusMode() == NetworkMatchStatusMode::Auto);
	writeRead([&](Writer& writer) { writer.NewPropertyWithValue("NetworkMatchStatusMode", "Banana"); });
	check("unknown-enum once", g_UnknownEnumWarnings == warningsBefore + 1);

	settings.Clear();
	writeRead([&](Writer& writer) {
		writer.NewPropertyWithValue("NetworkMatchStatusMode", "always");
		writer.NewPropertyWithValue("NetworkChatDefaultScope", "TEAM");
		writer.NewPropertyWithValue("NetworkChatTextSize", "large");
		writer.NewPropertyWithValue("NetworkHostDelayPolicy", "FIXED");
		writer.NewPropertyWithValue("NetworkHostVisibility", "listed");
	});
	check("case-insensitive", settings.GetNetworkMatchStatusMode() == NetworkMatchStatusMode::Always && settings.GetNetworkChatDefaultScope() == NetworkChatDefaultScope::Team && settings.GetNetworkChatTextSize() == NetworkChatTextSize::Large && settings.GetNetworkHostDelayPolicy() == NetworkHostDelayPolicy::Fixed && settings.GetNetworkHostVisibility() == NetworkHostVisibility::Listed);

	// The saved host options only reach a match through the mapping, so the mapping is checked here.
	settings.Clear();
	settings.SetNetworkHostDelayPolicy(NetworkHostDelayPolicy::Fixed);
	settings.SetNetworkHostIdleWaitMinutes(25);
	settings.SetNetworkHostAutoRepair(false);
	NetMatchConfig hosted = NetMatchConfigUtil::MakeDefault(1);
	NetMatchConfigUtil::ApplySavedHostOptions(hosted);
	check("host options mapped", hosted.delayPolicy == NetMatchDelayPolicy::Fixed && hosted.idleWaitMinutes == 25 && !hosted.automaticRepair);
	settings.SetNetworkHostDelayPolicy(NetworkHostDelayPolicy::Auto);
	settings.SetNetworkHostAutoRepair(true);
	NetMatchConfigUtil::ApplySavedHostOptions(hosted);
	check("host options mapped back", hosted.delayPolicy == NetMatchDelayPolicy::Auto && hosted.automaticRepair);

	if (failures != 0) {
		return 1;
	}
	std::cout << Tag << " PASS" << std::endl;
	return 0;
}
