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
	class GUIListBox;
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
		/// @param screenLine The line the screen beneath already shows; the rows never repeat it.
		void DrawMatchToasts(const std::string& screenLine = {});
		bool SetOpen(bool open);
		bool IsOpen() const { return m_Open; }
		bool AutomationModerate(const std::string& action, int stableSeat);
		GUIControl* GetControl(const std::string& name) const;
		/// The panel's own control manager, for the script commands that read its rows.
		GUIControlManager* AutomationManager() const { return m_Controls.get(); }
		GUIControlManager* OverlayManager() const { return m_OverlayControls.get(); }
		/// Clicks a named panel control the way its own manager's mouse would.
		bool AutomationPostCommand(const std::string& name);
		/// Reads a named label's text for a script assert.
		bool AutomationLabelText(const std::string& name, std::string& text) const;
		/// The player rows the open panel lists, or zero while it is closed or shows the match's rules.
		size_t AutomationSeatRowCount() const { return m_Open && !m_OptionsView ? m_RowsShown : 0; }
		/// The rows the open host panel has across its pages, or zero while it is closed, shows the rules or is a client's.
		size_t AutomationRowsListed() const { return m_Open && !m_OptionsView ? m_Rows.size() : 0; }
		/// The rows the match's roster calls for on the open host panel, counted from the roster and not from the panel's rows.
		size_t AutomationRowsImplied() const;
		/// The controls a shown row must show for its player to be acted on, by name.
		std::vector<std::string> AutomationRowControls(size_t slot) const;

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
		/// What a wrapped surface was asked to show on the last frame it drew, so a readback can check
		/// the wrap held at word boundaries and the panel took its longest word.
		struct WrapAudit {
			std::string source;   //!< The text before wrapping.
			std::string wrapped;  //!< The lines the surface drew (the label's own input for the status box).
			int textWidth = 0;    //!< The pixel column the text wrapped into.
			int longestWord = 0;  //!< The widest whitespace-delimited token, measured at draw time.
			int capWidth = 0;     //!< The widest column the width rule allowed before the screen cap.
			bool active = false;  //!< Whether the surface drew on the last Draw pass.
		};
		const WrapAudit& GetRosterWrap() const { return m_RosterWrap; }
		const WrapAudit& GetStatusWrap() const { return m_StatusWrap; }
		/// Extra status line. Empty leaves the box unchanged.
		void SetStatusProbeLine(const std::string& line) { m_StatusProbeLine = line; }
		/// A toast-fill run no live overlay rect covers: the pixels a moved band left on a layer
		/// nothing cleared that frame (ENGINE 195's second band).
		struct GhostBandHit {
			bool found = false;
			int x = 0, y = 0, run = 0;
			int probePixel = -1;  //!< The raw pixel inside the live toast band, so a blind scan is visible.
		};
		/// Scans the GUI layer for a toast-fill run of at least minRunPx that no reported rect covers.
		GhostBandHit ScanGhostBand(int minRunPx) const;
		/// Arms a per-draw ghost scan: every DrawMatchToasts call counts a frame whose GUI layer
		/// still shows a band where nothing drew one, so a stale band caught between renders is seen.
		void ArmGhostWatch() { m_GhostWatchArmed = true; m_GhostWatchHits = 0; m_GhostWatchLast = {}; }
		void DisarmGhostWatch() { m_GhostWatchArmed = false; }
		int GhostWatchHits() const { return m_GhostWatchHits; }
		const GhostBandHit& GhostWatchLast() const { return m_GhostWatchLast; }
		int GhostWatchProbe() const { return m_GhostWatchProbe; }
		/// Wraps a text the way the roster box does - same helper, same width rule - so a script can
		/// check the wrap on a line the match may never produce.
		bool AutomationWrapLines(const std::string& text, std::string& wrapped, int& boxWidth) const;
		/// The roster box's width rule: the stock width, grown to the longest word, capped by the screen.
		static int RosterBoxWidth(GUIFont* font, const std::string& text);
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
		/// Whether the in-match net surfaces draw. They belong to the match, not to the running round: a
		/// round that completed or stopped still owes this peer its status box, chat and toasts until the
		/// match activity itself is over. The service detaches the coordinator before it reports the end,
		/// so an attached coordinator alone cannot answer this.
		/// @param controllerSyncActive The lockstep round is running.
		/// @param matchResyncing The service is rebuilding the round.
		/// @param hostLost The snapshot's status names a lost host.
		/// @param lockstepAttached A coordinator is still attached to this peer, running or not.
		/// @param matchEnded The service has completed the match this peer is still standing in.
		/// @param activityInMatch The match activity exists and is not over - a paused one still counts.
		/// @param postMatchLobby The finished match's lobby still stands on this peer: its pump is owed
		/// or the snapshot's playedAMatch mark is on.
		/// @param lobbyMenuActive The post-match lobby is the menu on screen.
		static bool MatchSurfacesDrawn(bool controllerSyncActive, bool matchResyncing, bool hostLost, bool lockstepAttached, bool matchEnded, bool activityInMatch, bool postMatchLobby, bool lobbyMenuActive);
		/// The match activity this peer is standing in, paused or not, until it ends.
		static bool ActivityInMatch();
		/// The menu-loop arm as the menu loop asks it: a finished match's lobby stands while its
		/// playedAMatch mark is on, and while the multiplayer lobby is the screen up it draws the
		/// surfaces the game loop did. The title screen, settings and every other menu leave it off.
		static bool PostMatchLobbySurfaces();

		/// One player's row on the host's panel: the name and state line, the host's actions and why any is off.
		struct Controls {
			GUILabel* name = nullptr;
			GUILabel* detail = nullptr;
			GUILabel* hint = nullptr;
			GUIListBox* requests = nullptr; //!< The people asking for this place; a pick is the one Let names.
			std::array<GUIButton*, 3> actions{}; //!< Keep, Let join and Cancel, in NetModerationAction order.
			GUIButton* remove = nullptr;
			GUIButton* ban = nullptr;
		};
		/// A player the host's panel lists: everyone in the match but the host, the held first.
		struct PanelRow {
			uint8_t peer = 0;
			std::string name;
			std::string state; //!< The state line the roster reads for this player.
			std::optional<NetModerationUx::Row> decision; //!< The held seat's model row, when the seat waits on the host.
			std::optional<NetH4ModerationSeat> seat;      //!< The admission row Remove and Ban act on.
			bool opened = false; //!< A place the host opened that a newcomer asks for: only Let acts on it.
		};
		/// An action that takes a second press: it names its consequence first.
		struct Armed {
			enum class Kind { Let, Remove, Ban } kind = Kind::Remove;
			size_t slot = 0; //!< The row of controls it was armed on; it dies when another player shows there.
			uint8_t peer = 0;
			uint16_t stableSeat = 0;
			uint32_t incarnation = 0;
			NetPeerId applicant = c_InvalidNetPeerId;
			uint64_t untilMs = 0;
		};
		/// A press on a row's control, kept until its release: it acts only if the release finds the same player there.
		struct Press {
			const GUIControl* button = nullptr;
			size_t slot = 0;
			PanelRow row;
		};
		/// The row a slot of controls shows on the current page.
		const PanelRow& SlotRow(size_t slot) const { return m_Rows[m_PageStart + slot]; }
		/// A row's height on a panel of this inner width.
		int RowHeight(const PanelRow& row, int inner) const;
		/// Ends a kept press that can no longer act: its row is off the page, shows another player, or the mouse is up.
		void DropStalePress(bool mouseDown);
		/// Ends an armed action whose player no longer shows on the controls it was armed on.
		void DropStaleArmed();
		void Refresh();
		/// Builds the host's rows from the roster and the admission rows.
		std::vector<PanelRow> BuildRows(const NetLobbySnapshot& snapshot);
		/// Lays out and fills one slot's controls from its top; returns its height.
		int FillRow(Controls& controls, const PanelRow& row, size_t slot, int top);
		void HideRow(Controls& controls);
		/// The second press of an armed action, or the first, which only says what the action will do.
		void PressArmed(const PanelRow& row, size_t slot, Armed::Kind kind);
		/// Reads the panel's events; returns whether there were any.
		bool HandleEvents();
		void DrawRoster(const NetLobbySnapshot& snapshot);
		/// Creates presentation controls only when an online match draws them.
		void CreateOverlay();
		/// Whether the status widget should be up, per NetworkMatchStatusMode: Off never, Always always, Auto on events and three seconds past recovery.
		bool MatchStatusWanted() const;
		/// Draws the status widget: the box on tall screens, a single-line strip in the top HUD gap on short ones.
		void DrawMatchStatus(const NetLobbySnapshot& snapshot);
		void RecordStatusObservation(const NetLobbySnapshot& snapshot, bool hostLost, long long currentWaitMs);
		void DrawMatchChat(const NetLobbySnapshot& snapshot);
		/// The chat's key every pass; its lines and whether it shows once a drawn frame (frameDue).
		void UpdateMatchChat(const NetLobbySnapshot& snapshot, bool frameDue);
		/// Whether the mouse moved or a button changed since the last pass; a click's edge lasts one pass.
		bool MouseChanged();
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
		WrapAudit m_RosterWrap;   //!< The roster box's last wrap, for the word-boundary check.
		WrapAudit m_StatusWrap;   //!< The status box's last wrap, for the word-boundary check.
		WrapAudit m_StatusLayoutWrap; //!< The wrap the kept status layout was drawn with; a frame that draws the kept box reports it.
		std::string m_StatusProbeLine; //!< Extra status line; empty leaves the box unchanged.
		bool m_GhostWatchArmed = false;  //!< Whether DrawMatchToasts runs the ghost scan after each draw.
		int m_GhostWatchHits = 0;        //!< Frames the armed watch saw a stale band on.
		GhostBandHit m_GhostWatchLast;   //!< The last stale run the watch saw.
		int m_GhostWatchProbe = -1;      //!< The last frame's pixel inside the live band, so a blind scan shows.
		void GhostWatchTick();
		long long m_StatusWaitStartedUs = 0;
		uint32_t m_StatusWaitReclaimEpoch = 0;
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
		std::string m_StatusLayoutKey; //!< Every input the status box was last laid out from.
		OverlayRect m_StatusLayoutRect; //!< Where that layout put the box.
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
		/// Turns to the next page of players when they do not all fit.
		GUIButton* m_More = nullptr;
		/// The F6 panel's second view: the match's adopted options, the same read-only panel the pause
		/// menu's Match Details and the lobby's Details show.
		GUIButton* m_OptionsToggle = nullptr;
		GUILabel* m_Options = nullptr;
		bool m_OptionsView = false;
		std::array<Controls, 3> m_Seats;
		std::vector<PanelRow> m_Rows;
		size_t m_RowsShown = 0;
		size_t m_RowsImplied = 0; //!< The rows the roster called for at the last refresh, counted apart from m_Rows.
		size_t m_PageStart = 0; //!< The first row of the page shown.
		uint8_t m_PageFirstPeer = 0; //!< That row's player, so the page stays put while rows come and go.
		bool m_PageTurn = false; //!< The next refresh shows the page after this one.
		std::optional<Armed> m_Armed;
		std::string m_PanelStatus; //!< The last action's line; empty leaves the model's.
		std::optional<Press> m_Press;
		NetModerationUx m_Model;
		std::optional<NetH4ModerationResult> m_ActionResult;
		std::vector<uint8_t> m_AnnouncedAISeats;
		uint64_t m_DepartureFrame = 0;
		std::unique_ptr<NetLobbySnapshot> m_FrameSnapshot; //!< The match as the last drawn frame read it, or Update when nothing drew.
		uint64_t m_DrawSerial = 0; //!< Drawn frames.
		uint64_t m_ReadDrawSerial = 0; //!< The drawn frame Update last followed the match at.
		uint64_t m_ReadMs = 0; //!< When Update last followed the match.
		bool m_ChatInMatch = false; //!< Whether the chat showed at the last drawn frame.
		int m_LastMouseX = -1, m_LastMouseY = -1;
		bool m_Open = false;
		int m_RefreshCount = 0;
		int m_RefreshChangeCount = 0;
		uint64_t m_LastRefreshHash = 0;
		long long m_LastRefreshMs = 0;
		/// The CC_TEST_PANEL_COST lever: per-frame time of the panel's update and draw, closed and open apart.
		struct FrameCost {
			std::vector<long long> closedUs, openUs, closedUpdateUs, openUpdateUs;
			long long frameUs = 0;
			long long updates = 0; //!< Update passes in the window: the loop comes by many times a drawn frame.
		};
		std::unique_ptr<FrameCost> m_Cost;
		void NoteFrameCost(long long drawUs);
	};
}
