#pragma once

#include "NetReconnectUx.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RTE {
	class AllegroScreen;
	class GUIInputWrapper;
	class GUIControlManager;
	class GUICollectionBox;
	class GUIControl;
	class GUIButton;
	class GUILabel;
	class GUIFont;
	class GUITextBox;
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
		const OverlayRect& GetChatRect() const { return m_ChatRect; }
		const OverlayRect& GetRosterRect() const { return m_RosterRect; }
		const OverlayRect& GetSeatsPanelRect() const { return m_SeatsPanelRect; }
		bool IsChatEntryOpen() const { return m_ChatEntryOpen; }
		int RefreshCount() const { return m_RefreshCount; }
		int RefreshChangeCount() const { return m_RefreshChangeCount; }

		/// What the chat band laid out on the last frame, so a check can hold the rows it drew against the heights it used.
		struct ChatBand {
			int rowHeight = 0;
			int entryHeight = 0;
			int rows = 0;
			bool historyVisible = false;
			/// Set when the run left to the band could not hold the chosen size's history row and it drew the smaller one.
			bool reducedTextSize = false;
		};
		const ChatBand& GetChatBand() const { return m_ChatBand; }
		/// The SDL scancode the entry opens on, as the settings key name resolves it.
		static int ChatKeyScancode();

		/// A run of window rows the seats panel must not cross, as [top, bottom).
		struct PanelBand {
			int top = 0;
			int bottom = 0;
		};
		/// Where the seats panel sits.
		struct PanelPlacement {
			int top = 0;
			int height = 0;
		};
		/// Picks the highest run of rows no band holds: the whole panel when one run fits it, otherwise the
		/// longest run, which leaves the roster scrolling for the rows the panel gave up.
		/// @param highestTop The first row the panel may take.
		/// @param bottomLimit One past the last row the panel may take.
		/// @param wantedHeight The height the panel would have with no band in the way.
		/// @param minHeight The height below which the panel loses its close row.
		/// @param bands Every band, in any order; they may overlap.
		static PanelPlacement PlaceSeatsPanel(int highestTop, int bottomLimit, int wantedHeight, int minHeight, const std::vector<PanelBand>& bands);
		/// The placement LayoutPanel applies: a compact screen grows every seat message band by the toast
		/// row under it and solves around them, a tall one keeps the plain centred panel.
		/// @param screenHeight The window's height in rows.
		/// @param rowHeight One text row plus its padding, as the panel's font measures it.
		/// @param textBands Each seat message band as the editor reports it, ungrown, in any order.
		/// @param reservedTop The highest top an open chat entry's run leaves the panel; 0 when no entry is open.
		static PanelPlacement PlaceSeatsPanelOnScreen(int screenHeight, int rowHeight, const std::vector<PanelBand>& textBands, int reservedTop = 0);

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
		void RecordStatusObservation(const NetLobbySnapshot& snapshot, bool hostLost, long long currentWaitMs);
		void DrawMatchChat(const NetLobbySnapshot& snapshot);
		void UpdateMatchChat(const NetLobbySnapshot& snapshot);
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
		OverlayRect m_ChatRect;
		OverlayRect m_RosterRect;
		OverlayRect m_SeatsPanelRect; //!< Where the open seats panel sat when the toast band was laid out.
		long long m_StatusWaitStartedUs = 0;
		long long m_LastStatusObservationMs = 0;
		bool m_LastSlowNotice = false;
		bool m_LastHostLost = false;
		std::string m_LastHandoverToast;
		uint64_t m_LastHandoverRound = 0;
		ChatBand m_ChatBand;
		std::array<GUILabel*, 8> m_MatchChat{};
		GUITextBox* m_MatchChatInput = nullptr;
		struct MatchChatLine {
			uint64_t receivedTick = 0;
			uint8_t senderPeerId = 0;
			uint8_t team = 0;
			uint8_t scope = 0;
			std::string name;
			std::string text;
			long long seenUs = 0;
		};
		std::deque<MatchChatLine> m_MatchChatLines;
		bool m_ChatEntryOpen = false;
		bool m_ChatKeysHeld = false;
		bool m_ChatDisabledKeys = false;
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
		std::vector<uint8_t> m_AnnouncedAISeats;
		uint64_t m_DepartureFrame = 0;
		bool m_Open = false;
		int m_RefreshCount = 0;
		int m_RefreshChangeCount = 0;
		uint64_t m_LastRefreshHash = 0;
		long long m_LastRefreshMs = 0;
	};
}
