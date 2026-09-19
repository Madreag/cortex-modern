#pragma once

#include "NetReconnectUx.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace RTE {
	class AllegroScreen;
	class GUIInputWrapper;
	class GUIControlManager;
	class GUICollectionBox;
	class GUIControl;
	class GUIButton;
	class GUILabel;
	class GUIFont;
	struct NetLobbySnapshot;

	/// A live-match roster and host panel; opening it leaves the simulation running.
	class NetModerationGUI {
	public:
		explicit NetModerationGUI(AllegroScreen* screen);
		~NetModerationGUI();
		void Update();
		void Draw();
		/// Draws the bounded toast rows after the rest of the network UI.
		void DrawMatchToasts();
		bool SetOpen(bool open);
		bool IsOpen() const { return m_Open; }
		bool AutomationModerate(const std::string& action, int stableSeat);
		GUIControl* GetControl(const std::string& name) const;
		/// The panel's own control manager, for the script commands that read its rows.
		GUIControlManager* AutomationManager() const { return m_Controls.get(); }
		/// Clicks a named panel control the way its own manager's mouse would.
		bool AutomationPostCommand(const std::string& name);
		/// Reads a named label's text for a script assert.
		bool AutomationLabelText(const std::string& name, std::string& text) const;

		/// The area an overlay element drew into on the last frame, in screen pixels.
		struct OverlayRect {
			int x = 0, y = 0, width = 0, height = 0;
			bool visible = false;
		};
		const OverlayRect& GetStatusRect() const { return m_StatusRect; }
		const OverlayRect& GetToastRect() const { return m_ToastRect; }

		struct Controls {
			GUILabel* name = nullptr;
			GUILabel* detail = nullptr;
			GUIButton* applicant = nullptr;
			std::array<GUIButton*, 3> actions{};
		};
		struct Press {
			const GUIControl* button = nullptr;
			NetModerationUx::Row row;
		};
		void Refresh();
		void HandleEvents();
		void DrawRoster(const NetLobbySnapshot& snapshot);
		/// Creates presentation controls only when an online match draws them.
		void CreateOverlay();
		/// Whether the status widget should be up, per NetworkMatchStatusMode: Off never, Always always, Auto on events and three seconds past recovery.
		bool MatchStatusWanted() const;
		/// Draws the status widget: the box on tall screens, a single-line strip in the top HUD gap on short ones.
		void DrawMatchStatus(const NetLobbySnapshot& snapshot);
		/// Places the seats panel for the current screen height; a compact screen's top band keeps
		/// one toast row between the strip and the panel, which the roster's slack absorbs.
		void LayoutPanel();
		AllegroScreen* m_Screen = nullptr;
		std::unique_ptr<GUIInputWrapper> m_Input;
		std::unique_ptr<GUIControlManager> m_Controls;
		std::unique_ptr<GUIControlManager> m_OverlayControls;
		GUICollectionBox* m_NetStatusBox = nullptr;
		GUILabel* m_NetStatus = nullptr;
		std::array<GUILabel*, 3> m_Toasts{};
		OverlayRect m_StatusRect;
		OverlayRect m_ToastRect;
		std::string m_StripText;
		mutable long long m_AutoShowUntilUs = 0;
		uint16_t m_MatchDelayFrames = 0;
		uint16_t m_BaseDelayFrames = 0;
		GUICollectionBox* m_Panel = nullptr;
		GUIFont* m_LabelFont = nullptr;
		GUILabel* m_Title = nullptr;
		GUILabel* m_Summary = nullptr;
		GUILabel* m_Status = nullptr;
		GUILabel* m_Roster = nullptr;
		GUIButton* m_Close = nullptr;
		/// The F6 panel's second view: the match's adopted options, the same read-only panel the pause
		/// menu's Match Options and the lobby's Details show.
		GUIButton* m_OptionsToggle = nullptr;
		GUILabel* m_Options = nullptr;
		bool m_OptionsView = false;
		std::array<Controls, 3> m_Seats;
		std::optional<Press> m_Press;
		NetModerationUx m_Model;
		std::optional<NetH4ModerationResult> m_ActionResult;
		bool m_Open = false;
	};
}
