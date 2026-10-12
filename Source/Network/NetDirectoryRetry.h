#pragma once

#include "System.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace RTE {

	struct NetDirectoryRetry {
		uint64_t nextMs = 0;
		uint64_t backoffMs = 0;

		static uint64_t NowMs() {
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		static uint64_t After(uint64_t nowMs, uint64_t delayMs) {
			return nowMs + std::min(delayMs, UINT64_MAX - nowMs);
		}
		static uint64_t RetryAfterMs(const std::string& body, const std::string& header) {
			uint64_t wait = 0;
			const auto milliseconds = [](double seconds) -> uint64_t {
				if (!std::isfinite(seconds) || seconds <= 0) return 0;
				return static_cast<uint64_t>(std::min(std::ceil(seconds * 1000), static_cast<double>(UINT64_MAX / 2)));
			};
			const auto value = nlohmann::json::parse(body, nullptr, false);
			if (value.is_object()) {
				const auto found = value.find("retry_after_s");
				if (found != value.end() && found->is_number()) wait = milliseconds(found->get<double>());
			}
			if (!header.empty()) {
				std::istringstream numeric(header); double seconds = 0;
				if ((numeric >> seconds) && (numeric >> std::ws).eof()) wait = std::max(wait, milliseconds(seconds));
				else {
					std::tm date{}; std::istringstream text(header); text.imbue(std::locale::classic());
					if (text >> std::get_time(&date, "%a, %d %b %Y %H:%M:%S GMT")) {
#ifdef _WIN32
						const auto until = _mkgmtime(&date);
#else
						const auto until = timegm(&date);
#endif
						const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
						if (until > 0 && until <= INT64_MAX / 1000 && until * 1000 > wall) wait = std::max(wait, static_cast<uint64_t>(until * 1000 - wall));
					}
				}
			}
			return wait;
		}
		static bool SessionGone(long status, const std::string& body) {
			const auto value = nlohmann::json::parse(body, nullptr, false);
			return (status == 404 || status == 410) && value.is_object() && value.contains("error") &&
			    (value["error"] == "not_found" || value["error"] == "match_ended");
		}
		static std::string Reason(long status, const std::string& body) {
			if (status == 429) {
				const auto value = nlohmann::json::parse(body, nullptr, false);
				if (value.is_object() && value.contains("error") && value["error"].is_string()) {
					const std::string code = value["error"];
					if (code == "rate_limited_key" || code == "rate_limited_ip" || code == "connection_busy") return code;
				}
				return "directory_busy";
			}
			return status == 0 ? "connection_wait" : status == 200 ? "reply_wait" : status >= 500 ? "service_wait" : "HTTP" + std::to_string(status);
		}
		static void Note(uint64_t nowMs, const std::string& path, const std::string& reason, uint64_t retryMs) {
			struct Count { uint64_t atMs = 0, pending = 0; bool printed = false; };
			static std::mutex mutex; static std::map<std::string, Count> counts;
			std::lock_guard lock(mutex); auto& count = counts[reason]; ++count.pending;
			if (!count.printed || nowMs < count.atMs || nowMs - count.atMs >= 10000) {
				System::PrintDiagnosticLine("[net-directory-wait] path=" + path + " class=" + reason + " count=" + std::to_string(count.pending) + " retry_ms=" + std::to_string(retryMs));
				count.atMs = nowMs; count.pending = 0; count.printed = true;
			}
		}
		uint64_t Failed(uint64_t nowMs, long status, const std::string& body, const std::string& header, const std::string& path) {
			uint64_t delay = RetryAfterMs(body, header);
			if (status == 429) delay = std::max<uint64_t>(1000, delay);
			else { backoffMs = backoffMs == 0 ? 1000 : std::min<uint64_t>(backoffMs * 2, 30000); delay = std::max(delay, backoffMs); }
			nextMs = std::max(nextMs, After(nowMs, delay));
			Note(nowMs, path, Reason(status, body), nextMs - nowMs);
			return nextMs;
		}
		void Succeeded() { backoffMs = 0; }
	};

	/// Shared directory waits survive a new operation, route or signal channel.
	class NetDirectoryBudget {
		struct Wait {
			uint64_t startMs = 0, untilMs = 0, completedMs = 0;
			void Extend(uint64_t nowMs, uint64_t until) {
				if (nowMs >= untilMs) { completedMs += untilMs - startMs; startMs = nowMs; untilMs = nowMs; }
				untilMs = std::max(untilMs, until);
			}
			uint64_t Spent(uint64_t nowMs) const { return completedMs + (nowMs >= startMs ? std::min(nowMs, untilMs) - startMs : 0); }
		};
		struct Row {
			uint64_t clockMs = 0;
			std::map<std::string, Wait> waits;
			Wait admissionWait;
			bool pollFirst = false;
			std::map<std::string, bool> ended;
		};
		inline static std::mutex s_Mutex;
		inline static std::map<std::pair<std::string, std::string>, Row> s_Rows;
		static Row& Find(const std::string& url, const std::string& install, uint64_t nowMs) {
			auto& row = s_Rows[{url, install}];
			row.clockMs = std::max(row.clockMs, nowMs);
			return row;
		}
	public:
		static void Refused(const std::string& url, const std::string& install, const std::string& defaultBudget,
		                    uint64_t nowMs, uint64_t untilMs, const std::string& body, bool hostSignal = false) {
			std::string budget = defaultBudget;
			const auto value = nlohmann::json::parse(body, nullptr, false);
			if (value.is_object() && value.contains("budget") && value["budget"].is_string()) {
				const std::string named = value["budget"];
				if (named == "general" || named == "heartbeat" || named == "admission" || named == "relay") budget = named;
			}
			std::lock_guard lock(s_Mutex); auto& row = Find(url, install, nowMs);
			row.waits[budget].Extend(nowMs, untilMs);
			if (budget == "general" || budget == "admission") row.admissionWait.Extend(nowMs, untilMs);
			if (hostSignal) row.pollFirst = true;
		}
		static bool Ready(const std::string& url, const std::string& install, const std::string& budget, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); auto& row = Find(url, install, nowMs);
			if (nowMs < row.waits[budget].untilMs) return false;
			if (budget == "admission" && row.pollFirst && nowMs < row.waits["general"].untilMs) return false;
			if (budget == "heartbeat" && row.pollFirst && nowMs >= row.admissionWait.untilMs) return false;
			if (budget == "general" && row.pollFirst) return false;
			return true;
		}
		static void PollAnswered(const std::string& url, const std::string& install, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); Find(url, install, nowMs).pollFirst = false;
		}
		static uint64_t AdmissionWaitedMs(const std::string& url, const std::string& install, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); return Find(url, install, nowMs).admissionWait.Spent(nowMs);
		}
		static uint64_t AdmissionUntilMs(const std::string& url, const std::string& install, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); return Find(url, install, nowMs).admissionWait.untilMs;
		}
		static void EndSession(const std::string& url, const std::string& install, const std::string& session, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); auto& row = Find(url, install, nowMs);
			if (row.ended.size() >= 32) row.ended.erase(row.ended.begin());
			row.ended.try_emplace(session, false);
		}
		static void SessionOpened(const std::string& url, const std::string& install, const std::string& session, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); Find(url, install, nowMs).ended.erase(session);
		}
		static bool TakeEndedNotice(const std::string& url, const std::string& install, const std::string& session, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); auto& ended = Find(url, install, nowMs).ended;
			const auto found = ended.find(session);
			if (found == ended.end() || found->second) return false;
			found->second = true; return true;
		}
		static bool SessionEnded(const std::string& url, const std::string& install, const std::string& session, uint64_t nowMs) {
			std::lock_guard lock(s_Mutex); return Find(url, install, nowMs).ended.contains(session);
		}
	};
}
