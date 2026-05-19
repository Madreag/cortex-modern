#include "AIDebugOverlay.h"

#include "AIDecisionChannel.h"
#include "Actor.h"
#include "AHuman.h"
#include "HeldDevice.h"
#include "MovableMan.h"
#include "NetworkSimulator.h"
#include "SimChecksum.h"

#include "imgui.h"

#include <algorithm>

namespace RTE {

	AIDebugOverlay::AIDebugOverlay() = default;
	AIDebugOverlay::~AIDebugOverlay() = default;

	void AIDebugOverlay::SetHistoryLength(int n) {
		m_HistoryLength = std::clamp(n, 1, 64);
	}

	static const char* AIModeName(int mode) {
		switch (mode) {
			case Actor::AIMODE_NONE: return "NONE";
			case Actor::AIMODE_SENTRY: return "SENTRY";
			case Actor::AIMODE_PATROL: return "PATROL";
			case Actor::AIMODE_GOTO: return "GOTO";
			case Actor::AIMODE_BRAINHUNT: return "BRAINHUNT";
			case Actor::AIMODE_GOLDDIG: return "GOLDDIG";
			case Actor::AIMODE_RETURN: return "RETURN";
			case Actor::AIMODE_STAY: return "STAY";
			case Actor::AIMODE_SCUTTLE: return "SCUTTLE";
			case Actor::AIMODE_DELIVER: return "DELIVER";
			case Actor::AIMODE_BOMB: return "BOMB";
			case Actor::AIMODE_SQUAD: return "SQUAD";
			default: return "?";
		}
	}

	void AIDebugOverlay::Draw(const Actor* currentActor) {
		if (!m_Enabled) {
			return;
		}
		if (ImGui::GetCurrentContext() == nullptr) {
			return;
		}

		int actorId = (m_WatchedActorId >= 0) ? m_WatchedActorId :
		              (currentActor ? currentActor->GetUniqueID() : -1);

		const Actor* watched = nullptr;
		if (actorId >= 0) {
			MovableObject* mo = g_MovableMan.FindObjectByUniqueID(actorId);
			watched = dynamic_cast<const Actor*>(mo);
			if (!watched && currentActor) {
				watched = currentActor;
				actorId = currentActor->GetUniqueID();
			}
		}

		ImGui::SetNextWindowSize(ImVec2(460, 420), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("AI Decision Overlay", &m_Enabled)) {
			ImGui::End();
			return;
		}

		const uint64_t tick = g_AIDecisionChannel.GetCurrentTick();
		const std::string hashHex = SimChecksum::HashHex(g_SimChecksum.GetLastResult().total);
		const std::string hashTail = hashHex.size() >= 8 ? hashHex.substr(hashHex.size() - 8) : hashHex;
		ImGui::Text("Tick: %llu   Hash: ...%s", static_cast<unsigned long long>(tick), hashTail.c_str());

		if (g_NetworkSimulator.IsActive()) {
			ImGui::SameLine();
			ImGui::TextDisabled(" | NetSim: lat=%dms loss=%d%% jit=%dms",
			                    g_NetworkSimulator.GetLatencyMs(),
			                    g_NetworkSimulator.GetLossPct(),
			                    g_NetworkSimulator.GetJitterMs());
		}

		ImGui::Separator();

		if (!watched) {
			ImGui::TextDisabled("No actor selected.");
			ImGui::TextWrapped("Trust scenarios auto-watch the first spawned actor; in normal play, set "
			                   "AIDebugOverlay.WatchedActorId from the in-game console (~) to a target id.");
			ImGui::End();
			return;
		}

		const std::string presetName = watched->GetPresetName();
		const std::string moduleName = watched->GetModuleName();
		ImGui::Text("Actor #%d  %s.%s", actorId, moduleName.c_str(), presetName.c_str());

		const Vector& pos = watched->GetPos();
		const Vector& vel = watched->GetVel();
		ImGui::Text("Pos:   (%.0f, %.0f)    Vel: (%.1f, %.1f)", pos.GetX(), pos.GetY(), vel.GetX(), vel.GetY());
		ImGui::Text("Team:  %d    Health: %.0f / %.0f    AIMode: %s",
		            watched->GetTeam(),
		            watched->GetHealth(), watched->GetMaxHealth(),
		            AIModeName(watched->GetAIMode()));

		if (const AHuman* asHuman = dynamic_cast<const AHuman*>(watched)) {
			const HeldDevice* equipped = asHuman->GetEquippedItem();
			if (equipped) {
				ImGui::Text("Weapon: %s", equipped->GetPresetName().c_str());
			} else {
				ImGui::TextDisabled("Weapon: (none equipped)");
			}
		}

		ImGui::Separator();

		std::vector<AIDecisionChannel::Event> events;
		g_AIDecisionChannel.PeekRecent(actorId, static_cast<size_t>(m_HistoryLength), events);

		if (events.empty()) {
			ImGui::TextDisabled("(no decision events yet)");
			ImGui::End();
			return;
		}

		if (ImGui::BeginTable("events", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 72);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 116);
			ImGui::TableSetupColumn("Chosen", ImGuiTableColumnFlags_WidthFixed, 116);
			ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			for (const auto& e: events) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%llu", static_cast<unsigned long long>(e.tick));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(AIDecisionChannel::LayerName(e.layer));
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(std::string(g_AIDecisionChannel.GetString(e.type)).c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(std::string(g_AIDecisionChannel.GetString(e.chosen)).c_str());
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(std::string(g_AIDecisionChannel.GetString(e.reason)).c_str());
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}

} // namespace RTE
