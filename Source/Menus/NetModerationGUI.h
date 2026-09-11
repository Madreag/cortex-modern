#pragma once

#include "NetReconnectUx.h"

#include <array>
#include <memory>
#include <optional>

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
		bool SetOpen(bool open);
		bool IsOpen() const { return m_Open; }
		bool AutomationModerate(const std::string& action, int stableSeat);
		GUIControl* GetControl(const std::string& name) const;

	private:
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
		std::unique_ptr<GUIInputWrapper> m_Input;
		std::unique_ptr<GUIControlManager> m_Controls;
		GUICollectionBox* m_Panel = nullptr;
		GUIFont* m_LabelFont = nullptr;
		GUILabel* m_Title = nullptr;
		GUILabel* m_Summary = nullptr;
		GUILabel* m_Status = nullptr;
		GUILabel* m_Roster = nullptr;
		GUIButton* m_Close = nullptr;
		std::array<Controls, 3> m_Seats;
		std::optional<Press> m_Press;
		NetModerationUx m_Model;
		std::optional<NetH4ModerationResult> m_ActionResult;
		bool m_Open = false;
	};
}
