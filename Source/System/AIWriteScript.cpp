#include "AIWriteScript.h"

#include "AHuman.h"
#include "Controller.h"
#include "MovableMan.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace RTE {

	namespace {
		struct Line {
			uint64_t tick = 0;
			int team = 0;
			std::string op;
			std::vector<std::string> args;
		};

		std::vector<Line> s_Lines;

		int ArgCount(const std::string& op) {
			if (op == "equip-group" || op == "equip-named" || op == "aim" || op == "flip") {
				return 1;
			}
			if (op == "equip-loaded" || op == "scene-waypoint") {
				return 2;
			}
			if (op == "scene-waypoint-for") {
				return 3;
			}
			return 0;
		}

		bool KnownOp(const std::string& op) {
			for (const char* known: {"equip-firearm", "equip-group", "equip-loaded", "equip-named", "equip-throwable", "equip-digger", "equip-shield", "equip-shield-bg", "unequip-fg", "unequip-bg", "flip", "aim", "scene-waypoint", "scene-waypoint-for", "clear-waypoints"}) {
				if (op == known) {
					return true;
				}
			}
			return false;
		}

		Actor* ResolveWaypointTarget(const std::string& token, const std::deque<Actor*>& actors, const Actor* writer) {
			if (token.rfind("slot=", 0) == 0) {
				const int slot = std::atoi(token.c_str() + 5);
				std::vector<Actor*> ranked(actors.begin(), actors.end());
				std::sort(ranked.begin(), ranked.end(), [](const Actor* left, const Actor* right) {
					return left->GetUniqueID() < right->GetUniqueID();
				});
				int index = 0;
				for (Actor* actor: ranked) {
					if (actor == writer) {
						continue;
					}
					if (index++ == slot) {
						return actor;
					}
				}
				return nullptr;
			}
			const long uid = std::strtol(token.c_str(), nullptr, 10);
			return dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(uid));
		}

		void Perform(AHuman& human, const Line& line) {
			if (line.op == "equip-firearm") {
				human.EquipFirearm(true);
			} else if (line.op == "equip-group") {
				human.EquipDeviceInGroup(line.args[0], true);
			} else if (line.op == "equip-loaded") {
				human.EquipLoadedFirearmInGroup(line.args[0], line.args[1], true);
			} else if (line.op == "equip-named") {
				human.EquipNamedDevice(line.args[0], true);
			} else if (line.op == "equip-throwable") {
				human.EquipThrowable(true);
			} else if (line.op == "equip-digger") {
				human.EquipDiggingTool(true);
			} else if (line.op == "equip-shield") {
				human.EquipShield();
			} else if (line.op == "equip-shield-bg") {
				human.EquipShieldInBGArm();
			} else if (line.op == "unequip-fg") {
				human.UnequipFGArm();
			} else if (line.op == "unequip-bg") {
				human.UnequipBGArm();
			} else if (line.op == "flip") {
				human.SetHFlipped(line.args[0] != "0");
			} else if (line.op == "aim") {
				human.SetAimAngle(std::strtof(line.args[0].c_str(), nullptr));
			} else if (line.op == "scene-waypoint") {
				human.AddAISceneWaypoint(Vector(std::strtof(line.args[0].c_str(), nullptr), std::strtof(line.args[1].c_str(), nullptr)));
			} else if (line.op == "clear-waypoints") {
				human.ClearAIWaypoints();
			}
		}
	} // namespace

	bool AIWriteScript::s_Active = false;

	bool AIWriteScript::Load(const std::string& path, std::string* error) {
		std::ifstream in(path);
		if (!in) {
			if (error) *error = "could not open AI write script: " + path;
			return false;
		}
		s_Lines.clear();
		std::string text;
		int lineNumber = 0;
		while (std::getline(in, text)) {
			++lineNumber;
			const size_t hash = text.find('#');
			if (hash != std::string::npos) {
				text.erase(hash);
			}
			std::istringstream words(text);
			std::vector<std::string> tokens;
			std::string word;
			while (words >> word) {
				tokens.push_back(word);
			}
			if (tokens.empty()) {
				continue;
			}
			if (tokens.size() < 3 || tokens[1].rfind("team=", 0) != 0 || !KnownOp(tokens[2]) || tokens.size() != 3 + static_cast<size_t>(ArgCount(tokens[2]))) {
				if (error) *error = "AI write script line " + std::to_string(lineNumber) + ": expected <tick> team=<n> <op> [args]";
				return false;
			}
			Line line;
			line.tick = std::strtoull(tokens[0].c_str(), nullptr, 10);
			line.team = std::atoi(tokens[1].c_str() + 5);
			line.op = tokens[2];
			line.args.assign(tokens.begin() + 3, tokens.end());
			s_Lines.push_back(std::move(line));
		}
		s_Active = true;
		std::cout << "[ai-write-script] " << path << ": " << s_Lines.size() << " lines" << std::endl;
		return true;
	}

	void AIWriteScript::RunTick(uint64_t simTick, const std::deque<Actor*>& actors, const std::function<bool(const Actor*)>& isLocal) {
		if (!s_Active) {
			return;
		}
		for (const Line& line: s_Lines) {
			if (line.tick != simTick) {
				continue;
			}
			AHuman* target = nullptr;
			for (Actor* actor: actors) {
				AHuman* human = dynamic_cast<AHuman*>(actor);
				if (human && human->GetTeam() == line.team && isLocal(actor) && !actor->GetController()->IsSeatedByPlayer() && (!target || human->GetUniqueID() < target->GetUniqueID())) {
					target = human;
				}
			}
			if (!target) {
				std::cout << "[ai-write-script] tick " << simTick << " team " << line.team << " " << line.op << ": no local AI actor" << std::endl;
				continue;
			}
			// The write happens the way a script's would: inside the AI pass of the actor's owner.
			g_CurrentAIActor = target;
			if (line.op == "scene-waypoint-for") {
				Actor* dest = ResolveWaypointTarget(line.args[0], actors, target);
				if (!dest) {
					g_CurrentAIActor = nullptr;
					std::cout << "[ai-write-script] tick " << simTick << " team " << line.team << " " << line.op << ": no target " << line.args[0] << std::endl;
					continue;
				}
				dest->AddAISceneWaypoint(Vector(std::strtof(line.args[1].c_str(), nullptr), std::strtof(line.args[2].c_str(), nullptr)));
				g_CurrentAIActor = nullptr;
				std::cout << "[ai-write-script] tick " << simTick << " team " << line.team << " " << line.op;
				for (const std::string& arg: line.args) {
					std::cout << " " << arg;
				}
				std::cout << " writer " << target->GetUniqueID() << " target " << dest->GetUniqueID() << std::endl;
				continue;
			}
			Perform(*target, line);
			g_CurrentAIActor = nullptr;
			std::cout << "[ai-write-script] tick " << simTick << " team " << line.team << " " << line.op;
			for (const std::string& arg: line.args) {
				std::cout << " " << arg;
			}
			std::cout << " on uid " << target->GetUniqueID() << std::endl;
		}
	}
} // namespace RTE
