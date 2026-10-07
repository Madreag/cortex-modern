#pragma once

#include "NetProtocol.h"
#include "SettingsMan.h"

#include <functional>
#include <string>
#include <vector>

namespace RTE {
	inline uint8_t NetChatAudience(SettingsMan::NetworkChatDefaultScope saved, bool control, bool shift) {
		if (control) return c_NetChatScopeTeam;
		if (shift) return c_NetChatScopeAll;
		return saved == SettingsMan::NetworkChatDefaultScope::Team ? c_NetChatScopeTeam : c_NetChatScopeAll;
	}

	inline unsigned NetChatRosterPeer(uint8_t sessionPeer) { return static_cast<unsigned>(sessionPeer) + 1; }

	/// The held-seat screen's Escape must first belong to its active text entry or Players panel.
	inline bool NetHeldWaitShouldLeave(bool heldWait, bool panelOpen, bool chatOpen, bool escapePressed) {
		return heldWait && !panelOpen && !chatOpen && escapePressed;
	}

	struct NetChatArrivalAlert { bool notify; bool sound; };
	/// Only fresh remote arrivals alert; opening history and receiving an own echo do not.
	inline NetChatArrivalAlert NetChatAlertFor(bool known, bool initialized, bool local, bool notifyEnabled, bool soundEnabled) {
		const bool arriving = !known && initialized && !local;
		return {arriving && notifyEnabled, arriving && soundEnabled};
	}

	/// Hidden history reveals the arrival that triggered the notice, even when an own echo follows it.
	inline bool NetChatLineVisible(bool historyVisible, bool entryOpen, bool notifying, uint64_t lineId, uint64_t arrivalId) {
		return historyVisible || entryOpen || (notifying && lineId == arrivalId);
	}

	/// Wrap every character, including long unbroken words, at UTF-8 boundaries. Chat never ellipsizes its payload.
	inline std::vector<std::string> NetChatWrap(const std::string& text, int width, const std::function<int(const std::string&)>& measure) {
		std::vector<std::string> lines;
		size_t start = 0;
		while (start < text.size()) {
			size_t end = start, space = std::string::npos;
			while (end < text.size() && text[end] != '\n') {
				size_t next = end + 1;
				while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
				if (end > start && measure(text.substr(start, next - start)) > width) break;
				if (text[end] == ' ') space = end;
				end = next;
			}
			if (end < text.size() && text[end] != '\n' && space != std::string::npos && space > start) end = space;
			lines.push_back(text.substr(start, end - start));
			start = end;
			if (start < text.size() && (text[start] == ' ' || text[start] == '\n')) ++start;
		}
		return lines;
	}
}
