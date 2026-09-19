#include "MenuAutomation.h"
#include "SettingsGUI.h"
#include "MenuMan.h"
#include "MainMenuGUI.h"

#include "GUI.h"
#include "GUIFont.h"
#include "GUIButton.h"
#include "GUICheckbox.h"
#include "GUIComboBox.h"
#include "GUIInputWrapper.h"
#include "GUILabel.h"
#include "GUIListPanel.h"
#include "GUIRadioButton.h"
#include "GUITab.h"
#include "GUITextBox.h"
#include "FrameMan.h"
#include "GAScripted.h"
#include "GameActivity.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "PresetMan.h"
#include "Scene.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "UInputMan.h"
#include "WindowMan.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <list>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace RTE::MenuAutomation {
	using Json = nlohmann::json;
	using Rect = std::array<int, 4>;
	Rect Rectangle(GUIPanel* panel) {
		Rect rect{0, 0, g_WindowMan.GetResX(), g_WindowMan.GetResY()};
		if (panel) panel->GetRect(&rect[0], &rect[1], &rect[2], &rect[3]);
		return rect;
	}
	bool Visible(GUIControl* control) {
		if (!control || !control->GetPanel()) return false;
		for (auto* node = control->GetPanel(); node; node = node->GetParentPanel()) if (!node->_GetVisible()) return false;
		return true;
	}
	bool Enabled(GUIControl* control) {
		if (!Visible(control)) return false;
		for (auto* node = control->GetPanel(); node; node = node->GetParentPanel()) if (!node->_GetEnabled()) return false;
		return true;
	}
	// Both settings skins name a page's tab and box after the page, so scripts address pages by name.
	constexpr std::array<std::string_view, 6> c_SettingsPages{"Video", "Audio", "Input", "Gameplay", "Misc", "Network"};
	// The network page's own selector names its sub-pages the same way; a script addresses
	// one as "Network:<page>" once the network page is up.
	constexpr std::array<std::string_view, 5> c_NetworkPages{"Player", "Chat", "Recovery", "Files", "Internet"};

	std::string SettingsPage(GUIControlManager* manager) {
		for (const std::string_view page: c_SettingsPages) {
			if (manager && Visible(manager->GetControl("CollectionBox" + std::string(page) + "Settings"))) {
				if (page == "Network") {
					for (const std::string_view sub: c_NetworkPages) {
						if (Visible(manager->GetControl("CollectionBoxNetPage" + std::string(sub)))) return "Network:" + std::string(sub);
					}
				}
				return std::string(page);
			}
		}
		return "";
	}

	// A manager's Update clears its event queue, so a page request waits for the settings menu's own pass.
	static std::unordered_map<GUIControlManager*, SettingsGUI*> s_SettingsOwners;

	GUITab* PageTab(GUIControlManager* manager, const std::string& page) {
		const size_t colon = page.find(':');
		if (colon != std::string::npos) {
			const std::string sub = page.substr(colon + 1);
			const bool known = page.substr(0, colon) == "Network" && std::find(c_NetworkPages.begin(), c_NetworkPages.end(), sub) != c_NetworkPages.end();
			return known && manager ? dynamic_cast<GUITab*>(manager->GetControl("TabNetPage" + sub)) : nullptr;
		}
		const bool known = std::find(c_SettingsPages.begin(), c_SettingsPages.end(), page) != c_SettingsPages.end();
		return known && manager ? dynamic_cast<GUITab*>(manager->GetControl("Tab" + page + "Settings")) : nullptr;
	}

	void BindSettingsOwner(GUIControlManager* manager, SettingsGUI* owner) {
		if (manager && owner) s_SettingsOwners[manager] = owner;
	}

	void UnbindSettingsOwner(GUIControlManager* manager) {
		s_SettingsOwners.erase(manager);
	}

	bool QueuePage(GUIControlManager* manager, const std::string& page) {
		auto owner = s_SettingsOwners.find(manager);
		if (owner == s_SettingsOwners.end() || !Enabled(PageTab(manager, page))) return false;
		owner->second->QueuePendingPage(page);
		return true;
	}

	void ApplyQueuedPage(GUIControlManager* manager) {
		auto owner = s_SettingsOwners.find(manager);
		if (!manager || owner == s_SettingsOwners.end() || owner->second->PendingPage().empty()) return;
		GUITab* tab = PageTab(manager, owner->second->PendingPage());
		owner->second->ClearPendingPage();
		if (!Enabled(tab)) return;
		// The settings menu switches pages on the notification a tab click raises, so raise that.
		tab->SetCheck(true);
		tab->AddEvent(GUIEvent::Notification, GUITab::UnPushed, 0);
	}
	bool Text(GUIControl* control, std::string& text) {
		if (auto* value = dynamic_cast<GUILabel*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIButton*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUITextBox*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUICheckbox*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIRadioButton*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUITab*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIComboBox*>(control)) text = value->GetSelectedItem() ? value->GetSelectedItem()->m_Name : value->GetText();
		else return false;
		return true;
	}
	void Click(GUIControlManager* manager, const std::string& name) {
		auto* control = manager->GetControl(name);
		if (!Enabled(control)) return;
		auto* panel = control->GetPanel();
		const auto r = Rectangle(panel);
		panel->OnMouseDown(r[0] + r[2] / 2, r[1] + r[3] / 2, GUIPanel::MOUSE_LEFT, 0);
		panel->OnMouseUp(r[0] + r[2] / 2, r[1] + r[3] / 2, GUIPanel::MOUSE_LEFT, 0);
	}
	bool Inside(const Rect& child, const Rect& parent) {
		return child[2] > 0 && child[3] > 0 && child[0] >= parent[0] && child[1] >= parent[1] &&
			child[0] + child[2] <= parent[0] + parent[2] && child[1] + child[3] <= parent[1] + parent[3];
	}
	bool TextFits(GUIControlManager* manager, GUIControl* control, std::string& observation) {
		std::string text;
		const bool measureHiddenPreset = control && control->GetName() == "ComboPresetResolution";
		if ((!Visible(control) && !measureHiddenPreset) || !Text(control, text)) return false;
		const auto rect = Rectangle(control->GetPanel());
		observation += " text=" + Json(text).dump() + " rect=" + Json(rect).dump();
		if (auto* label = dynamic_cast<GUILabel*>(control)) {
			observation += " height=" + std::to_string(label->GetTextHeight()) + " word_width=" + std::to_string(label->GetMaxWordWidth());
			if (label->GetHorizontalOverflowScroll() && control->GetName() == "LabelMultiplayerStatus") {
				return label->GetTextHeight() <= rect[3];
			}
			return label->GetTextHeight() <= rect[3] && label->GetMaxWordWidth() <= rect[2];
		}
		std::string section = dynamic_cast<GUIButton*>(control) ? "Button_Up" : dynamic_cast<GUITab*>(control) ? "Tab" :
			dynamic_cast<GUICheckbox*>(control) ? "Checkbox" : dynamic_cast<GUIRadioButton*>(control) ? "RadioButton" :
			dynamic_cast<GUITextBox*>(control) || dynamic_cast<GUIComboBox*>(control) ? "TextBox" : "Label";
		std::string fontName;
		auto* skin = manager->GetSkin();
		if (!skin->GetValue(section, "Font", &fontName)) return false;
		auto* font = skin->GetFont(fontName);
		if (!font) return false;
		int kerning = 0, width = rect[2], height = rect[3];
		skin->GetValue(section, "FontKerning", &kerning);
		const int savedKerning = font->GetKerning();
		font->SetKerning(kerning);
		if (section == "Button_Up") {
			for (const auto* side : {"Left", "Right", "Top", "Bottom"}) {
				int border[4]{};
				skin->GetValue(section, side, border, 4);
				if (std::string_view(side) == "Left" || std::string_view(side) == "Right") width -= border[2]; else height -= border[3];
			}
			--width; --height;
		} else if (section == "Tab") { text = " " + text; width -= 4; }
		else if (section == "Checkbox" || section == "RadioButton") {
			int base[4]{}; skin->GetValue(section, "Base", base, 4);
			width -= base[2] + (section == "Checkbox" ? 2 : 0); text = " " + text;
		} else if (section == "TextBox") {
			int margin = 3, top = 0;
			skin->GetValue(section, "WidthMargin", &margin); skin->GetValue(section, "HeightMargin", &top);
			width -= 2 * margin; height -= top;
			// A combo box shows its selected item in a text panel the 17 pixel drop-down button covers.
			if (dynamic_cast<GUIComboBox*>(control)) width -= 17;
		}
		const int textWidth = font->CalculateWidth(text), textHeight = font->CalculateHeight(text);
		observation += " measured=" + Json({textWidth, textHeight}).dump() + " available=" + Json({width, height}).dump();
		const bool fits = textWidth <= width && textHeight <= height;
		font->SetKerning(savedKerning);
		return fits;
	}
	bool Handles(const std::string& command) {
		return command == "assert_visible" || command == "assert_focus" || command == "assert_rect_inside" || command == "assert_text_fits" ||
			command == "dump_host_options" || command == "dump_player_options" || command == "focus_next" || command == "focus_previous" || command == "key" || command == "pad" ||
			command == "key_down" || command == "key_up" || command == "focus" ||
			command == "set_text" || command == "set_share_address" || command == "combo_drop" || command == "combo_select" ||
			command == "select_settings_page" || command == "assert_settings_page";
	}
	bool Execute(GUIControlManager* manager, const std::string& screen, const std::string& command, std::istream& args, std::string& observation) {
		try {
			if (!manager) { observation = "no active control manager"; return false; }
			if (command == "key_down" || command == "key_up") {
				std::string key;
				args >> std::quoted(key);
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				observation = key;
				return input && !key.empty() && input->QueueAutomationInput("key", key, command == "key_down");
			}
			if (command == "focus") {
				std::string target;
				args >> std::quoted(target);
				auto* control = manager->GetControl(target);
				if (!control || !Enabled(control) || !control->GetPanel()) {
					observation = target + " missing or disabled";
					return false;
				}
				manager->GetManager()->SetFocus(control->GetPanel());
				const auto r = Rectangle(control->GetPanel());
				g_UInputMan.SetAbsoluteMousePosition(Vector(r[0] + r[2] / 2, r[1] + r[3] / 2) * g_WindowMan.GetResMultiplier());
				observation = target;
				return true;
			}
			if (command == "combo_drop" || command == "combo_select") {
				std::string comboName;
				args >> std::quoted(comboName);
				std::string item;
				std::getline(args >> std::ws, item);
				auto* combo = dynamic_cast<GUIComboBox*>(manager->GetControl(comboName));
				if (!combo || !Enabled(combo)) { observation = comboName + " missing or disabled"; return false; }
				observation = comboName + (item.empty() ? "" : " " + item);
				if (command == "combo_drop") {
					// The same state the text-panel click produces: the list shows, takes focus and
					// the mouse, sits topmost, and the control notifies Dropped.
					auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
					return input && input->QueueAutomationCommand([combo] {
						GUIListPanel* list = combo->GetListPanel();
						list->_SetVisible(true);
						list->SetFocus();
						list->CaptureMouse();
						list->EndUpdate();
						list->ChangeZPosition(GUIPanel::TopMost);
						combo->AddEvent(GUIEvent::Notification, GUIComboBox::Dropped, 0);
					});
				}
				if (item.empty()) { observation += " no item"; return false; }
				int index = -1;
				for (int i = 0; i < combo->GetCount(); ++i) {
					if (const GUIListPanel::Item* entry = combo->GetItem(i); entry && entry->m_Name == item) { index = i; break; }
				}
				if (index < 0) { observation += " no such item"; return false; }
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				GUIManager* gui = manager->GetManager();
				return input && input->QueueAutomationCommand([combo, index, gui] {
					GUIListPanel* list = combo->GetListPanel();
					// A scripted pick takes the row-click's close path so listeners see the same event.
					list->_SetVisible(false);
					list->ReleaseMouse();
					gui->SetFocus(nullptr);
					combo->SetSelectedIndex(index);
					combo->AddEvent(GUIEvent::Notification, GUIComboBox::Closed, 0);
				});
			}
			std::string name, argument, extra;
			args >> std::quoted(name) >> argument >> extra;
			if (!extra.empty()) { observation = "unexpected arguments"; return false; }
			auto* control = manager->GetControl(name);
			if (command == "set_share_address") {
				if (name.empty() || !argument.empty()) { observation = "need one address"; return false; }
				if (auto* menu = g_MenuMan.GetMainMenu()) {
					menu->AutomationSetShareAddress(name);
					observation = name;
					return true;
				}
				observation = "no main menu";
				return false;
			}
			if (command == "key" || command == "pad") {
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				observation = name + " " + argument;
				return input && (argument == "down" || argument == "up") && input->QueueAutomationInput(command, name, argument == "down");
			}
			if (command == "focus_next" || command == "focus_previous") {
				if (!name.empty()) return false;
				std::vector<GUIControl*> controls;
				for (auto* item : *manager->GetControlList()) if (Enabled(item) && !item->IsContainer() && !dynamic_cast<GUILabel*>(item)) controls.push_back(item);
				std::stable_sort(controls.begin(), controls.end(), [](auto* a, auto* b) {
					const auto x = Rectangle(a->GetPanel()), y = Rectangle(b->GetPanel());
					return std::tie(x[1], x[0]) < std::tie(y[1], y[0]);
				});
				if (controls.empty()) return false;
				const auto current = std::find_if(controls.begin(), controls.end(), [](auto* item) { return item->GetPanel()->HasFocus(); });
				const int count = static_cast<int>(controls.size()), index = static_cast<int>(current - controls.begin());
				const int next = current == controls.end() ? (command == "focus_next" ? 0 : count - 1) : (index + (command == "focus_next" ? 1 : count - 1)) % count;
				auto* target = controls[next];
				manager->GetManager()->SetFocus(target->GetPanel());
				const auto r = Rectangle(target->GetPanel());
				g_UInputMan.SetAbsoluteMousePosition(Vector(r[0] + r[2] / 2, r[1] + r[3] / 2) * g_WindowMan.GetResMultiplier());
				observation = target->GetName();
				return true;
			}
			if (command == "select_settings_page" || command == "assert_settings_page") {
				const std::string active = SettingsPage(manager);
				observation = name + " active=" + active;
				if (!argument.empty()) return false;
				// "Network" asserts wherever its selector sits; "Network:Chat" asserts one page.
				if (command == "assert_settings_page") {
					return active == name || (active.size() > name.size() && active.compare(0, name.size(), name) == 0 && active[name.size()] == ':');
				}
				return QueuePage(manager, name);
			}
			if (command == "dump_host_options" || command == "dump_player_options") {
				if (!name.empty()) return false;
				static unsigned int capture = 0;
				const auto path = std::filesystem::path("ScreenShots") / (command + "_" + std::to_string(capture++));
				const auto lobby = g_NetMatchService.GetLobbySnapshot();
				Json table = Json::array();
				for (const NetHostActivityChoice& activity : NetMatchService::ListHostActivities()) {
					Json scenes = Json::array();
					for (const NetHostSceneChoice& scene : activity.scenes) {
						scenes.push_back({{"name", scene.name}, {"module", scene.module}});
					}
					table.push_back({{"preset", activity.preset}, {"module", activity.module},
						{"activity_type", activity.activityType}, {"scenes", scenes}});
				}
				Json loaded = Json::array();
				std::list<Entity*> activityPresets;
				g_PresetMan.GetAllOfType(activityPresets, "Activity");
				for (Entity* entity : activityPresets) {
					auto* activity = dynamic_cast<GameActivity*>(entity);
					if (!activity || activity->IsTestActivity()) {
						continue;
					}
					Json required = Json::array();
					if (auto* scripted = dynamic_cast<GAScripted*>(activity)) {
						for (const std::string& area : scripted->GetRequiredAreas()) {
							required.push_back(area);
						}
					}
					loaded.push_back({{"preset", activity->GetPresetName()},
						{"module", g_PresetMan.GetDataModuleName(activity->GetModuleID())},
						{"activity_type", activity->GetClassName()},
						{"min_teams", activity->GetMinTeamsRequired()},
						{"required_areas", required}});
				}
				Json loadedScenes = Json::array();
				std::list<Entity*> scenePresets;
				g_PresetMan.GetAllOfType(scenePresets, "Scene");
				for (Entity* entity : scenePresets) {
					auto* scene = dynamic_cast<Scene*>(entity);
					if (!scene) {
						continue;
					}
					Json areas = Json::array();
					for (const Scene::Area* area : scene->GetAreas()) {
						if (area) {
							areas.push_back(area->GetName());
						}
					}
					loadedScenes.push_back({{"name", scene->GetPresetName()},
						{"module", g_PresetMan.GetDataModuleName(scene->GetModuleID())},
						{"areas", areas},
						{"location_zero", scene->GetLocation().IsZero()},
						{"metagame_internal", scene->IsMetagameInternal()},
						{"saved_game_internal", scene->IsSavedGameInternal()},
						{"metascene_parent", scene->GetMetasceneParent()}});
				}
				Json result = {{"schema", 1}, {"screen", screen}, {"settings_page", SettingsPage(manager)}, {"viewport", Rectangle(nullptr)}, {"service", lobby.serviceState},
					{"phase", "after_draw"}, {"sim_frame", g_TimerMan.GetSimUpdateCount()}, {"host", lobby.isHost}, {"peer_id", lobby.localPeerId},
					{"activity_preset", lobby.activityPreset}, {"activity_module", lobby.activityModule},
					{"scene_name", lobby.sceneName}, {"scene_module", lobby.sceneModule},
					{"activity_table", table}, {"game_activities", loaded}, {"loaded_scenes", loadedScenes},
					{"show_metascenes", g_SettingsMan.ShowMetascenes()}, {"controls", Json::array()}};
				for (auto* item : *manager->GetControlList()) {
					const bool dumpHiddenPreset = item->GetName() == "ComboPresetResolution";
					if (!Visible(item) && !dumpHiddenPreset) continue;
					auto* panel = item->GetPanel();
					auto* parent = dynamic_cast<GUIControl*>(panel->GetParentPanel());
					std::string text;
					Json row = {{"name", item->GetName()}, {"rect", Rectangle(panel)}, {"parent", parent ? parent->GetName() : ""},
						{"parent_rect", Rectangle(panel->GetParentPanel())}, {"text", Text(item, text) ? text : ""}, {"enabled", Enabled(item) || dumpHiddenPreset}, {"visible", Visible(item)}, {"focus", panel->HasFocus()}};
					if (auto* label = dynamic_cast<GUILabel*>(item)) {
						row["overflow_scroll"] = label->GetHorizontalOverflowScroll();
						row["word_width"] = label->GetMaxWordWidth();
						row["row_width"] = row["rect"][2];
					}
					// The combo's open list is a panel, not a control, so its state rides its owner's row.
					if (auto* combo = dynamic_cast<GUIComboBox*>(item)) {
						row["dropped"] = combo->IsDropped();
						row["item_count"] = combo->GetCount();
						Json items = Json::array();
						GUIListPanel* list = combo->GetListPanel();
						GUIFont* listFont = list ? list->GetFont() : nullptr;
						int longest = 0;
						for (int i = 0; i < combo->GetCount(); ++i) {
							const GUIListPanel::Item* entry = combo->GetItem(i);
							if (!entry) continue;
							const int nameRoom = std::max(1, list ? list->RegularItemNameRoom(entry->m_OffsetX) : combo->GetWidth() - 8);
							const std::string display = list ? list->RegularFittedName(entry->m_Name, entry->m_OffsetX) : entry->m_Name;
							const int rawWidth = listFont ? listFont->CalculateWidth(entry->m_Name) : 0;
							const int drawnWidth = listFont ? listFont->CalculateWidth(display) : 0;
							longest = std::max(longest, rawWidth);
							items.push_back({{"text", entry->m_Name}, {"display", display}, {"text_fits", drawnWidth <= nameRoom},
								{"name_room", nameRoom}, {"drawn_width", drawnWidth}, {"raw_width", rawWidth}});
						}
						row["items"] = items;
						row["selected_index"] = combo->GetSelectedIndex();
						constexpr int namePad = 8;
						constexpr int scrollThickness = 17;
						constexpr int panelPad = 12;
						const int stackHeight = list ? list->GetStackHeight() : 0;
						const int scroll = stackHeight > combo->GetDropHeight() ? scrollThickness : 0;
						const int needed = longest > 0 ? longest + namePad + scroll : combo->GetWidth();
						GUIPanel* parentPanel = panel->GetParentPanel();
						const int valueX = parentPanel ? combo->GetRelXPos() : 0;
						const int clamp = std::max(80, (parentPanel ? parentPanel->GetWidth() : combo->GetWidth()) - valueX - panelPad);
						row["fit_longest"] = longest;
						row["fit_needed"] = needed;
						row["fit_clamp"] = clamp;
						row["fit_width"] = combo->GetWidth();
					}
					// Measure every drawn caption here so a layout review reads the whole page, not the named controls.
					std::string measured;
					if (!text.empty()) { row["text_fits"] = TextFits(manager, item, measured); row["text_measure"] = measured; }
					result["controls"].push_back(row);
				}
				std::ofstream output(path.string() + ".json");
				output << result.dump(2) << '\n';
				output.flush();
				observation = result.dump();
				return output.good() && g_FrameMan.SaveBitmapToPNG(g_FrameMan.GetBackBuffer32(), (path.string() + ".png").c_str()) == 0;
			}
			observation = name + " " + argument;
			if (!control) { observation += " missing control"; return false; }
			if (command == "assert_visible") {
				observation += " actual=" + std::to_string(Visible(control));
				return (argument == "0" || argument == "1") && Visible(control) == (argument == "1");
			}
			if (command == "assert_focus") {
				observation += " actual=" + std::to_string(Visible(control) && control->GetPanel()->HasFocus());
				return argument.empty() && Visible(control) && control->GetPanel()->HasFocus();
			}
			if (command == "set_text") {
				// The typed-entry seam for a settings page: the box takes the value and raises the notification a typed entry raises.
				auto* box = dynamic_cast<GUITextBox*>(control);
				if (!box || !Enabled(box) || argument.empty()) return false;
				box->SetText(argument);
				// The event queue clears at the top of every Update, so the Enter has to be raised
				// inside one - the same channel a scripted click takes.
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				return input && input->QueueAutomationCommand([box] { box->AddEvent(GUIEvent::Notification, GUITextBox::Enter, 0); });
			}
			if (command == "assert_text_fits") return argument.empty() && TextFits(manager, control, observation);
			if (command == "assert_rect_inside") {
				auto* parent = argument == "parent" ? control->GetPanel()->GetParentPanel() : manager->GetControl(argument) ? manager->GetControl(argument)->GetPanel() : nullptr;
				observation += " rect=" + Json(Rectangle(control->GetPanel())).dump() + " bounds=" + Json(Rectangle(parent)).dump();
				return (parent || argument == "viewport") && Inside(Rectangle(control->GetPanel()), Rectangle(parent));
			}
			return false;
		} catch (const std::exception& error) { observation = error.what(); return false; }
	}
}
