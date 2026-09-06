#include "InputScript.h"

#include "Constants.h"
#include "Vector.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace RTE {

	namespace {
		struct ElementRange {
			int player = 0;
			uint64_t from = 0;
			uint64_t to = 0;
			int element = 0;
		};

		struct VectorRange {
			int player = 0;
			uint64_t from = 0;
			uint64_t to = 0;
			Vector value;
		};

		std::vector<ElementRange> s_Elements;
		std::vector<VectorRange> s_Aims;
		std::vector<VectorRange> s_Mouse;
		std::array<bool, Players::MaxPlayerCount> s_PlayerDriven{};

		// Names follow the InputElements enum without its INPUT_ prefix, in enum order.
		constexpr const char* c_ElementNames[] = {
		    "L_UP", "L_DOWN", "L_LEFT", "L_RIGHT", "MOVE_FAST", "MOVE_FAST_TOGGLE", "AIM_UP", "AIM_DOWN", "AIM_LEFT", "AIM_RIGHT", "FIRE", "AIM",
		    "PIEMENU_ANALOG", "PIEMENU_DIGITAL", "JUMP", "CROUCH", "PRONE", "NEXT", "PREV", "WEAPON_CHANGE_NEXT", "WEAPON_CHANGE_PREV",
		    "WEAPON_PICKUP", "WEAPON_DROP", "WEAPON_RELOAD", "WEAPON_PRIMARY_HOTKEY", "WEAPON_AUXILIARY_HOTKEY", "ACTOR_PRIMARY_HOTKEY",
		    "ACTOR_AUXILIARY_HOTKEY", "START", "BACK", "R_UP", "R_DOWN", "R_LEFT", "R_RIGHT"};
		static_assert(sizeof(c_ElementNames) / sizeof(c_ElementNames[0]) == InputElements::INPUT_COUNT, "InputScript element names must cover the enum");

		bool ParseVector(const std::string& text, Vector& out) {
			const size_t comma = text.find(',');
			if (comma == std::string::npos) {
				return false;
			}
			char* endX = nullptr;
			char* endY = nullptr;
			const float x = std::strtof(text.c_str(), &endX);
			const float y = std::strtof(text.c_str() + comma + 1, &endY);
			if (endX != text.c_str() + comma || *endY != '\0') {
				return false;
			}
			out.SetXY(x, y);
			return true;
		}
	} // namespace

	bool InputScript::s_Active = false;
	std::string InputScript::s_Path;

	int InputScript::ElementFromName(const std::string& name) {
		for (int element = 0; element < InputElements::INPUT_COUNT; ++element) {
			if (name == c_ElementNames[element]) {
				return element;
			}
		}
		return -1;
	}

	const char* InputScript::ElementName(int element) {
		return element >= 0 && element < InputElements::INPUT_COUNT ? c_ElementNames[element] : "?";
	}

	bool InputScript::Load(const std::string& path, std::string* error) {
		std::ifstream in(path);
		if (!in) {
			if (error) *error = "could not open input script: " + path;
			return false;
		}
		s_Elements.clear();
		s_Aims.clear();
		s_Mouse.clear();
		s_PlayerDriven.fill(false);
		std::string line;
		int lineNumber = 0;
		while (std::getline(in, line)) {
			++lineNumber;
			const size_t hash = line.find('#');
			if (hash != std::string::npos) {
				line.erase(hash);
			}
			std::istringstream words(line);
			std::string word;
			std::vector<std::string> tokens;
			while (words >> word) {
				tokens.push_back(word);
			}
			if (tokens.empty()) {
				continue;
			}
			int player = 0;
			size_t index = 0;
			if (tokens[0].rfind("player=", 0) == 0) {
				player = std::atoi(tokens[0].c_str() + 7);
				++index;
			}
			if (player < 0 || player >= Players::MaxPlayerCount || tokens.size() < index + 3) {
				if (error) *error = "input script line " + std::to_string(lineNumber) + ": expected [player=N] <from> <to> ACTION...";
				return false;
			}
			const uint64_t from = std::strtoull(tokens[index].c_str(), nullptr, 10);
			const uint64_t to = std::strtoull(tokens[index + 1].c_str(), nullptr, 10);
			if (to < from) {
				if (error) *error = "input script line " + std::to_string(lineNumber) + ": the range ends before it starts";
				return false;
			}
			for (size_t i = index + 2; i < tokens.size(); ++i) {
				const std::string& action = tokens[i];
				Vector value;
				if (action.rfind("AIM=", 0) == 0) {
					if (!ParseVector(action.substr(4), value)) {
						if (error) *error = "input script line " + std::to_string(lineNumber) + ": bad AIM vector";
						return false;
					}
					s_Aims.push_back({player, from, to, value});
				} else if (action.rfind("MOUSE=", 0) == 0) {
					if (!ParseVector(action.substr(6), value)) {
						if (error) *error = "input script line " + std::to_string(lineNumber) + ": bad MOUSE vector";
						return false;
					}
					s_Mouse.push_back({player, from, to, value});
				} else {
					const int element = ElementFromName(action);
					if (element < 0) {
						if (error) *error = "input script line " + std::to_string(lineNumber) + ": unknown action " + action;
						return false;
					}
					s_Elements.push_back({player, from, to, element});
				}
			}
			s_PlayerDriven[player] = true;
		}
		s_Path = path;
		s_Active = true;
		std::cout << "[input-script] " << path << ": " << s_Elements.size() << " element ranges, " << s_Aims.size() << " aim ranges, " << s_Mouse.size() << " mouse ranges" << std::endl;
		return true;
	}

	bool InputScript::HeldAt(int player, int element, uint64_t simTick) {
		for (const ElementRange& range: s_Elements) {
			if (range.player == player && range.element == element && simTick >= range.from && simTick <= range.to) {
				return true;
			}
		}
		return false;
	}

	bool InputScript::AimAt(int player, uint64_t simTick, Vector& outAim) {
		for (const VectorRange& range: s_Aims) {
			if (range.player == player && simTick >= range.from && simTick <= range.to) {
				outAim = range.value;
				return true;
			}
		}
		return false;
	}

	bool InputScript::MouseAt(int player, uint64_t simTick, Vector& outMovement) {
		for (const VectorRange& range: s_Mouse) {
			if (range.player == player && simTick >= range.from && simTick <= range.to) {
				outMovement = range.value;
				return true;
			}
		}
		return false;
	}

	bool InputScript::DrivesPlayer(int player) {
		return s_Active && player >= 0 && player < Players::MaxPlayerCount && s_PlayerDriven[player];
	}
} // namespace RTE
