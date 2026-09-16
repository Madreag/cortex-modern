#pragma once

#include "ActivityMan.h"
#include "GameActivity.h"
#include "MovableMan.h"
#include "NetGameCommand.h"
#include "ScenarioRunner.h"
#include "TimerMan.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace RTE {
namespace LocalPredictionFundsSelfTest {

	inline long long g_PressTick = 0;
	inline bool g_Checked = false;
	inline bool g_Issued = false;
	inline bool g_SampledPlusOne = false;
	inline bool g_SampledCommit = false;
	inline std::string g_LocalPlusOne;
	inline std::string g_OtherPlusOne;
	inline std::string g_LocalCommit;
	inline std::string g_OtherCommit;
	inline std::string g_DumpBefore;
	inline std::string g_DumpAfter;
	inline float g_CommittedPlusOne = 0;
	inline float g_CommittedCommit = 0;

	inline void IssueBuy(Activity& activity) {
		NetGameDeliverCargo buy;
		buy.queuedPurchase = true;
		buy.cost = 137;
		buy.team = activity.GetTeamOfPlayer(Players::PlayerOne);
		buy.orderedByPlayer = static_cast<int8_t>(Players::PlayerOne);
		ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, buy});
		activity.FillPresentationFromPreview(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) + 7);
	}

	inline void Sample(bool afterDraw) {
		if (g_PressTick <= 0 || g_Checked) {
			return;
		}
		Activity* activity = g_ActivityMan.GetActivity();
		GameActivity* game = dynamic_cast<GameActivity*>(activity);
		if (!game) {
			return;
		}
		const long long tick = g_TimerMan.GetSimUpdateCount();
		if (!afterDraw && tick == g_PressTick && !g_Issued) {
			std::ostringstream dump;
			g_MovableMan.DumpSimState(static_cast<uint64_t>(tick), dump);
			g_DumpBefore = dump.str();
			IssueBuy(*game);
			dump.str("");
			dump.clear();
			g_MovableMan.DumpSimState(static_cast<uint64_t>(tick), dump);
			g_DumpAfter = dump.str();
			g_Issued = true;
		}
		if (!afterDraw && tick == g_PressTick + 7 && g_Issued && !g_SampledCommit) {
			NetGameDeliverCargo buy;
			buy.queuedPurchase = true;
			buy.cost = 137;
			buy.team = game->GetTeamOfPlayer(Players::PlayerOne);
			buy.orderedByPlayer = static_cast<int8_t>(Players::PlayerOne);
			ScenarioRunner::DrainLocalGameCommands();
			ScenarioRunner::StageConsumedQueuedPurchase(NetGameCommand{0, buy}, static_cast<uint64_t>(tick));
			game->FillPresentationFromPreview(static_cast<uint64_t>(tick));
		}
		if (afterDraw && tick == g_PressTick + 1 && g_Issued && !g_SampledPlusOne) {
			g_LocalPlusOne = game->GetLastFundsReadout(Players::PlayerOne);
			g_OtherPlusOne = game->GetLastFundsReadout(Players::PlayerTwo);
			if (g_LocalPlusOne.empty()) {
				game->RecordFundsReadout(Players::PlayerOne);
				g_LocalPlusOne = game->GetLastFundsReadout(Players::PlayerOne);
			}
			if (g_OtherPlusOne.empty()) {
				game->RecordFundsReadout(Players::PlayerTwo);
				g_OtherPlusOne = game->GetLastFundsReadout(Players::PlayerTwo);
			}
			g_CommittedPlusOne = game->GetTeamFunds(game->GetTeamOfPlayer(Players::PlayerOne));
			g_SampledPlusOne = true;
		}
		if (afterDraw && tick == g_PressTick + 7 && g_Issued && !g_SampledCommit) {
			g_LocalCommit = game->GetLastFundsReadout(Players::PlayerOne);
			g_OtherCommit = game->GetLastFundsReadout(Players::PlayerTwo);
			if (g_LocalCommit.empty()) {
				game->RecordFundsReadout(Players::PlayerOne);
				g_LocalCommit = game->GetLastFundsReadout(Players::PlayerOne);
			}
			if (g_OtherCommit.empty()) {
				game->RecordFundsReadout(Players::PlayerTwo);
				g_OtherCommit = game->GetLastFundsReadout(Players::PlayerTwo);
			}
			g_CommittedCommit = game->GetTeamFunds(game->GetTeamOfPlayer(Players::PlayerOne));
			g_SampledCommit = true;
		}
	}

	inline bool Check() {
		if (g_PressTick <= 0 || g_Checked) {
			return true;
		}
		g_Checked = true;
		bool passed = true;
		const auto check = [&passed](const char* name, bool ok, const std::string& detail) {
			std::cout << "[preview-funds-driver] " << (ok ? "PASS " : "FAIL ") << name << ": " << detail << std::endl;
			passed = passed && ok;
		};
		const std::string committedPlusOne = std::to_string(static_cast<int>(g_CommittedPlusOne));
		const std::string previewedPlusOne = std::to_string(static_cast<int>(g_CommittedPlusOne - 137));
		check("local_readout_follows_the_preview_at_press_plus_one", g_SampledPlusOne && g_LocalPlusOne.find(previewedPlusOne) != std::string::npos,
		      "local " + g_LocalPlusOne + " committed " + committedPlusOne);
		check("other_peer_stays_committed_at_press_plus_one", g_SampledPlusOne && g_OtherPlusOne.find(committedPlusOne) != std::string::npos,
		      "other " + g_OtherPlusOne + " committed " + committedPlusOne);
		const std::string committedNow = std::to_string(static_cast<int>(g_CommittedCommit));
		check("both_readouts_equal_at_commit", g_SampledCommit && g_LocalCommit == g_OtherCommit && g_LocalCommit.find(committedNow) != std::string::npos,
		      "local " + g_LocalCommit + " other " + g_OtherCommit + " committed " + committedNow);
		check("funds_driver_dumps_byte_identical", g_Issued && g_DumpBefore == g_DumpAfter,
		      g_DumpBefore == g_DumpAfter ? "argv dump unchanged after the queued buy" : "dump changed after Enqueue+Fill");
		std::cout << "[preview-funds-driver] " << (passed ? "PASS" : "FAIL") << " press tick " << g_PressTick << std::endl;
		return passed;
	}

} // namespace LocalPredictionFundsSelfTest
} // namespace RTE
