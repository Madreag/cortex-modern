#include "NetSessionPlaneSelfTest.h"

#include "NetLockstep.h"
#include "NetPeerFrameHelpers.h"
#include "NetLockstepSelfTest.h"
#include "NetMatchService.h"
#include "LoopbackTransport.h"
#if __has_include("NetPeerSessionWire.h")
#include "NetPeerSessionWire.h"
#endif
#include "DiagnosticLine.h"
#include "NetDirectoryRetry.h"
#include "NetIdentity.h"
#include "NetLobbySession.h"
#include "NetMuxTransport.h"
#include "NetProtocol.h"
#include "TimerMan.h"
#include "ScenarioRunner.h"
#include "SteadyClock.h"
#include "ActivityMan.h"
#include "GameActivity.h"
#include "MovableMan.h"
#include "lz4.h"
#include "Actor.h"
#include "Controller.h"
#include "MovableObject.h"
#include "NetActorOwnership.h"
#include "AudioMan.h"
#include "LuaMan.h"
#include "SettingsMan.h"
#include "SimChecksum.h"
#include "SceneMan.h"
#include "allegro.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <streambuf>
#include <string_view>
#include <type_traits>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace RTE {
	namespace {
		constexpr double c_TestTickMs = 1000.0 / 60;
		constexpr uint64_t c_TestReturnAllowanceMs = 2000;
		constexpr uint32_t c_TestBoundTicks = 3, c_TestCatchUpRate = 3;
		/// The longest a tie exchange waits for its answer, as NetConnectionAuthority's c_TieTimeoutCapMs.
		constexpr uint64_t c_TestTieTimeoutCapMs = 2000;
		using HoldBoundaries = std::map<uint8_t, std::map<uint64_t, bool>>;
		// Levers read once: every getenv takes the C runtime's environment lock and scans the environment, and the wire asks per message.
		bool DirectFaultObservations() {
			static const bool enabled = std::getenv("CC_TEST_DIRECT_FAULT_OBSERVATIONS") != nullptr;
			return enabled;
		}
		const char* ObservedTargetText() {
			static const std::optional<std::string> text = [] {
				const char* value = std::getenv("CC_TEST_LOCKSTEP_OBSERVE_TARGET");
				return value ? std::optional<std::string>(value) : std::nullopt;
			}();
			return text ? text->c_str() : nullptr;
		}
		// Installed state, read by the self-test's friend accessors. The fixture never supplies a voting group to a peer.
		std::function<uint32_t(const NetLockstepCoordinator&, uint64_t)> s_FrameMembers, s_HeldSeats;
		std::function<uint8_t(const NetLockstepCoordinator&, const NetLockstepReadyFrame&)> s_FrameAuthor;
		std::function<const HoldBoundaries&(const NetLockstepCoordinator&)> s_HoldBoundaries;
		std::function<bool(const NetLockstepCoordinator&, uint64_t)> s_OwnHoldPending; //!< Whether a hold of the peer's own seat from this frame on waits to be taken.
		std::function<std::string(const NetLockstepCoordinator&)> s_WaitState;
		std::function<NetPeerId(const NetLockstepCoordinator&, uint8_t)> s_ReturnTransport;
		std::function<std::pair<INetTransport*, NetPeerId>(const NetLockstepCoordinator&, uint8_t)> s_ReturnRoute;
		std::once_flag s_FixtureObservers;
		uint8_t WitnessedFrameAuthor(uint64_t frame, uint32_t members, const NetHash32& digest, const NetHash32& prepared,
		    const std::map<uint8_t, NetHostMigrationMessage>& witnesses) {
			for (const auto& [sender, witness]: witnesses)
				if (sender > 0 && sender <= NetMatchConfigUtil::c_MaxPeerCount && (members & (1U << (sender - 1))) &&
				    witness.type == NetHostMigrationMessageType::PeerReceipt && witness.senderPeerId == sender && witness.successorPeerId == sender &&
				    witness.frame == frame && witness.appliedFrame == frame && witness.preparedFrame == UINT64_MAX && witness.voterMask == members &&
				    witness.bytes.size() == digest.size() + prepared.size() && std::equal(digest.begin(), digest.end(), witness.bytes.begin()) &&
				    std::equal(prepared.begin(), prepared.end(), witness.bytes.begin() + digest.size())) return sender;
			return 0;
		}
		struct Fixture;
		struct ImageReturnTask {
			struct promise_type;
			using Handle = std::coroutine_handle<promise_type>;
			struct promise_type {
				bool succeeded = false;
				std::exception_ptr exception;
				ImageReturnTask get_return_object() { return ImageReturnTask(Handle::from_promise(*this)); }
				std::suspend_always initial_suspend() noexcept { return {}; }
				std::suspend_always final_suspend() noexcept { return {}; }
				void return_value(bool value) { succeeded = value; }
				void unhandled_exception() { exception = std::current_exception(); }
			};
			Handle handle{};
			ImageReturnTask() = default;
			explicit ImageReturnTask(Handle value): handle(value) {}
			ImageReturnTask(const ImageReturnTask&) = delete;
			ImageReturnTask& operator=(const ImageReturnTask&) = delete;
			ImageReturnTask(ImageReturnTask&& other) noexcept: handle(std::exchange(other.handle, {})) {}
			ImageReturnTask& operator=(ImageReturnTask&& other) noexcept {
				if (this == &other) return *this;
				if (handle) handle.destroy();
				handle = std::exchange(other.handle, {}); return *this;
			}
			~ImageReturnTask() { if (handle) handle.destroy(); }
			bool Done() const { return !handle || handle.done(); }
			bool Resume() {
				if (!Done()) handle.resume();
				if (handle && handle.promise().exception) std::rethrow_exception(handle.promise().exception);
				return !Done() || !handle || handle.promise().succeeded;
			}
		};
		ImageReturnTask ReadImageReturn(Fixture& fixture, uint8_t seat, uint8_t donor, std::string& error);
		bool ReturnThroughImage(Fixture& fixture, uint8_t seat, uint8_t donor, std::string& error);
		struct Case {
			const char* name;
			uint8_t subject = 2, seats = 4;
			uint64_t gapMs = 0;
			bool frozen = false, partition = false, tie = false, internet = false, directoryDown = false, directoryBoth = false, blackout = false;
			uint32_t rttMs = 2, otherRttMs = 2, lossPercent = 0;
			uint64_t steadyMs = 0;
			bool measurePace = false;
			bool captureFrames = false, realFrozen = false, hung = false;
			uint32_t checkpointEveryFrames = 0;
			// These are the agreed match mode and roster stage, shared by every seat.
			uint8_t mode = 0, rosterStage = 0; // mode: pause, dedicated, persistent; stage: starting, placement.
			bool adminDirectory = false, adminDirectoryDown = false, repeatedAdministration = false;
			bool tieBeforeAdministration = false;
			unsigned asymmetricBridge = 0;
			uint32_t planeIntervalMs = 1, recoveryRttMs = 0, stepMs = 1;
			bool delayStartup = false;
			bool receiptGap = false;
			uint64_t postReturnMs = 0;
			bool laterSessionMessageFirst = false;
			// The joiner's inputs stop reaching the host this long before the host goes dark, as inputs in flight do over a relay.
			uint64_t joinerInputsLostFirstMs = 0;
			// Both halves install their own holds at the bound. A delivered referee answer selects only which half must image-return.
			bool splitContract = false;
			bool oneWaySubject = false, directMigration = false;
			// The absent seat is gone past the donor's history and hands itself to a private image.
			bool imageReturn = false;
			// No proposal or certificate of the subject's own return reaches it: it learns the return from the committed tail alone.
			bool returnByTailOnly = false;
			// The subject's machine simulates at this share of the round's rate through the fault, its catch-up included, its link up (0: off).
			uint32_t slowPercent = 0;
			// A second seat drops this long after the subject and stays away until the fault ends (0: none).
			uint8_t secondSubject = 0;
			uint64_t secondAfterMs = 0;
			// This seat's receipts reach the others this late through the fault, its inputs on time: a member slow to hear, not to play.
			uint8_t lateReceiptsFrom = 0;
			uint64_t lateReceiptsMs = 0;
			// Once the host has the subject back, its machine runs at this share of the round's rate for refaultMs from refaultAfterMs later,
			// while it still catches up: a second hold in a new split epoch that reaches it ahead of its own frames (0: none).
			uint32_t refaultPercent = 0;
			uint64_t refaultAfterMs = 0, refaultMs = 0;
			// Once its slow spell ends, the subject catches up at only this share of the round's rate (0: the fixture's usual catch-up).
			uint32_t catchUpPercent = 0;
			// After this long past its fault, the machine has only the round's rate even while it is still a private reader.
			uint64_t roundRateAfterMs = 0;
			// The slow subject is held once through the fault and stays held; no peer sets a decision aside for authority it will have
			// or a return for a hold it has not installed; the playing seats keep the bound, a two-seat internet round included.
			bool heldOnce = false, noRefusals = false, strictBound = false;
			// This seat goes silent for silentVoterMs from the moment the leader first votes the subject's return: it never votes that return (0: none).
			uint8_t silentVoter = 0;
			uint64_t silentVoterMs = 0;
			// The subject's own inputs stop reaching the others through the fault while its link stays up - receipts, heartbeats, tails.
			bool inputsLost = false;
			// The two halves still exchange control, but their inputs and the subject's committed tail arrive only after its directory returns.
			bool inputHalvesLost = false, tailWaitForDirectory = false;
			// The subject returns within this long after the fault, then stays returned for twenty seconds (0: the usual allowance).
			uint64_t returnWithinMs = 0;
			// One directory request's HTTP exchange, from its send to its answer (0: 100 ms).
			uint32_t directoryExchangeMs = 0;
			// The subject's directory requests fail through its fault though its link stays up: its directory alone is out of reach.
			bool subjectDirectoryDark = false;
			bool secondHalfDirectoryDark = false;
			// The subject's link to seat 3 has this round trip (0: the subject's own), and seat 3 must be one that sends it the tail.
			uint32_t thirdSubjectRttMs = 0;
			uint8_t donorSeat = 0;
			// Through the fault only every keepInputEvery-th input message of the subject reaches each seat (0: all of them).
			uint32_t keepInputEvery = 0;
		};
		// The authority ruling's asynchronous split referee: authenticated requests name every retained owner and the asking group.
		// The first group to check in keeps that split; later replies cannot change it. Holds never consult this fixture's record directly.
		// The existing frame-tie message carries the request, including unequal splits of three retained seats.
		struct Directory {
			struct Tie { std::vector<uint16_t> owners; uint16_t host = 0; std::vector<uint16_t> winner; };
			std::map<std::tuple<uint64_t, uint64_t, NetHash32, uint64_t>, Tie> ties;
			std::vector<uint16_t> retained;
			std::set<uint16_t> leases, removed;
			std::map<uint16_t, uint64_t> lastCheckInMs;
			using HostReport = std::pair<NetHostChangeRequest, uint64_t>;
			std::map<uint16_t, HostReport> hostReports, agreementReports;
			struct HostDecision { NetHostChangeRequest request; NetHostChangeReply reply; uint16_t oldHost; };
			std::map<uint64_t, HostDecision> hostHistory;
			NetHash32 configHash{};
			long tieStatus = 200, checkInStatus = 200;
			uint64_t retryAfterSeconds = 0;
			uint16_t host = 1;
			uint64_t generation = 0;
			bool persistentWorld = false;
			std::vector<std::string> refusals;
			// A lever: the directory refuses every check-in, as it refused the earlier ties of the matches a row stands for.
			bool refusing = false;
			struct Answer { long status = 0; bool decided = false; std::vector<uint16_t> members; uint64_t retryAfterSeconds = 0; };
			Answer CheckIn(uint16_t seat, uint64_t nowMs = 0) {
				if (checkInStatus != 200) return {checkInStatus, false, {}, retryAfterSeconds};
				if (leases.empty()) leases.insert(retained.begin(), retained.end());
				if (!leases.contains(seat) || removed.contains(seat)) return {403};
				lastCheckInMs[seat] = nowMs;
				return {200};
			}
			struct HostAnswer { long status = 0; NetHostChangeReply reply; uint64_t retryAfterSeconds = 0; };
			HostAnswer HostChange(uint16_t seat, const NetHostChangeRequest& request, uint64_t nowMs) {
				if (const auto auth = CheckIn(seat, nowMs); auth.status != 200) return {auth.status, {}, auth.retryAfterSeconds};
				if (persistentWorld || request.configHash != configHash || !request.roundId || request.preparedFrame < request.appliedFrame) return {409};
				const auto prior = hostHistory.find(request.generation);
				const auto sameRound = [&](const NetHostChangeRequest& report) {
					return report.generation == request.generation && report.roundId == request.roundId && report.configHash == request.configHash;
				};
				if (prior != hostHistory.end() && request.agreedHost == UINT16_MAX)
					return sameRound(prior->second.request) ? HostAnswer{200, prior->second.reply} : HostAnswer{409};
				if (generation != request.generation && !(request.agreedHost != UINT16_MAX && generation == request.generation + 1)) return {409};
				const uint16_t oldHost = prior == hostHistory.end() ? host : prior->second.oldHost;
				const uint64_t heard = lastCheckInMs[oldHost];
				NetHostChangeReply reply; reply.state = NetHostChangeReply::State::Waiting;
				if (generation == request.generation && (nowMs < heard || nowMs - heard < c_NetHostLossSilenceMs)) return {200, reply};
				std::vector<uint16_t> live;
				for (uint16_t peer: retained) if (peer != oldHost && !removed.contains(peer) && nowMs >= lastCheckInMs[peer] && nowMs - lastCheckInMs[peer] < c_NetHostLossSilenceMs) live.push_back(peer);
				if (live.empty() || !std::binary_search(live.begin(), live.end(), seat)) return {200, reply};
				if (request.agreedHost != UINT16_MAX) {
					if (request.agreedMembers != live || !std::binary_search(live.begin(), live.end(), request.agreedHost)) return {409};
					const auto sameAgreement = [&](const NetHostChangeRequest& report) {
						return sameRound(report) && report.agreedHost == request.agreedHost && report.agreedBoundary == request.agreedBoundary && report.agreedMembers == request.agreedMembers;
					};
					if (const auto earlier = agreementReports.find(seat); earlier != agreementReports.end() && earlier->second.first.generation == request.generation && !sameAgreement(earlier->second.first)) return {409};
					agreementReports[seat] = {request, nowMs};
					for (uint16_t peer: live) if (!agreementReports.contains(peer) || !sameAgreement(agreementReports.at(peer).first)) return {200, reply};
					reply.host = reply.donor = request.agreedHost; reply.boundary = request.agreedBoundary;
				} else {
					hostReports[seat] = {request, nowMs};
					for (uint16_t peer: live) if (!hostReports.contains(peer) || !sameRound(hostReports.at(peer).first) || nowMs - hostReports.at(peer).second >= c_NetHostLossSilenceMs) return {200, reply};
					reply.host = reply.donor = live.front();
					for (uint16_t peer: live) if (hostReports.at(peer).first.preparedFrame > hostReports.at(reply.donor).first.preparedFrame) reply.donor = peer;
					reply.boundary = hostReports.at(reply.donor).first.preparedFrame;
				}
				reply.state = NetHostChangeReply::State::Decided; reply.generation = request.generation + 1; reply.members = live;
				hostHistory[request.generation] = {request, reply, oldHost};
				host = reply.host; generation = reply.generation; lastCheckInMs[host] = nowMs; hostReports.clear();
				return {200, reply};
			}
			Answer FrameTie(uint16_t seat, const NetFrameTieRequest& request, uint64_t nowMs = 0) {
				if (const auto auth = CheckIn(seat, nowMs); auth.status != 200) return auth;
				const auto refuse = [&](long status, const char* code) {
					refusals.push_back("seat " + std::to_string(seat) + " frame " + std::to_string(request.frame) + ": " + code);
					return Answer{status};
				};
				const auto sorted = [](const std::vector<uint16_t>& seats) { return std::is_sorted(seats.begin(), seats.end()) && std::adjacent_find(seats.begin(), seats.end()) == seats.end(); };
				const auto holds = [](const std::vector<uint16_t>& seats, uint16_t wanted) { return std::find(seats.begin(), seats.end(), wanted) != seats.end(); };
				if (request.owners.size() < 2 || request.owners.size() > 4 || !sorted(request.owners) || !sorted(request.members) ||
				    request.members.empty() || request.members.size() >= request.owners.size() ||
				    !std::includes(request.owners.begin(), request.owners.end(), request.members.begin(), request.members.end()) || !holds(request.owners, request.host) ||
				    !holds(request.owners, seat) || (!request.queryOnly && !holds(request.members, seat))) return refuse(400, "frame_tie_request");
				if (refusing) return refuse(409, "frame_tie_owners");
				if (tieStatus != 200) return {tieStatus, false, {}, retryAfterSeconds};
				// As the directory does, a seat the host removed is dropped from every check-in and every decision it discloses.
				const auto kept = [&](std::vector<uint16_t> seats) { std::erase_if(seats, [&](uint16_t seat) { return removed.contains(seat); }); return seats; };
				const auto owners = kept(request.owners), members = kept(request.members);
				const auto key = std::make_tuple(request.generation, request.roundId, request.configHash, request.frame);
				if (const auto prior = ties.find(key); prior != ties.end() && !prior->second.winner.empty()) {
					if (kept(prior->second.owners) != owners || prior->second.host != request.host) return refuse(409, "frame_tie_mismatch");
					return {200, true, kept(prior->second.winner)};
				}
				if (owners.size() < 2 || members.size() >= owners.size() || (!request.queryOnly && members.empty())) return refuse(400, "frame_tie_request");
				if (request.generation != generation || request.host != host || request.configHash != configHash) return refuse(409, "frame_tie_round");
				if (leases.empty()) leases.insert(retained.begin(), retained.end());
				for (uint16_t owner: owners) if (!leases.contains(owner)) return refuse(403, "frame_tie_seats");
				if (owners != retained) return refuse(409, "frame_tie_owners");
				if (request.queryOnly && !ties.contains(key)) return {200, false, {}};
				auto& tie = ties.try_emplace(key, Tie{owners, request.host, {}}).first->second;
				if (kept(tie.owners) != owners || tie.host != request.host) return refuse(409, "frame_tie_mismatch");
				if (tie.winner.empty() && !request.queryOnly) tie.winner = members;
				return {200, !tie.winner.empty(), kept(tie.winner)};
			}
		};
		// Each requester follows NetConnectionAuthority's asynchronous tie and routine check-in policies.
		struct DirectoryClient {
			struct Exchange { NetFrameTieRequest sent; uint64_t arriveMs = 0, doneMs = 0; bool arrived = false, delivered = false; Directory::Answer answer; };
			struct TieQuestion {
				NetFrameTieRequest request;
				NetFrameTieReply reply{NetFrameTieReply::State::Waiting};
				NetDirectoryRetry retry;
				uint64_t nextMs = 0;
				std::optional<uint64_t> boundStartMs;
				bool checkedIn = false, finalQuerySent = false;
			};
			std::vector<TieQuestion> questions;
			std::optional<size_t> activeTie;
			size_t nextQuestion = 0;
			std::optional<Exchange> tie, checkIn;
			struct HostExchange { NetHostChangeRequest sent; uint64_t arriveMs, doneMs; bool arrived, delivered; Directory::HostAnswer answer; };
			std::optional<HostExchange> hostChange;
			std::optional<NetHostChangeRequest> hostRequest;
			NetHostChangeReply hostReply;
			uint64_t nextHostMs = 0;
			NetDirectoryRetry hostRetry;
			std::vector<Exchange> retiredTies;
			struct Decision { NetFrameTieRequest request; NetFrameTieReply reply; uint64_t atMs = 0; };
			std::vector<Decision> consumed;
			uint64_t nextCheckInMs = 0, nextSeatRequestMs = 0;
			NetDirectoryRetry checkInRetry;
			std::deque<uint64_t> seatRequests, tieRequests;
			uint64_t sentMs = 0, exchangeEstimateMs = 0, generalUntilMs = 0;
			bool sentCheckIn = false;
			// The row's evidence of the product's answers: the questions asked, those answered Waiting, and those decided.
			uint64_t questionsAsked = 0, waitingAnswers = 0, decidedAnswers = 0, tieRequestCount = 0, firstQuestionMs = UINT64_MAX, firstDecidedMs = UINT64_MAX;
			// A seat checks in every 5.1 s from its lobby on, so its round starts with this minute's allowance spent up to `phaseMs` ago; a
			// rematch's round keeps the requests the seat has spent since.
			bool started = false;
			void StartRound(uint64_t nowMs, uint64_t phaseMs) {
				if (std::exchange(started, true)) return;
				for (int64_t at = static_cast<int64_t>(nowMs) - static_cast<int64_t>(phaseMs); at > static_cast<int64_t>(nowMs) - 60100; at -= 5100)
					if (at >= 0) seatRequests.push_front(static_cast<uint64_t>(at));
				nextCheckInMs = nowMs + 5100 - phaseMs; nextSeatRequestMs = nextCheckInMs;
			}
			bool CanSeatRequest(uint64_t nowMs, bool decisionRequest) {
				while (!seatRequests.empty() && nowMs >= seatRequests.front() && nowMs - seatRequests.front() >= 60100) seatRequests.pop_front();
				return nowMs >= generalUntilMs && seatRequests.size() < 12 && (decisionRequest || nowMs >= nextSeatRequestMs);
			}
			bool CanTieRequest(uint64_t nowMs) {
				while (!tieRequests.empty() && nowMs >= tieRequests.front() && nowMs - tieRequests.front() >= 60100) tieRequests.pop_front();
				return nowMs >= generalUntilMs && tieRequests.size() < 30;
			}
			size_t QuestionIndex(const NetFrameTieRequest& asked) {
				for (size_t index = 0; index < questions.size(); ++index) {
					auto identity = questions[index].request; identity.members = asked.members; identity.queryOnly = asked.queryOnly;
					if (identity == asked) return index;
				}
				questions.push_back(TieQuestion{asked});
				return questions.size() - 1;
			}
			TieQuestion& Question(const NetFrameTieRequest& asked) { return questions[QuestionIndex(asked)]; }
			std::optional<size_t> NextQuestion(uint64_t nowMs) {
				for (size_t offset = 0; offset < questions.size(); ++offset) {
					const size_t index = (nextQuestion + offset) % questions.size();
					const auto& question = questions[index];
					if (question.reply.state == NetFrameTieReply::State::Decided || nowMs < question.nextMs || nowMs < question.retry.nextMs) continue;
					nextQuestion = (index + 1) % questions.size();
					return index;
				}
				return {};
			}
			NetFrameTieReply Query(const NetFrameTieRequest& asked, uint64_t nowMs) {
				++questionsAsked; firstQuestionMs = std::min(firstQuestionMs, nowMs);
				if (!questions.empty() && (questions.front().request.roundId != asked.roundId || questions.front().request.configHash != asked.configHash)) {
					if (tie) {
						if (retiredTies.size() >= 16) return {};
						retiredTies.push_back(std::move(*tie)); tie.reset();
					}
					questions.clear(); activeTie.reset(); nextQuestion = 0;
				}
				const size_t index = QuestionIndex(asked);
				auto& question = questions[index];
				if (question.reply.state != NetFrameTieReply::State::Decided && asked.queryOnly != question.request.queryOnly) {
					if (activeTie == index && tie) {
						if (retiredTies.size() >= 16) return {};
						retiredTies.push_back(std::move(*tie)); tie.reset(); activeTie.reset();
					}
					question = TieQuestion{asked};
				}
				const auto& reply = question.reply;
				if (reply.state == NetFrameTieReply::State::Decided) {
					++decidedAnswers; firstDecidedMs = std::min(firstDecidedMs, nowMs);
					if (consumed.empty() || consumed.back().request != asked || consumed.back().reply.members != reply.members)
						consumed.push_back({asked, reply, nowMs});
				} else if (reply.state == NetFrameTieReply::State::Waiting) ++waitingAnswers;
				return reply;
			}

			NetHostChangeReply QueryHost(const NetHostChangeRequest& asked) {
				if (!hostRequest || *hostRequest != asked) {
					if (hostChange) return {NetHostChangeReply::State::Waiting};
					hostRequest = asked; nextHostMs = std::max(nextHostMs, hostRetry.nextMs);
					hostReply = {}; hostReply.state = NetHostChangeReply::State::Waiting;
				}
				return hostReply;
			}
			// `exchangeMs` is this seat's HTTP exchange now, `reachable` whether its request reaches the directory at all.
			void Poll(uint64_t nowMs, uint32_t exchangeMs, bool reachable, uint8_t seat, Directory& directory) {
				if (hostChange && !hostChange->arrived && nowMs >= hostChange->arriveMs) {
					hostChange->arrived = true;
					if (reachable) hostChange->answer = directory.HostChange(seat, hostChange->sent, nowMs);
					else hostChange->delivered = false;
				}
				if (hostChange && nowMs >= hostChange->doneMs) {
					const auto answer = hostChange->delivered ? hostChange->answer : Directory::HostAnswer{};
					hostChange.reset(); nextHostMs = nowMs + 1000; hostReply = {};
					if (answer.status == 200) { hostReply = answer.reply; hostRetry.Succeeded(); }
					else {
						nextHostMs = hostRetry.Failed(nowMs, answer.status, "", std::to_string(answer.retryAfterSeconds), "host-change");
						if (answer.status == 429) generalUntilMs = std::max(generalUntilMs, nextHostMs);
					}
				}
				if (hostRequest && !hostChange && hostReply.state != NetHostChangeReply::State::Decided && nowMs >= nextHostMs && nowMs >= hostRetry.nextMs && CanSeatRequest(nowMs, true)) {
					hostChange = HostExchange{*hostRequest, nowMs + exchangeMs / 2, nowMs + (reachable ? exchangeMs : 5000), false, reachable, {}};
					seatRequests.push_back(nowMs); nextSeatRequestMs = nowMs + 5100;
				}
				const auto frameTie = [&](const NetFrameTieRequest& sent) {
					const auto answer = directory.FrameTie(seat, sent, nowMs);
					if (NetPeerFrameDetail::ReturnInputDetails() && answer.status != 200) {
						uint32_t members = 0; for (uint16_t member: sent.members) if (member > 0 && member <= 4) members |= 1U << (member - 1);
						DiagnosticLine() << "[session-plane-directory-refusal] seat=" << unsigned(seat) << " frame=" << sent.frame
						    << " request_generation=" << sent.generation << " request_host=" << sent.host << " query_only=" << sent.queryOnly
						    << " members=" << members << " current_generation=" << directory.generation << " current_host=" << directory.host
						    << " status=" << answer.status << " now_ms=" << nowMs << std::endl;
					}
					return answer;
				};
				for (auto& retired: retiredTies) if (!retired.arrived && nowMs >= retired.arriveMs) {
					retired.arrived = true;
					if (reachable) retired.answer = frameTie(retired.sent);
				}
				std::erase_if(retiredTies, [nowMs](const auto& retired) { return nowMs >= retired.doneMs; });
				for (auto* exchange: {&tie, &checkIn}) if (*exchange && !(*exchange)->arrived && nowMs >= (*exchange)->arriveMs) {
					(*exchange)->arrived = true;
					// The directory acts on a request that reaches it even when the asker has stopped waiting for the answer.
					if (reachable) { (*exchange)->answer = exchange == &tie ? frameTie((*exchange)->sent) : directory.CheckIn(seat, nowMs); }
					else (*exchange)->delivered = false;
				}
				if (tie && nowMs >= tie->doneMs) {
					auto& question = questions.at(*activeTie);
					const auto answer = tie->delivered ? tie->answer : Directory::Answer{};
					tie.reset(); activeTie.reset(); question.nextMs = nowMs + 50; question.reply = {};
					const uint64_t exchanged = nowMs - sentMs;
					const bool valid = answer.status == 200 && (!answer.decided || (!answer.members.empty() && answer.members.size() < question.request.owners.size() &&
					    std::is_sorted(answer.members.begin(), answer.members.end()) && std::adjacent_find(answer.members.begin(), answer.members.end()) == answer.members.end() &&
					    std::includes(question.request.owners.begin(), question.request.owners.end(), answer.members.begin(), answer.members.end())));
					if (valid) {
						if (!question.checkedIn) question.boundStartMs = nowMs;
						question.checkedIn = true; question.reply.state = NetFrameTieReply::State::Waiting;
						if (answer.decided) { question.reply.state = NetFrameTieReply::State::Decided; question.reply.members = answer.members; }
						question.retry.Succeeded();
						exchangeEstimateMs = std::max(exchanged, exchangeEstimateMs - exchangeEstimateMs / 4);
						if (question.finalQuerySent) question.nextMs = nowMs + 250;
					} else if (answer.status == 0 && sentCheckIn) { exchangeEstimateMs = std::max(exchangeEstimateMs, exchanged); question.finalQuerySent = false; }
					else {
						question.nextMs = question.retry.Failed(nowMs, answer.status, "", std::to_string(answer.retryAfterSeconds), "frame-tie");
						if (answer.status == 429) generalUntilMs = std::max(generalUntilMs, question.nextMs);
						question.finalQuerySent = false;
					}
				}
				if (!tie && CanTieRequest(nowMs)) if (const auto selected = NextQuestion(nowMs)) {
					auto& question = questions[*selected];
					const bool finalQuery = question.boundStartMs && nowMs >= *question.boundStartMs + c_NetFrameTieDeadlineMs;
					auto sent = question.request; sent.queryOnly = sent.queryOnly || question.checkedIn || finalQuery;
					const uint64_t timeoutMs = std::clamp<uint64_t>(2 * exchangeEstimateMs, c_NetFrameTieHttpTimeoutMs, c_TestTieTimeoutCapMs);
					const bool answered = reachable && exchangeMs <= timeoutMs;
					tie = Exchange{sent, nowMs + exchangeMs / 2, nowMs + (answered ? exchangeMs : timeoutMs), false, answered, {}};
					activeTie = selected;
					question.finalQuerySent = question.finalQuerySent || finalQuery; ++tieRequestCount;
					tieRequests.push_back(nowMs); sentMs = nowMs; sentCheckIn = !sent.queryOnly;
				}

				if (checkIn) {
					if (nowMs < checkIn->doneMs) return;
					const auto answer = checkIn->delivered ? checkIn->answer : Directory::Answer{};
					const bool ok = answer.status == 200;
					checkIn.reset(); nextCheckInMs = nowMs + 5000;
					// The directory answers again: a tie request backing off from its silence asks at once.
					if (ok) { checkInRetry.Succeeded(); for (auto& question: questions) question.retry = {}; } else {
						nextCheckInMs = checkInRetry.Failed(nowMs, answer.status, "", std::to_string(answer.retryAfterSeconds), "seat-check-in");
						if (answer.status == 429) generalUntilMs = std::max(generalUntilMs, nextCheckInMs);
					}
				} else if (nowMs >= nextCheckInMs && nowMs >= checkInRetry.nextMs && CanSeatRequest(nowMs, false)) {
					// A directory out of reach answers a check-in only at the connection's own five-second timeout.
					checkIn = Exchange{{}, nowMs + exchangeMs / 2, nowMs + (reachable ? exchangeMs : 5000), false, reachable, {}};
					seatRequests.push_back(nowMs); nextSeatRequestMs = nowMs + 5100;
				}
			}
		};
		bool CheckDirectoryClient(std::string& error) {
			const auto fail = [&](const char* detail) { error = std::string("seed=0 ") + detail; return false; };
			Directory directory; directory.retained = {1, 2};
			NetFrameTieRequest request; request.owners = {1, 2}; request.members = {1}; request.host = 1; request.roundId = 1; request.frame = 100;
			DirectoryClient first, second;
			first.StartRound(60000, 1000); second.StartRound(60000, 1000);
			if (first.Query(request, 60000).state != NetFrameTieReply::State::Waiting) return fail("new tie skipped its HTTP exchange");
			first.Poll(60000, 100, true, 1, directory);
			first.Poll(60050, 100, true, 1, directory);
			if (first.Query(request, 60050).state != NetFrameTieReply::State::Waiting) return fail("outward request supplied an answer before its reply arrived");
			auto later = request; ++later.frame;
			if (first.Query(later, 60050).state != NetFrameTieReply::State::Waiting || (!first.tie || first.tie->sent != request)) return fail("new question replaced an in-flight request");
			first.Poll(60100, 100, true, 1, directory);
			if (first.Query(request, 60100).state != NetFrameTieReply::State::Decided || first.Question(request).reply.members != std::vector<uint16_t>{1}) return fail("completed exchange lost its decided half");
			auto opposite = request; opposite.members = {2};
			if (second.Query(opposite, 60100).state != NetFrameTieReply::State::Waiting) return fail("another requester received an answer without its own exchange");
			second.Poll(60100, 100, true, 2, directory); second.Poll(60150, 100, true, 2, directory); second.Poll(60200, 100, true, 2, directory);
			if (second.Query(opposite, 60200).state != NetFrameTieReply::State::Decided || second.Question(opposite).reply.members != first.Question(request).reply.members) return fail("second check-in rewrote a decided split");
			auto wrong = request; wrong.owners = {1, 3}; wrong.members = {1};
			if (directory.FrameTie(1, wrong).status != 409) return fail("decided split accepted different owners");
			wrong = request; wrong.members = {1, 2};
			if (directory.FrameTie(1, wrong).status != 400) return fail("a claim containing all owners was accepted as a split");
			Directory three; three.retained = {1, 2, 3};
			auto unequal = request; unequal.owners = three.retained; unequal.members = {2, 3};
			DirectoryClient minority, majority;
			majority.StartRound(60000, 1000); minority.StartRound(60000, 1000);
			majority.Query(unequal, 60000); majority.Poll(60000, 100, true, 2, three); majority.Poll(60050, 100, true, 2, three); majority.Poll(60100, 100, true, 2, three);
			if (majority.Query(unequal, 60100).members != unequal.members) return fail("the asynchronous referee could not arbitrate a real unequal split");
			unequal.members = {1}; minority.Query(unequal, 60100); minority.Poll(60100, 100, true, 1, three); minority.Poll(60150, 100, true, 1, three); minority.Poll(60200, 100, true, 1, three);
			if (minority.Query(unequal, 60200).members != std::vector<uint16_t>{2, 3}) return fail("the losing unequal group did not receive the immutable ruling");
			wrong = request; ++wrong.frame; wrong.owners = {1, 3};
			if (directory.FrameTie(1, wrong).status != 403) return fail("new split accepted an owner with no lease");
			DirectoryClient allowance; allowance.StartRound(60000, 1000);
			if (allowance.seatRequests.size() != 12 || allowance.CanSeatRequest(60000, false) || !allowance.CanTieRequest(60000)) return fail("routine check-ins spent the tie allowance");
			for (unsigned index = 0; index < 30; ++index) allowance.tieRequests.push_back(60000);
			if (allowance.CanTieRequest(120099) || !allowance.CanTieRequest(120100)) return fail("tie allowance did not retain its delivery margin");
			DirectoryClient timeout; timeout.StartRound(60000, 1000); timeout.Query(later, 60000);
			timeout.Poll(60000, 300, true, 1, directory); timeout.Poll(60150, 300, true, 1, directory); timeout.Poll(60250, 300, true, 1, directory);
			if (timeout.Question(later).retry.nextMs || timeout.exchangeEstimateMs != 250 || timeout.Question(later).nextMs != 60300) return fail("timed-out check-in entered failure backoff");
			timeout.Poll(60300, 300, true, 1, directory); timeout.Poll(60450, 300, true, 1, directory); timeout.Poll(60600, 300, true, 1, directory);
			if (timeout.Query(later, 60600).state != NetFrameTieReply::State::Decided || timeout.tieRequestCount != 2) return fail("adaptive timeout did not receive the retried check-in");
			DirectoryClient refused; refused.StartRound(60000, 1000); refused.Query(wrong, 60000);
			refused.Poll(60000, 100, true, 1, directory); refused.Poll(60050, 100, true, 1, directory); refused.Poll(60100, 100, true, 1, directory);
			if (refused.Question(wrong).reply.state == NetFrameTieReply::State::Decided || refused.Question(wrong).retry.nextMs != 61100) return fail("refused check-in supplied authority or skipped backoff");
			Directory administration; administration.retained = {1, 2, 3};
			administration.CheckIn(1, 20000); administration.CheckIn(3, 20000);
			NetHostChangeRequest hostRequest; hostRequest.roundId = 1; hostRequest.appliedFrame = 100; hostRequest.preparedFrame = 102;
			DirectoryClient admin; admin.nextCheckInMs = UINT64_MAX;
			if (admin.QueryHost(hostRequest).state != NetHostChangeReply::State::Waiting) return fail("host change skipped its asynchronous exchange");
			admin.Poll(21000, 100, true, 2, administration); admin.Poll(21050, 100, true, 2, administration);
			if (admin.QueryHost(hostRequest).state != NetHostChangeReply::State::Waiting || admin.seatRequests.size() != 1) return fail("host exchange answered early or bypassed the seat allowance");
			admin.Poll(21100, 100, true, 2, administration);
			if (admin.QueryHost(hostRequest).state != NetHostChangeReply::State::Waiting || administration.host != 1) return fail("peer silence overrode the directory's live host lease");
			administration.CheckIn(3, 35000);
			if (administration.HostChange(2, hostRequest, 35001).reply.state != NetHostChangeReply::State::Waiting) return fail("host expiry omitted a live survivor's prefix");
			auto advanced = hostRequest; advanced.preparedFrame = 104;
			const auto decision = administration.HostChange(3, advanced, 35002);
			if (decision.status != 200 || decision.reply.state != NetHostChangeReply::State::Decided || decision.reply.members != std::vector<uint16_t>{2, 3} ||
			    decision.reply.host != 2 || decision.reply.donor != 3 || decision.reply.boundary != 104 || decision.reply.generation != 1)
				return fail("host expiry lost a survivor, successor or greatest prepared prefix");
			if (administration.HostChange(2, hostRequest, 35003).reply.boundary != 104) return fail("later host report rewrote the decided boundary");
			DirectoryClient dark; dark.nextCheckInMs = UINT64_MAX; dark.QueryHost(hostRequest);
			dark.Poll(60000, 100, false, 2, administration); dark.Poll(64999, 100, false, 2, administration);
			if (dark.QueryHost(hostRequest).state != NetHostChangeReply::State::Waiting) return fail("unreachable host exchange failed before its timeout");
			dark.Poll(65000, 100, false, 2, administration);
			if (dark.QueryHost(hostRequest).state != NetHostChangeReply::State::Unavailable || dark.seatRequests.size() != 1 || dark.hostRetry.nextMs <= 65000)
				return fail("unreachable directory supplied a host decision or skipped retry policy");
			DirectoryClient exhausted; exhausted.StartRound(60000, 1000); exhausted.QueryHost(hostRequest);
			exhausted.Poll(60000, 100, true, 2, administration);
			if (exhausted.hostChange || exhausted.seatRequests.size() != 12) return fail("host change bypassed the shared twelve-request allowance");
			return true;
		}
		bool CheckDirectoryDelayedReply(std::string_view shape, std::string& error) {
			const auto fail = [&](const char* detail) { error = std::string("seed=0 ") + detail; return false; };
			Directory directory; directory.retained = {1, 2};
			NetFrameTieRequest request; request.owners = {1, 2}; request.members = {1}; request.host = 1; request.roundId = 1; request.frame = 100;
			if (shape == "final") {
				DirectoryClient observer, decider; observer.StartRound(60000, 1000); decider.StartRound(60000, 1000);
				auto query = request; query.queryOnly = true; observer.Query(query, 60000);
				for (uint64_t now = 60000; now <= 61500; now += 50) observer.Poll(now, 100, true, 1, directory);
				if (!observer.Question(query).finalQuerySent || observer.Question(query).reply.state == NetFrameTieReply::State::Decided) return fail("the missing split did not stay undecided through its final query");
				auto own = request; own.members = {2}; decider.Query(own, 61500);
				for (uint64_t now = 61500; now <= 62000; now += 50) { decider.Poll(now, 100, true, 2, directory); observer.Poll(now, 100, true, 1, directory); }
				if (observer.Query(query, 62000).state != NetFrameTieReply::State::Decided || observer.tieRequestCount < 9 || observer.Question(query).reply.members != std::vector<uint16_t>{2}) return fail("a deciding check-in after the final query was never learned");
				return true;
			}
			if (shape == "refusals") {
				auto wrong = request; wrong.configHash[0] = 1;
				if (directory.FrameTie(1, wrong).status != 409) return fail("an unpublished match hash supplied frame authority");
				wrong = request; ++wrong.generation;
				if (directory.FrameTie(1, wrong).status != 409) return fail("another host generation supplied frame authority");
				wrong = request; wrong.host = 2;
				if (directory.FrameTie(1, wrong).status != 409) return fail("another host supplied frame authority");
				directory.leases = {1, 2, 3, 4}; directory.retained = {1, 2, 3, 4};
				if (directory.FrameTie(1, request).status != 409) return fail("a check-in omitted retained owners");
				directory.retained = {1, 2};
				if (!directory.FrameTie(1, request).decided) return fail("the valid split was not recorded");
				directory.removed.insert(2); std::erase(directory.retained, 2);
				if (!directory.FrameTie(1, request).decided || directory.FrameTie(2, request).status != 403)
					return fail("old decisions bypassed the requester's current seat authentication");
				++request.frame;
				if (directory.FrameTie(1, request).status == 200) return fail("a removed owner supplied frame authority");
				return true;
			}
			DirectoryClient client; client.nextCheckInMs = 60000;
			if (shape == "healed-query") {
				client.Query(request, 60000); client.Poll(60000, 240, true, 1, directory);
				client.Question(request).retry.nextMs = 65000; client.Question(request).nextMs = 65000;
				auto query = request; query.queryOnly = true;
				if (client.Query(query, 60020).state != NetFrameTieReply::State::Waiting || client.tie || client.retiredTies.size() != 1 || client.Question(request).retry.nextMs)
					return fail("the recovered link waited for the old check-in or its backoff before querying");
				for (uint64_t now = 60020; now <= 60320; now += 10) client.Poll(now, 100, true, 1, directory);
				if (client.Query(query, 60320).state != NetFrameTieReply::State::Decided || !client.retiredTies.empty() || client.Question(request).reply.members != std::vector<uint16_t>{1})
					return fail("retiring the old exchange lost its recorded check-in or bypassed the query's round trip");
				query.members = {2};
				if (client.Query(query, 60320).state != NetFrameTieReply::State::Decided || client.tie)
					return fail("the same decided split was re-asked merely because the caller named the other half");
				return true;
			}
			if (shape == "waiting-question") {
				auto query = request; query.queryOnly = true;
				client.Query(query, 60000); client.Poll(60000, 100, true, 1, directory);
				auto later = request; ++later.frame;
				client.Query(later, 60050);
				for (uint64_t now = 60050; now <= 60400; now += 50) client.Poll(now, 100, true, 1, directory);
				if (client.Query(query, 60400).state != NetFrameTieReply::State::Waiting ||
				    client.Query(later, 60400).state != NetFrameTieReply::State::Decided ||
				    client.Question(query).boundStartMs != 60100 || directory.ties.size() != 1)
					return fail("an unrecorded split query starved a later check-in or lost its own wait clock");
				auto checkIn = request; checkIn.members = {2};
				if (!directory.FrameTie(2, checkIn).decided) return fail("the other half's legitimate check-in was refused");
				for (uint64_t now = 60450; now <= 61000; now += 50) client.Poll(now, 100, true, 1, directory);
				if (client.Query(query, 61000).members != std::vector<uint16_t>{2} || client.Query(later, 61000).members != std::vector<uint16_t>{1})
					return fail("a delayed ruling displaced the later split's different authenticated answer");
				return true;
			}
			if (shape == "two-questions") {
				client.Query(request, 60000); client.Poll(60000, 100, true, 1, directory);
				auto other = request; ++other.frame;
				if (client.Query(other, 60050).state != NetFrameTieReply::State::Waiting || (!client.tie || client.tie->sent != request))
					return fail("a second split replaced the exchange still in flight");
				for (uint64_t now: {60050ULL, 60100ULL}) client.Poll(now, 100, true, 1, directory);
				if (client.Query(request, 60100).state != NetFrameTieReply::State::Decided) return fail("the older split lost its completed answer");
				if (client.Query(other, 60100).state != NetFrameTieReply::State::Waiting) return fail("the new split inherited the old split's answer");
				for (uint64_t now: {60150ULL, 60200ULL, 60250ULL}) client.Poll(now, 100, true, 1, directory);
				if (client.Query(other, 60250).state != NetFrameTieReply::State::Decided) return fail("the second split did not complete its own exchange");
				// FLAGGED departure: each split retains its authenticated reply across later questions.
				if (client.Query(request, 60250).state != NetFrameTieReply::State::Decided || directory.ties.size() != 2 || client.tieRequestCount != 2 || client.tie)
					return fail("two split questions did not retain separate authenticated answers after their own exchanges");
				return true;
			}
			if (shape == "throttle") { directory.checkInStatus = 429; directory.retryAfterSeconds = 2; }
			else { directory.tieStatus = directory.checkInStatus = 503; client.nextCheckInMs = 65000; client.Query(request, 60000); }
			for (uint64_t now: {60000ULL, 60050ULL, 60100ULL}) client.Poll(now, 100, true, 1, directory);
			if (shape == "throttle") {
				if (client.CanTieRequest(62099) || client.CanSeatRequest(62099, true) || !client.CanTieRequest(62100)) return fail("a routine request's Retry-After did not gate both allowances");
				client.Query(request, 60200); client.Poll(62099, 100, true, 1, directory);
				if (client.tie) return fail("a tie bypassed its install's shared throttle");
				directory.checkInStatus = 200;
			} else {
				if (client.Question(request).reply.state == NetFrameTieReply::State::Decided || client.Question(request).retry.nextMs != 61100) return fail("a full directory record supplied authority or skipped its service backoff");
				directory.tieStatus = directory.checkInStatus = 200;
			}
			const uint64_t retry = shape == "throttle" ? 62100 : 61100;
			for (uint64_t now = retry; now <= retry + 100; now += 50) client.Poll(now, 100, true, 1, directory);
			if (client.Query(request, retry + 100).state != NetFrameTieReply::State::Decided) return fail("a recovered directory did not answer through a fresh exchange");
			return true;
		}
		// A round's schedule of trouble, the random driver's unit: which seat's machine slows or stops and when, each link's round trip
		// and jitter, loss bursts, how fast each machine catches up once it runs again, and the directory's exchange.
		struct Spell {
			// Slow: the machine runs at `percent` of the round's rate, its link up. Stopped: its main loop stops while its plane keeps the
			// link alive. Silent: every link of the seat drops while its machine runs on. Hung: the whole process stops - nothing sent,
			// what reaches it waits in its socket. Late: every packet of the seat's links takes `percent` ms longer each way - its inputs
			// keep arriving, past the bound. Stall: only its reliable lanes take `percent` ms longer - votes and certificates stall while
			// its inputs still flow.
			enum class Kind : uint8_t { Slow, Stopped, Silent, Hung, Late, Stall };
			uint8_t seat = 2;
			Kind kind = Kind::Slow;
			uint64_t atMs = 6000, ms = 2000;
			uint32_t percent = 80;
			bool directoryDark = false; //!< Silent: its whole connection goes dark, its directory requests with it.
			uint64_t EndMs() const { return atMs + ms; }
		};
		// The round's links split into two sides for a while: every link between them drops, each side's own links stay up.
		struct Cut {
			uint32_t side = 0; //!< The seats on one side, as a mask of their bits; the rest are the other side.
			uint64_t atMs = 6000, ms = 2000;
			uint32_t directoryDark = 0; //!< The seats whose directory requests fail through the cut.
			bool Separates(uint8_t a, uint8_t b) const { return ((side >> (a - 1)) & 1U) != ((side >> (b - 1)) & 1U); }
		};
		struct Burst {
			uint8_t seat = 2; //!< Every link of this seat loses `percent` of its packets: unreliable ones dropped, reliable ones resent late.
			uint64_t atMs = 6000, ms = 1000;
			uint32_t percent = 30;
		};
		struct Schedule {
			uint64_t seed = 0;
			uint8_t seats = 2;
			bool internet = true;
			std::array<std::array<uint32_t, 5>, 5> rttMs{}, jitterMs{}; //!< Per pair of seats, symmetric.
			std::array<uint32_t, 5> recoverPercent{300, 300, 300, 300, 300}; //!< Each machine's rate while it catches up after its spells.
			std::array<uint32_t, 5> allowancePhaseMs{}; //!< How long before the round each seat's last routine check-in went out.
			uint32_t directoryExchangeMs = 100;
			std::vector<Spell> spells;
			std::vector<Burst> bursts;
			std::vector<Cut> cuts;
			uint8_t leaver = 0; //!< This seat leaves the round at leaveAtMs, its seat kept by the AI (0: none).
			uint64_t leaveAtMs = 0;
			uint8_t kicked = 0; //!< The host removes this seat at kickAtMs; the directory hears of it one exchange later (0: none).
			uint64_t kickAtMs = 0;
			uint64_t rematchAtMs = 0; //!< The host ends the round here and every seat starts the next one 300 ms later, on the same links (0: none).
			uint64_t endMs = 30000;
			bool Left(uint8_t seat, uint64_t now) const { return (leaver && seat == leaver && now >= leaveAtMs) || Removed(seat, now); }
			bool Removed(uint8_t seat, uint64_t now) const { return kicked && seat == kicked && now >= kickAtMs; }
			bool CutBetween(uint8_t a, uint8_t b, uint64_t now) const {
				for (const auto& cut: cuts) if (now >= cut.atMs && now < cut.atMs + cut.ms && cut.Separates(a, b)) return true;
				return false;
			}
			uint32_t Rtt(uint8_t a, uint8_t b) const { return a == b ? 0 : rttMs[a][b]; }
			bool In(const Spell& spell, uint8_t seat, uint64_t now) const { return spell.seat == seat && now >= spell.atMs && now < spell.EndMs(); }
			const Spell* Active(uint8_t seat, uint64_t now) const {
				for (const auto& spell: spells) if (In(spell, seat, now)) return &spell;
				return nullptr;
			}
			bool Is(uint8_t seat, uint64_t now, Spell::Kind kind) const { const auto* spell = Active(seat, now); return spell && spell->kind == kind; }
			bool Severed(uint8_t a, uint8_t b, uint64_t now) const { return a != b && (Is(a, now, Spell::Kind::Silent) || Is(b, now, Spell::Kind::Silent) || CutBetween(a, b, now)); }
			bool DirectoryReachable(uint8_t seat, uint64_t now) const {
				for (const auto& cut: cuts) if (now >= cut.atMs && now < cut.atMs + cut.ms && ((cut.directoryDark >> (seat - 1)) & 1U)) return false;
				const auto* spell = Active(seat, now);
				return !spell || spell->kind == Spell::Kind::Slow || spell->kind == Spell::Kind::Stopped || spell->kind == Spell::Kind::Late || spell->kind == Spell::Kind::Stall ||
				    (spell->kind == Spell::Kind::Silent && !spell->directoryDark);
			}
			uint32_t LateMs(uint8_t a, uint8_t b, uint64_t now, bool reliable) const {
				uint32_t late = 0;
				for (uint8_t seat: {a, b})
					if (const auto* spell = Active(seat, now); spell && (spell->kind == Spell::Kind::Late || (reliable && spell->kind == Spell::Kind::Stall))) late = std::max(late, spell->percent);
				return late;
			}
			uint32_t LossPercent(uint8_t a, uint8_t b, uint64_t now) const {
				uint32_t loss = 0;
				for (const auto& burst: bursts) if ((burst.seat == a || burst.seat == b) && now >= burst.atMs && now < burst.atMs + burst.ms) loss = std::max(loss, burst.percent);
				return loss;
			}
			// The rate this machine catches up at: the fixture's usual three times the round's until its first spell, then its own.
			uint32_t CapacityPercent(uint8_t seat, uint64_t now) const {
				for (const auto& spell: spells) if (spell.seat == seat && now >= spell.atMs) return recoverPercent[seat];
				return 300;
			}
		};
		bool InputWindowContains(const NetHostMigrationMessage& packet, uint8_t owner, uint64_t frame) {
			if ((packet.type != NetHostMigrationMessageType::PeerInput && packet.type != NetHostMigrationMessageType::PeerForwardInput) ||
			    owner != (packet.type == NetHostMigrationMessageType::PeerInput ? packet.senderPeerId : packet.successorPeerId) ||
			    packet.offset != 0 || packet.totalBytes != packet.bytes.size() || packet.bytes.empty() || frame > packet.frame ||
			    packet.frame - frame >= NetLockstepCodec::c_PeerInputWindowTicks) return false;
			const auto take = [](const std::vector<uint8_t>& data, size_t& at, uint32_t& value) {
				if (at + 4 > data.size()) return false;
				value = 0; for (unsigned i = 0; i < 4; ++i) value |= uint32_t(data[at++]) << (8 * i); return true;
			};
			std::vector<uint8_t> body = packet.bytes;
			if (body[0] == 0) {
				size_t at = 1; uint32_t size = 0;
				if (!take(body, at, size) || size == 0 || size > NetLockstepCodec::c_InputWindowByteCap) return false;
				std::vector<uint8_t> expanded(size);
				if (LZ4_decompress_safe(reinterpret_cast<const char*>(body.data() + at), reinterpret_cast<char*>(expanded.data()), static_cast<int>(body.size() - at), static_cast<int>(size)) != static_cast<int>(size)) return false;
				body = std::move(expanded);
			}
			if (body[0] == 0 || body[0] > NetLockstepCodec::c_PeerInputWindowTicks) return false;
			size_t at = 1; uint64_t previous = 0; bool found = false;
			for (unsigned i = 0; i < body[0]; ++i) {
				uint32_t size = 0;
				if (!take(body, at, size) || size > body.size() - at) return false;
				NetLockstepFrame input;
				if (!NetLockstepCodec::DecodeRecoveryInput({body.begin() + at, body.begin() + at + size}, input) ||
				    input.senderPeerId != owner || input.roundId != packet.roundId || input.targetFrame > packet.frame || (i && input.targetFrame <= previous)) return false;
				at += size; previous = input.targetFrame; found |= input.targetFrame == frame;
			}
			return found && at == body.size() && previous == packet.frame;
		}
		struct Wire;
		struct Hub {
			std::recursive_mutex mutex;
			uint64_t now = 0, faultAt = 6000, faultEnd = 6000;
			Case row;
			std::map<uint16_t, Wire*> listeners;
			std::set<Wire*> wires;
			std::map<std::pair<uint8_t, uint8_t>, unsigned> dials;
			bool cleanRemoteClose = true;
			bool DuringFault() const { return now >= faultAt && now < faultEnd; }
			uint64_t refaultAt = UINT64_MAX, refaultEnd = 0;
			bool InRefault() const { return now >= refaultAt && now < refaultEnd; }
			uint64_t silentAt = UINT64_MAX;
			std::map<uint8_t, uint64_t> inputSentThrough;
			std::set<std::pair<uint8_t, uint64_t>> returnsProposed;
			std::map<uint8_t, uint64_t> tailsSentBy;
			std::map<std::pair<uint8_t, uint8_t>, uint64_t> inputSends;
			struct InputWireObservation { uint64_t at; uint8_t receiver; bool impaired; NetHostMigrationMessage packet; };
			std::multimap<uint64_t, InputWireObservation> inputEvidence;
			std::multimap<uint64_t, InputWireObservation> receiptEvidence;
			void ObserveInput(const NetHostMigrationMessage& packet, uint8_t receiver, bool impaired) {
				if ((packet.type == NetHostMigrationMessageType::PeerInput || packet.type == NetHostMigrationMessageType::PeerForwardInput) &&
				    packet.offset == 0 && packet.totalBytes == packet.bytes.size()) inputEvidence.emplace(packet.frame, InputWireObservation{now, receiver, impaired, packet});
				// A cumulative receipt: its sender accepted its successor's inputs before its frame. Witness receipts name history instead.
				if (packet.type == NetHostMigrationMessageType::PeerReceipt && packet.preparedFrame != UINT64_MAX)
					receiptEvidence.emplace(packet.frame, InputWireObservation{now, receiver, impaired, packet});
			}
			// The acceptor's receipt for the held seat's own input at this frame was impaired on its way to the held seat, and none that
			// covers the frame reached it before it displayed the frame: the held seat could not know its input was taken.
			const InputWireObservation* ReceiptPartition(uint8_t acceptor, uint8_t held, uint64_t frame, uint64_t heldAt) const {
				if (acceptor == held) return nullptr;
				const InputWireObservation* impaired = nullptr;
				// A receipt names the first frame its sender has not yet accepted: one covers this frame only past it.
				for (auto it = receiptEvidence.upper_bound(frame); it != receiptEvidence.end(); ++it) {
					const auto& observation = it->second;
					if (observation.receiver != held || observation.packet.senderPeerId != acceptor || observation.packet.successorPeerId != held || observation.at > heldAt) continue;
					// One delivered in the very millisecond the held side decided at its bound came too late to stop that hold.
					if (!observation.impaired && observation.at == heldAt) continue;
					if (!observation.impaired) return nullptr;
					if (!impaired || observation.at < impaired->at) impaired = &observation;
				}
				return impaired;
			}
			const InputWireObservation* InputPartition(uint8_t owner, uint8_t human, uint8_t held, uint64_t frame, uint64_t humanAt, uint64_t heldAt) const {
				if (owner == held) return nullptr;
				const InputWireObservation* impaired = nullptr;
				bool humanHas = false, heldHas = false;
				for (auto it = inputEvidence.lower_bound(frame); it != inputEvidence.end() && it->first - frame < NetLockstepCodec::c_PeerInputWindowTicks; ++it) {
					const auto& observation = it->second;
					if ((observation.receiver != human && observation.receiver != held) || !InputWindowContains(observation.packet, owner, frame)) continue;
					if (observation.impaired && observation.receiver == held && observation.at <= heldAt) {
						if (!impaired || observation.at < impaired->at) impaired = &observation;
						if (human == owner && observation.at <= humanAt) humanHas = true;
					}
					if (!observation.impaired) {
						humanHas |= observation.receiver == human && observation.at <= humanAt;
						heldHas |= observation.receiver == held && observation.at <= heldAt;
					}
				}
				return impaired && humanHas && !heldHas ? impaired : nullptr;
			}
			// The first copy of any seat's input for the frame, or of any acceptor's receipt covering it, that reached this side only after it
			// showed the frame: traffic it needed for the frame and did not have, which landed late and was never lost.
			std::string LateTraffic(uint8_t side, uint64_t frame, uint64_t shownAt) const {
				for (uint8_t owner = 1; owner <= row.seats; ++owner) {
					if (owner == side) continue;
					uint64_t first = UINT64_MAX;
					for (auto it = inputEvidence.lower_bound(frame); it != inputEvidence.end() && it->first - frame < NetLockstepCodec::c_PeerInputWindowTicks; ++it)
						if (!it->second.impaired && it->second.receiver == side && InputWindowContains(it->second.packet, owner, frame)) first = std::min(first, it->second.at);
					if (first != UINT64_MAX && first > shownAt) return "late_input_of=" + std::to_string(owner) + " delivered_ms=" + std::to_string(first) + " shown_ms=" + std::to_string(shownAt);
				}
				std::map<std::pair<uint8_t, uint8_t>, uint64_t> receipts;
				for (auto it = receiptEvidence.upper_bound(frame); it != receiptEvidence.end(); ++it) {
					const auto& observation = it->second;
					if (observation.impaired || observation.receiver != side || observation.packet.senderPeerId == side) continue;
					auto& first = receipts.try_emplace({observation.packet.senderPeerId, observation.packet.successorPeerId}, UINT64_MAX).first->second;
					first = std::min(first, observation.at);
				}
				for (const auto& [pair, first]: receipts) if (first > shownAt)
					return "late_receipt_of=" + std::to_string(pair.first) + "_for_" + std::to_string(pair.second) + " delivered_ms=" + std::to_string(first) + " shown_ms=" + std::to_string(shownAt);
				return {};
			}
			std::string returnInsideInputs;
			// The random driver's round: its schedule decides every link, rate and reachability below.
			const Schedule* schedule = nullptr;
			uint64_t random = 0x9E3779B97F4A7C15ULL;
			uint64_t Next() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; }
			bool ProcessHung(uint8_t seat) const {
				return schedule ? schedule->Is(seat, now, Spell::Kind::Hung) : row.hung && seat == row.subject && DuringFault();
			}
			// Physical fault evidence only: this does not choose a peer's members or its authority.
			bool LiveSplit(uint8_t a, uint8_t b) const {
				if (a == b || ProcessHung(a) || ProcessHung(b)) return false;
				if (schedule && (schedule->Left(a, now) || schedule->Left(b, now))) return false;
				return Split(a, b) || Split(b, a) || (!schedule && DuringFault() &&
				    ((row.blackout && (a == row.subject || b == row.subject)) || (row.inputHalvesLost && (a <= 2) != (b <= 2))));
			}
			bool Split(uint8_t a, uint8_t b) const {
				if (schedule) return schedule->Severed(a, b, now);
				if (row.tieBeforeAdministration && now >= 4000 && now < 4150) return (a <= 2) != (b <= 2);
				if (row.silentVoter && a != b && (a == row.silentVoter || b == row.silentVoter) && now >= silentAt && now < silentAt + row.silentVoterMs) return true;
				// Flagged fixture correction: Hung stops the process; incoming packets wait in its socket,
				// as Schedule::Cut already models. A network cut is a separate Silent/blackout fault.
				if (!DuringFault() || row.hung || row.frozen || row.blackout || row.slowPercent || row.inputsLost || row.inputHalvesLost || row.keepInputEvery || !row.gapMs) return false;
				if (row.secondSubject && now >= faultAt + row.secondAfterMs && (a == row.secondSubject || b == row.secondSubject)) return true;
				if (row.tie) return (a <= 2) != (b <= 2);
				if (row.oneWaySubject) return a == row.subject;
				return a == row.subject || b == row.subject;
			}
			uint64_t Delay(uint8_t a, uint8_t b) const {
				if (schedule) return (schedule->Rtt(a, b) + 1) / 2;
				uint32_t rtt = a == row.subject || b == row.subject ? row.rttMs : row.otherRttMs;
				if (row.thirdSubjectRttMs && (a == row.subject || b == row.subject) && (a == 3 || b == 3)) rtt = row.thirdSubjectRttMs;
				if (row.recoveryRttMs && now >= faultEnd && now < faultEnd + 8000 && (a == row.subject || b == row.subject)) rtt = row.recoveryRttMs;
				return (rtt + 1) / 2;
			}
			bool DirectoryReachable(uint8_t peer) const {
				if (schedule) return schedule->DirectoryReachable(peer, now);
				if (!DuringFault()) return true;
				if (row.directoryDown) return false;
				if (row.secondHalfDirectoryDark && peer >= 3) return false;
				// A subject whose link stays up through its fault - a slow or stopped machine, lost inputs - reaches the directory as well.
				const bool linkUp = row.frozen || row.slowPercent || row.inputsLost || row.inputHalvesLost || row.keepInputEvery || row.lateReceiptsFrom;
				return row.directoryBoth || peer != row.subject || (linkUp && !row.subjectDirectoryDark);
			}
			uint8_t ReturningSeat() const { return row.subject; }
			Directory directory;
			std::map<uint8_t, DirectoryClient> directoryClients;
			uint32_t DirectoryExchangeMs(uint8_t) const { return schedule ? schedule->directoryExchangeMs : row.directoryExchangeMs ? row.directoryExchangeMs : 100; }
			// Every seat's directory requests advance as its authority polls them, before its round's tick.
			void PollDirectory(uint8_t peer) {
				if (const auto client = directoryClients.find(peer); client != directoryClients.end())
					client->second.Poll(now, DirectoryExchangeMs(peer), DirectoryReachable(peer), peer, directory);
			}
			std::map<uint8_t, NetHostChangeRequest> adminReports;
			NetHostChangeReply adminDecision;
			std::map<uint64_t, NetHostChangeReply> adminHistory;
			std::map<uint64_t, std::map<uint8_t, NetHostChangeRequest>> adminReportsByGeneration;
			uint64_t tailMissingSent = 0, tailMissingFrame = 0;
			uint8_t tailMissingDonor = 0;
			uint64_t firstAdminRequestMs = UINT64_MAX;
			struct DelayedInput { Wire* target; NetTransportEvent event; bool input; };
			std::vector<DelayedInput> delayedInputs;
			uint64_t asymmetricFrame = 0;
			uint32_t asymmetricVoters = 0;
			uint32_t asymmetricDropped = 0, asymmetricResent = 0;
			uint32_t droppedReturns = 0;
			bool asymmetricReleased = false;
			// Signed wire decisions name the actual substituted frame range;
			// a prepared bridge may exist before the display reaches that range.
			std::map<uint8_t, std::map<uint64_t, bool>> bridgeBoundaries;
			std::map<uint64_t, NetHostMigrationMessage> returnCertificates;
			uint64_t receiptGapFrame = 0, receiptGapDrops = 0;
			uint64_t receiptGapAtMs = 0, receiptGapRequestDrops = 0;
			uint64_t tailQueuePeak = 0;
			uint32_t delayedChatSends = 0;
			// Times each of seat 1's input ticks crossed each lane, and the bytes it took.
			bool countInputs = false;
			uint8_t dropReceiptsFrom = 0, dropInputsFrom = 0;
			bool tieAnswersHeld = false;
			std::map<uint64_t, unsigned> reliableInputTicks, unreliableInputTicks;
			uint64_t reliableInputBytes = 0, unreliableInputBytes = 0;
		};
		struct Wire final : INetTransport {
			Hub& hub;
			uint8_t owner;
			uint16_t port = 0;
			uint32_t next = 1, sends = 0;
			std::map<NetPeerId, std::pair<Wire*, NetPeerId>> connections;
			std::map<std::pair<NetPeerId, NetTransportLane>, uint64_t> lastDelivery; //!< A reliable lane delivers in order, however its packets were delayed.
			struct LinkPing {
				std::multimap<uint64_t, uint32_t> pending;
				std::array<uint32_t, 3> recent{};
				uint32_t count = 0, smoothed = 0;
				uint64_t nextSampleAt = 0, random = 0;
				uint32_t Read(uint64_t now, uint32_t initial) {
					while (!pending.empty() && pending.begin()->first <= now) {
						const auto [at, sample] = *pending.begin(); pending.erase(pending.begin());
						// FLAGGED fixture departure: match the pinned GNS PingTracker's 100 ms buckets and two-lowest-of-three smoothing.
						if (count == 3 && at < nextSampleAt) { if (sample >= recent[0]) continue; }
						else { recent[2] = recent[1]; recent[1] = recent[0]; nextSampleAt = at + 100; }
						recent[0] = sample; count = std::min<uint32_t>(3, count + 1);
						if (count < 3) { nextSampleAt = 0; smoothed = count == 1 ? sample : std::min(recent[0], recent[1]); }
						else smoothed = (recent[0] + recent[1] + recent[2] - *std::max_element(recent.begin(), recent.end())) / 2;
					}
					return count ? smoothed : initial;
				}
			};
			mutable std::map<NetPeerId, LinkPing> linkPings;
			// A delivered packet keeps the message its send authenticated, so observing it on arrival reads that one.
			struct Event { uint64_t at; NetTransportEvent value; bool crossesFault = false; std::shared_ptr<const NetHostMigrationMessage> sent; };
			std::vector<Event> events;
			bool keepReturnCertificates = false;
			std::map<uint64_t, NetHostMigrationMessage> receivedReturnCertificates;
			std::map<std::pair<uint64_t, uint8_t>, NetLockstepTiming> receivedTimingReturns;
			bool keepImageTail = false;
			struct ImageTailDelivery { uint8_t donor; NetTransportLane lane; NetLobbyStateChunk chunk; };
			std::deque<ImageTailDelivery> receivedImageTail;
			bool TraceFaultObservation() const {
				return DirectFaultObservations() && owner == 2 && hub.now >= hub.faultAt && hub.now <= hub.faultAt + 100;
			}
			static std::string BytesHex(const std::vector<uint8_t>& bytes) {
				static constexpr char digits[] = "0123456789abcdef";
				std::string text; text.reserve(bytes.size() * 2);
				for (uint8_t byte: bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
				return text;
			}
			Wire(Hub& h, uint8_t seat): hub(h), owner(seat) { hub.wires.insert(this); }
			~Wire() override { Stop(); hub.wires.erase(this); }
			bool StartHost(uint16_t wanted, std::string* error = nullptr) override {
				if (hub.listeners.contains(wanted)) { if (error) *error = "the fixture port already has a listener"; return false; }
				port = wanted; hub.listeners[wanted] = this; return true;
			}
			bool Connect(const std::string&, uint16_t wanted, std::string* error = nullptr) override {
				const auto found = hub.listeners.find(wanted);
				if (found == hub.listeners.end()) { if (error) *error = "the fixture peer has no listener"; return false; }
				Wire* other = found->second;
				++hub.dials[{owner, other->owner}];
				const NetPeerId local = next++, remote = other->next++;
				connections[local] = {other, remote}; other->connections[remote] = {this, local};
				events.push_back({hub.now, {NetTransportEventType::PeerConnected, local, NetTransportLane::ControlReliable, {}, {}}});
				other->events.push_back({hub.now, {NetTransportEventType::PeerConnected, remote, NetTransportLane::ControlReliable, {}, {}}}); return true;
			}
			template<class Message> unsigned DelayAsymmetricInput(const Message& packet, Wire& other, NetPeerId remote, NetTransportLane lane, const std::vector<uint8_t>& bytes) {
				using Type = std::decay_t<decltype(packet.type)>;
				if constexpr (requires { Type::PeerInput; Type::PeerForwardInput; Type::PeerReceipt; Type::PeerBridge; }) {
					// One final input crosses only to seat 3. Seats 1 and 2 see its
					// forwarded bytes and receipt only after both have voted to bridge.
					if (packet.type == Type::PeerInput && owner == hub.row.subject && other.owner == 3) {
						if (!hub.asymmetricFrame) hub.asymmetricFrame = packet.frame;
						if (packet.frame == hub.asymmetricFrame) return 2;
					}
					if (!hub.asymmetricFrame) return 0;
					const bool forwarded = packet.type == Type::PeerForwardInput && packet.successorPeerId == hub.row.subject && packet.frame >= hub.asymmetricFrame;
					const bool receipt = packet.type == Type::PeerReceipt && packet.successorPeerId == hub.row.subject && packet.frame > hub.asymmetricFrame && packet.bytes.empty();
					const uint32_t requesters = ((1U << hub.row.seats) - 1) & ~(1U << (hub.row.subject - 1)) & ~(1U << 2);
					if (hub.row.asymmetricBridge == 3) {
						if (owner == 3 && (requesters & (1U << (other.owner - 1))) != 0 && forwarded) {
							if (lane == NetTransportLane::InputUnreliable) { hub.asymmetricDropped |= 1U << (other.owner - 1); return 1; }
							hub.asymmetricResent |= 1U << (other.owner - 1);
						}
						return 0;
					}
					if (packet.type == Type::PeerBridge && packet.preparedFrame == 0 && packet.frame == hub.asymmetricFrame && (requesters & (1U << (owner - 1))) != 0 &&
					    std::find(packet.members.begin(), packet.members.end(), hub.row.subject) != packet.members.end()) {
						hub.asymmetricVoters |= 1U << (owner - 1);
						if (hub.asymmetricVoters == requesters && !hub.asymmetricReleased) {
							hub.asymmetricReleased = true;
							for (auto& held: hub.delayedInputs) {
								const uint64_t delay = held.input == (hub.row.asymmetricBridge == 1) ? 1 : 2;
								held.target->events.push_back({hub.now + delay, std::move(held.event)});
							}
							hub.delayedInputs.clear();
						}
					}
					if (!hub.asymmetricReleased && owner == 3 && (requesters & (1U << (other.owner - 1))) != 0 && (forwarded || receipt)) {
						hub.delayedInputs.push_back({&other, {NetTransportEventType::PacketReceived, remote, lane, bytes, {}}, forwarded});
						return 1;
					}
				}
				return 0;
			}
			bool RemoveReceiptGap(NetHostMigrationMessage& packet, std::vector<uint8_t>& bytes) {
#if __has_include("NetPeerSessionWire.h")
				if (!hub.row.receiptGap || owner != 3 || packet.type != NetHostMigrationMessageType::PeerInput || packet.offset != 0 || packet.totalBytes != packet.bytes.size()) return false;
				const auto returns = hub.bridgeBoundaries.find(1);
				if (returns == hub.bridgeBoundaries.end() || returns->second.empty() || returns->second.rbegin()->second || packet.frame < returns->second.rbegin()->first + 200) return false;
				if (!hub.receiptGapFrame) { hub.receiptGapFrame = packet.frame; hub.receiptGapAtMs = hub.now; }
				if (packet.frame >= hub.receiptGapFrame + 64) return false;
				std::vector<uint8_t> body = packet.bytes;
				const auto take = [](const std::vector<uint8_t>& data, size_t& at, uint32_t& value) {
					if (at + 4 > data.size()) return false;
					value = 0; for (unsigned i = 0; i < 4; ++i) value |= uint32_t(data[at++]) << (8 * i); return true;
				};
				if (body.empty()) return false;
				if (body[0] == 0) {
					size_t at = 1; uint32_t size = 0;
					if (!take(body, at, size) || size > NetLockstepCodec::c_InputWindowByteCap) return false;
					std::vector<uint8_t> expanded(size);
					if (LZ4_decompress_safe(reinterpret_cast<const char*>(body.data() + at), reinterpret_cast<char*>(expanded.data()), static_cast<int>(body.size() - at), static_cast<int>(size)) != static_cast<int>(size)) return false;
					body = std::move(expanded);
				}
				std::vector<uint8_t> filtered{0}; size_t at = 1; bool removed = false;
				for (unsigned i = 0; i < body[0]; ++i) {
					const size_t begin = at; uint32_t size = 0;
					if (!take(body, at, size) || size > body.size() - at) return false;
					NetLockstepFrame input;
					if (!NetLockstepCodec::DecodeRecoveryInput({body.begin() + at, body.begin() + at + size}, input)) return false;
					at += size;
					if (input.targetFrame == hub.receiptGapFrame) { removed = true; continue; }
					++filtered[0]; filtered.insert(filtered.end(), body.begin() + begin, body.begin() + at);
				}
				if (!removed) return false;
				++hub.receiptGapDrops;
				if (filtered[0] == 0) return true;
				packet.bytes = std::move(filtered); packet.totalBytes = static_cast<uint32_t>(packet.bytes.size());
				NetHash32 key; key.fill(0x39); NetHostMigrationCodec::Encode(packet, key, bytes);
				return false;
#else
				return false;
#endif
			}
			void TraceInputWindow(const std::vector<uint8_t>& bytes, const char* stage, uint8_t receiver) const {
				const char* watched = ObservedTargetText();
				if (!watched || !NetPeerFrameDetail::ReturnInputDetails()) return;
				const uint64_t target = std::strtoull(watched, nullptr, 10);
				NetHostMigrationMessage packet; NetHash32 key; key.fill(0x39);
				if (!NetHostMigrationCodec::Decode(bytes, key, packet)) return;
				const uint8_t source = packet.type == NetHostMigrationMessageType::PeerForwardInput ? packet.successorPeerId : packet.senderPeerId;
				if (InputWindowContains(packet, source, target)) DiagnosticLine() << "[session-plane-input-wire] stage=" << stage
				    << " sender=" << unsigned(packet.senderPeerId) << " owner=" << unsigned(source) << " receiver=" << unsigned(receiver)
				    << " frame=" << target << " through=" << packet.frame << " now_ms=" << hub.now << std::endl;
			}
			void ObserveInputWire(const std::vector<uint8_t>& bytes, uint8_t receiver, bool impaired) {
				if (!NetHostMigrationCodec::LooksLikePacket(bytes)) return;
				NetHostMigrationMessage packet; NetHash32 key; key.fill(0x39);
				if (NetHostMigrationCodec::Decode(bytes, key, packet)) hub.ObserveInput(packet, receiver, impaired);
			}
			void CountInputTicks(const std::vector<uint8_t>& bytes, NetTransportLane lane) {
				NetHostMigrationMessage packet; NetHash32 key; key.fill(0x39);
				if (!NetHostMigrationCodec::Decode(bytes, key, packet) || packet.type != NetHostMigrationMessageType::PeerInput || packet.senderPeerId != 1) return;
				const bool reliable = lane == NetTransportLane::ControlReliable;
				(reliable ? hub.reliableInputBytes : hub.unreliableInputBytes) += bytes.size();
				if (packet.offset != 0 || packet.totalBytes != packet.bytes.size() || packet.bytes.empty()) return;
				const auto take = [](const std::vector<uint8_t>& data, size_t& at, uint32_t& value) {
					if (at + 4 > data.size()) return false;
					value = 0; for (unsigned i = 0; i < 4; ++i) value |= uint32_t(data[at++]) << (8 * i); return true;
				};
				std::vector<uint8_t> body = packet.bytes;
				if (body[0] == 0) {
					size_t at = 1; uint32_t size = 0;
					if (!take(body, at, size) || size > NetLockstepCodec::c_InputWindowByteCap) return;
					std::vector<uint8_t> expanded(size);
					if (LZ4_decompress_safe(reinterpret_cast<const char*>(body.data() + at), reinterpret_cast<char*>(expanded.data()), static_cast<int>(body.size() - at), static_cast<int>(size)) != static_cast<int>(size)) return;
					body = std::move(expanded);
				}
				size_t at = 1;
				for (unsigned i = 0; i < body[0]; ++i) {
					uint32_t size = 0;
					if (!take(body, at, size) || size > body.size() - at) return;
					NetLockstepFrame input;
					if (!NetLockstepCodec::DecodeRecoveryInput({body.begin() + at, body.begin() + at + size}, input)) return;
					at += size;
					++(reliable ? hub.reliableInputTicks : hub.unreliableInputTicks)[input.targetFrame];
				}
			}
			bool Send(NetPeerId id, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override {
				std::lock_guard lock(hub.mutex);
				if (TraceFaultObservation()) DiagnosticLine() << "[direct-fault-observation] now=" << hub.now << " op=send route=" << id
				    << " connected=" << connections.contains(id) << " lane=" << static_cast<int>(lane) << " bytes=" << BytesHex(bytes);
				if (congested) *congested = false;
				const auto found = connections.find(id);
				if (found == connections.end()) { if (error) *error = "the fixture connection is absent"; return false; }
				auto [other, remote] = found->second;
				uint64_t sessionDelay = 0;
				std::vector<uint8_t> delivered = bytes;
				NetHostMigrationMessage decision; NetHash32 key; key.fill(0x39);
				// The packet as its receiver reads it, authenticated once for every check below that only looks at it.
				std::optional<bool> peeked; NetHostMigrationMessage read;
				const auto peekInto = [&](NetHostMigrationMessage& into) {
					if (!peeked) peeked = NetHostMigrationCodec::Decode(bytes, key, read);
					if (*peeked) into = read;
					return *peeked;
				};
				// The delivered packet's message, when it is still the one sent.
				std::shared_ptr<const NetHostMigrationMessage> sent;
				const auto sentMessage = [&]() -> std::shared_ptr<const NetHostMigrationMessage> {
					if (delivered != bytes) return nullptr;
					if (!peeked) peeked = NetHostMigrationCodec::Decode(bytes, key, read);
					if (*peeked && !sent) sent = std::make_shared<const NetHostMigrationMessage>(read);
					return sent;
				};
				const auto observeSent = [&](uint8_t receiver, bool impaired) {
					if (const auto message = sentMessage()) hub.ObserveInput(*message, receiver, impaired);
					else ObserveInputWire(delivered, receiver, impaired);
				};
				if (hub.row.laterSessionMessageFirst && owner == 2 && NetHostMigrationCodec::Decode(bytes, key, decision) &&
				    decision.type == NetHostMigrationMessageType::PeerSession) {
					const auto decoded = NetProtocol::Decode(decision.bytes);
					if (decoded.ok && std::holds_alternative<NetChat>(decoded.message.payload)) {
						sessionDelay = 300; ++hub.delayedChatSends;
						decision.appliedFrame = hub.now; NetHostMigrationCodec::Encode(decision, key, delivered);
					}
				}
				if (hub.row.receiptGap && NetHostMigrationCodec::Decode(bytes, key, decision) && RemoveReceiptGap(decision, delivered)) return true;
				if (NetPeerFrameDetail::ReturnInputDetails() && peekInto(decision) && decision.type == NetHostMigrationMessageType::PeerControl) {
					const auto decoded = NetLockstepCodec::Decode(decision.bytes, ControllerFrame::c_Version);
					if (decoded.ok) if (const auto* start = std::get_if<NetLockstepStart>(&decoded.packet.payload))
						DiagnosticLine() << "[session-plane-start-send] transport_owner=" << unsigned(owner) << " to=" << unsigned(other->owner)
						    << " wrapper_sender=" << unsigned(decision.senderPeerId) << " member=" << unsigned(start->localPeerId)
						    << " members=" << decision.connectedMask << " frame=" << start->startFrame << " delay=" << start->inputDelayFrames << " now_ms=" << hub.now << std::endl;
				}
				if (hub.row.lateReceiptsFrom == owner && hub.DuringFault() && peekInto(decision) &&
				    decision.type == NetHostMigrationMessageType::PeerReceipt) sessionDelay = hub.row.lateReceiptsMs;
				if (hub.row.tailWaitForDirectory && hub.DuringFault() && other->owner == hub.row.subject && peekInto(decision) &&
				    decision.type == NetHostMigrationMessageType::PeerTail) sessionDelay = hub.faultEnd - hub.now;
#if __has_include("NetPeerSessionWire.h")
				// The first frame requests are unavailable for one second. Sender
				// receipt repair must keep the human inputs accepted through that gap.
				if (hub.row.receiptGap && hub.receiptGapFrame && other->owner == 3 && hub.now < hub.receiptGapAtMs + 1000 &&
				    NetHostMigrationCodec::Decode(delivered, key, decision) && decision.type == NetHostMigrationMessageType::PeerControl) {
					const auto decoded = NetLockstepCodec::Decode(decision.bytes, ControllerFrame::c_Version);
					const auto* request = decoded.ok ? std::get_if<NetLockstepAck>(&decoded.packet.payload) : nullptr;
					if (request && request->highestContiguousFrame == hub.receiptGapFrame &&
					    (request->receivedMask & ~0xFFU) == NetLockstepCodec::c_FrameResendRequestMask && (request->receivedMask & 0xFFU) == 3) {
						++hub.receiptGapRequestDrops; return true;
					}
				}
#endif
				if (hub.countInputs && owner == 1) CountInputTicks(bytes, lane);
				if (other->owner == hub.row.subject && !NetHostMigrationCodec::LooksLikePacket(bytes)) {
					const auto lobby = NetLobbyProtocol::Decode(bytes);
					if (lobby.ok) if (const auto* chunk = std::get_if<NetLobbyStateChunk>(&lobby.message.payload);
					    chunk && chunk->transferId == c_NetWorldTailTransferId) ++hub.tailsSentBy[owner];
				}
				// A return is placed past every input its proposer has sent: no voter can have prepared that far.
				if (peekInto(decision) && decision.senderPeerId == owner) {
					if (decision.type == NetHostMigrationMessageType::PeerInput) hub.inputSentThrough[owner] = std::max(hub.inputSentThrough[owner], decision.frame);
					if (decision.type == NetHostMigrationMessageType::PeerTail && other->owner == hub.row.subject) ++hub.tailsSentBy[owner];
					if (decision.type == NetHostMigrationMessageType::PeerTailMissing && other->owner == hub.row.subject) {
						++hub.tailMissingSent; hub.tailMissingFrame = decision.frame; hub.tailMissingDonor = owner;
					}
					if (hub.row.keepInputEvery && owner == hub.row.subject && hub.DuringFault() && decision.type == NetHostMigrationMessageType::PeerInput &&
					    hub.inputSends[{owner, other->owner}]++ % hub.row.keepInputEvery != 0) return true;
					// Judged when it is first sent: the proposer's vote is sent again later, after more of its inputs.
					if (decision.type == NetHostMigrationMessageType::PeerBridge && decision.preparedFrame == 1 && decision.successorPeerId == owner &&
					    hub.returnsProposed.insert({owner, decision.frame}).second && decision.frame <= hub.inputSentThrough[owner] && hub.returnInsideInputs.empty())
						hub.returnInsideInputs = "peer " + std::to_string(owner) + " proposed a return at " + std::to_string(decision.frame) + " with its own inputs sent through " +
						                         std::to_string(hub.inputSentThrough[owner]);
				}
				if (hub.dropReceiptsFrom == owner && peekInto(decision) && decision.type == NetHostMigrationMessageType::PeerReceipt) return true;
				if ((hub.dropInputsFrom == owner || (hub.row.inputsLost && hub.row.secondSubject == owner && hub.DuringFault() && hub.now >= hub.faultAt + hub.row.secondAfterMs)) && peekInto(decision) &&
				    (decision.type == NetHostMigrationMessageType::PeerInput || decision.type == NetHostMigrationMessageType::PeerForwardInput)) return true;
				if (hub.row.inputHalvesLost && hub.DuringFault() && (owner <= 2) != (other->owner <= 2) && peekInto(decision) &&
				    (decision.type == NetHostMigrationMessageType::PeerInput || decision.type == NetHostMigrationMessageType::PeerForwardInput)) return true;
				if (lane == NetTransportLane::ControlReliable && peekInto(decision)) {
					[]<class Message>(Hub& hub, const Message& packet) {
						using Type = std::decay_t<decltype(packet.type)>;
						if constexpr (requires { Type::PeerBridgeCommit; }) {
							if (hub.row.captureFrames && packet.type == Type::PeerBridgeCommit && packet.preparedFrame == 1) hub.returnCertificates.try_emplace(packet.frame, packet);
							if (packet.type == Type::PeerBridgeCommit && packet.preparedFrame <= 1)
								for (uint8_t peer: packet.members) hub.bridgeBoundaries[peer][packet.frame] = packet.preparedFrame == 0;
						}
					}(hub, decision);
				}
				// The voter falls silent as the leader sends its first proposal of the subject's return, so that return can never gather its vote.
				if (hub.row.silentVoter && hub.silentAt == UINT64_MAX && owner == 1 && peekInto(decision) &&
				    decision.type == NetHostMigrationMessageType::PeerBridge && decision.preparedFrame == 1 &&
				    std::find(decision.members.begin(), decision.members.end(), hub.row.subject) != decision.members.end()) hub.silentAt = hub.now;
				if (hub.row.returnByTailOnly && other->owner == hub.row.subject && peekInto(decision) &&
				    (decision.type == NetHostMigrationMessageType::PeerBridge || decision.type == NetHostMigrationMessageType::PeerBridgeCommit) && decision.preparedFrame == 1 &&
				    std::find(decision.members.begin(), decision.members.end(), hub.row.subject) != decision.members.end()) { ++hub.droppedReturns; return true; }
				unsigned asymmetric = 0;
				if (hub.row.asymmetricBridge && hub.DuringFault()) {
					NetHostMigrationMessage packet; NetHash32 key; key.fill(0x39);
					if (NetHostMigrationCodec::Decode(bytes, key, packet)) asymmetric = DelayAsymmetricInput(packet, *other, remote, lane, bytes);
				}
				TraceInputWindow(delivered, "send", other->owner);
				if (asymmetric == 1 || (asymmetric != 2 && hub.Split(owner, other->owner))) { observeSent(other->owner, true); return true; }
				const uint32_t ordinal = ++sends;
				if (lane == NetTransportLane::InputUnreliable && hub.DuringFault() && hub.row.lossPercent &&
				    (owner == hub.row.subject || other->owner == hub.row.subject) && ordinal % 100 < hub.row.lossPercent) { observeSent(other->owner, true); return true; }
				if (hub.schedule) {
					// Jitter delays each packet on its own; a lossy link drops an unreliable packet and resends a reliable one a round trip late.
					const uint32_t rtt = hub.schedule->Rtt(owner, other->owner), jitter = hub.schedule->jitterMs[owner][other->owner], loss = hub.schedule->LossPercent(owner, other->owner, hub.now);
					const bool reliable = lane == NetTransportLane::ControlReliable || lane == NetTransportLane::DiagnosticsReliable;
					const uint32_t outboundJitter = jitter ? hub.Next() % (jitter + 1) : 0;
					const uint32_t lateMs = hub.schedule->LateMs(owner, other->owner, hub.now, reliable);
					uint64_t at = hub.now + hub.Delay(owner, other->owner) + lateMs + sessionDelay + outboundJitter;
					bool retransmitted = false;
					for (unsigned resend = 0; loss && resend < 4 && hub.Next() % 100 < loss; ++resend) {
						if (!reliable) { observeSent(other->owner, true); return true; }
						retransmitted = true;
						at += std::max<uint32_t>(200, rtt + rtt / 2);
					}
					if (retransmitted || lateMs) observeSent(other->owner, true);
					if (jitter && !retransmitted) {
						auto& ping = linkPings[id];
						(void)ping.Read(hub.now, static_cast<uint32_t>(2 * hub.Delay(owner, other->owner)));
						// Reverse transport ACK jitter has its own stream; application delivery and fault draws stay byte-for-byte identical.
						if (!ping.random) ping.random = 0xD1B54A32D192ED03ULL ^ hub.schedule->seed ^ (uint64_t(owner) << 32) ^ (uint64_t(other->owner) << 16) ^ id;
						ping.random ^= ping.random << 13; ping.random ^= ping.random >> 7; ping.random ^= ping.random << 17;
						const uint32_t trip = static_cast<uint32_t>(2 * hub.Delay(owner, other->owner)) + outboundJitter + ping.random % (jitter + 1);
						ping.pending.emplace(hub.now + trip, trip);
					}
					if (reliable) { auto& last = lastDelivery[{id, lane}]; at = std::max(at, last); last = at; }
					if (NetPeerFrameDetail::ReturnInputDetails()) if (const char* watched = ObservedTargetText();
					    watched && peekInto(decision) && (decision.type == NetHostMigrationMessageType::PeerInput || decision.type == NetHostMigrationMessageType::PeerForwardInput) &&
					    decision.frame == std::strtoull(watched, nullptr, 10))
						DiagnosticLine() << "[session-plane-input-delivery] sender=" << unsigned(owner) << " receiver=" << unsigned(other->owner)
						    << " frame=" << decision.frame << " sent_ms=" << hub.now << " arrival_ms=" << at << " reliable=" << reliable << " bytes=" << delivered.size() << std::endl;
					auto message = sentMessage();
					other->events.push_back({at, {NetTransportEventType::PacketReceived, remote, lane, std::move(delivered), {}}, false, std::move(message)}); return true;
				}
#if __has_include("NetPeerSessionWire.h")
				if (hub.row.receiptGap && NetHostMigrationCodec::Decode(delivered, key, decision)) {
					if (decision.type == NetHostMigrationMessageType::PeerTail)
						hub.tailQueuePeak = std::max(hub.tailQueuePeak, GetPeerQueueStats(id).reliableBytes + delivered.size());
					if (decision.type == NetHostMigrationMessageType::PeerTailRequest)
						for (unsigned duplicate = 0; duplicate < 5; ++duplicate)
							other->events.push_back({hub.now + hub.Delay(owner, other->owner), {NetTransportEventType::PacketReceived, remote, lane, delivered, {}}, false});
				}
#endif
				auto message = sentMessage();
				other->events.push_back({hub.now + hub.Delay(owner, other->owner) + sessionDelay, {NetTransportEventType::PacketReceived, remote, lane, std::move(delivered), {}}, asymmetric == 2, std::move(message)}); return true;
			}
			void Disconnect(NetPeerId id, const std::string& reason) override {
				const auto found = connections.find(id); if (found == connections.end()) return;
				auto [other, remote] = found->second; connections.erase(found); other->connections.erase(remote);
				NetTransportEvent closed{NetTransportEventType::PeerDisconnected, remote, NetTransportLane::ControlReliable, {}, reason};
				[]<class Event>(Event& event, bool clean) { if constexpr (requires { event.closedByPeer; }) event.closedByPeer = clean; }(closed, hub.cleanRemoteClose);
				other->events.push_back({hub.now, std::move(closed)});
			}
			void Stop() override { while (!connections.empty()) Disconnect(connections.begin()->first, "fixture closes"); if (port && hub.listeners[port] == this) hub.listeners.erase(port); port = 0; events.clear(); }
			std::vector<NetTransportEvent> PollEvents() override {
				std::vector<NetTransportEvent> ready;
				// Compact pending events once, retaining both pending and delivered order.
				size_t pending = 0;
				for (size_t index = 0; index < events.size(); ++index) {
					auto& event = events[index];
					if (event.at > hub.now) {
						if (pending != index) events[pending] = std::move(event);
						++pending; continue;
					}
					if (event.crossesFault || !hub.Split(connections.contains(event.value.peerId) ? connections.at(event.value.peerId).first->owner : owner, owner)) {
						if (event.value.type == NetTransportEventType::PacketReceived) TraceInputWindow(event.value.bytes, "deliver", owner);
						if (event.value.type == NetTransportEventType::PacketReceived) {
							if (event.sent) hub.ObserveInput(*event.sent, owner, false);
							else ObserveInputWire(event.value.bytes, owner, false);
						}
						// FLAGGED fixture departure: the private image service consumes its transported world-tail chunks before
						// the live coordinator polls this link. A donor's in-memory record is never a received replay frame.
						if (keepImageTail && event.value.type == NetTransportEventType::PacketReceived) {
							const auto lobby = NetLobbyProtocol::Decode(event.value.bytes);
							if (lobby.ok) if (const auto* chunk = std::get_if<NetLobbyStateChunk>(&lobby.message.payload);
							    chunk && chunk->transferId == c_NetWorldTailTransferId) {
								if (const auto route = connections.find(event.value.peerId); route != connections.end())
									receivedImageTail.push_back({route->second.first->owner, event.value.lane, *chunk});
								continue;
							}
						}
						// The real image service keeps certificates received on this seat's link while its private reader runs.
						if (keepReturnCertificates && event.value.type == NetTransportEventType::PacketReceived && NetHostMigrationCodec::LooksLikePacket(event.value.bytes)) {
							NetHash32 key; key.fill(0x39);
							NetHostMigrationMessage certificate;
							if (NetHostMigrationCodec::Decode(event.value.bytes, key, certificate) &&
							    certificate.type == NetHostMigrationMessageType::PeerBridgeCommit && certificate.preparedFrame == 1)
								receivedReturnCertificates.try_emplace(certificate.frame, std::move(certificate));
						}
						// FLAGGED fixture departure: legacy image returns keep the timing certificate delivered on their own authenticated link.
						if (keepReturnCertificates && event.value.type == NetTransportEventType::PacketReceived) {
							const auto decoded = NetLockstepCodec::Decode(event.value.bytes);
							const auto route = connections.find(event.value.peerId);
							if (decoded.ok && route != connections.end())
								if (const auto* timing = std::get_if<NetLockstepTiming>(&decoded.packet.payload);
								    timing && timing->action == NetTimingAction::Reclaim && timing->phase == NetTimingPhase::ReclaimAtFrame &&
								    timing->senderPeerId == route->second.first->owner)
									receivedTimingReturns.try_emplace(std::pair{timing->applyFrame, timing->peerId}, *timing);
						}
						ready.push_back(std::move(event.value));
					}
				}
				events.resize(pending);
				if (TraceFaultObservation()) {
					DiagnosticLine() << "[direct-fault-observation] now=" << hub.now << " op=poll count=" << ready.size();
					for (const auto& event: ready) DiagnosticLine() << "[direct-fault-observation] now=" << hub.now << " op=event type=" << static_cast<int>(event.type)
					    << " route=" << event.peerId << " lane=" << static_cast<int>(event.lane) << " bytes=" << BytesHex(event.bytes);
				}
				return ready;
			}
			uint32_t GetPeerPingMs(NetPeerId id) const override {
				const uint32_t rtt = connections.contains(id) ? static_cast<uint32_t>(2 * hub.Delay(owner, connections.at(id).first->owner)) : 0;
				if (TraceFaultObservation()) DiagnosticLine() << "[direct-fault-observation] now=" << hub.now << " op=rtt route=" << id << " value=" << rtt;
				if (const auto ping = linkPings.find(id); rtt && ping != linkPings.end()) return ping->second.Read(hub.now, rtt);
				return rtt;
			}
			bool IsPeerPingMeasured(NetPeerId id) const override {
				if (TraceFaultObservation()) DiagnosticLine() << "[direct-fault-observation] now=" << hub.now << " op=measured route=" << id << " value=" << connections.contains(id);
				return connections.contains(id);
			}
#if __has_include("NetPeerSessionWire.h")
			NetTransportQueueStats GetPeerQueueStats(NetPeerId id) const override {
				const auto connection = connections.find(id); if (connection == connections.end()) return {};
				const auto [other, remote] = connection->second; uint64_t bytes = 0;
				for (const auto& event: other->events) if (event.value.peerId == remote && event.value.lane == NetTransportLane::ControlReliable) bytes += event.value.bytes.size();
				if (TraceFaultObservation()) DiagnosticLine() << "[direct-fault-observation] now=" << hub.now << " op=queue route=" << id << " reliable_bytes=" << bytes << " queue_ms=0 send_rate=0";
				return {bytes, 0, 0};
			}
#endif
		};
		bool ReadImageTail(const Wire::ImageTailDelivery& delivery, uint8_t donor, uint64_t round, uint64_t applied,
		                   std::vector<uint8_t>& partial, std::map<uint64_t, NetLockstepFrame>& received, std::string& error) {
			uint64_t packetRound = 0;
			if (delivery.donor != donor || !ParseWorldTailChunkRound(delivery.chunk, packetRound)) {
				error = "image tail arrived from another donor or with no round"; return false;
			}
			// As the service's lobby does, a chunk of another round (one still in flight across a rematch) is dropped.
			if (packetRound != round) return true;
			const bool stream = delivery.lane == NetTransportLane::ControlReliable;
			std::vector<uint8_t> packed(delivery.chunk.bytes.begin() + c_NetWorldTailRoundBytes, delivery.chunk.bytes.end());
			if (stream) {
				if (partial.size() + packed.size() > 32ULL * 1024 * 1024) { error = "image tail exceeded the native receive bound"; return false; }
				partial.insert(partial.end(), packed.begin(), packed.end()); packed.swap(partial);
			}
			size_t offset = 0;
			while (offset + 4 <= packed.size()) {
				uint32_t size = 0;
				for (unsigned shift = 0; shift < 32; shift += 8) size |= uint32_t(packed[offset + shift / 8]) << shift;
				if (!size || size > 2 * NetLockstepCodec::c_MaxPeerCount * NetLockstepCodec::c_MaxRecoveryInputBytes) {
					error = "image tail has an invalid native record length"; return false;
				}
				if (size > packed.size() - offset - 4) break;
				offset += 4;
				NetLockstepFrame frame;
				if (!DecodeCommittedJoinFrame({packed.begin() + offset, packed.begin() + offset + size}, frame, &error)) return false;
				if (frame.roundId != round) { error = "image tail record belongs to another round"; return false; }
				if (frame.targetFrame > applied) received.try_emplace(frame.targetFrame, std::move(frame));
				offset += size;
			}
			if (!stream && offset != packed.size()) { error = "image tail datagram ends inside a record"; return false; }
			if (stream) partial.assign(packed.begin() + offset, packed.end());
			return true;
		}
		using WorldDigestCache = std::map<uint64_t, std::pair<std::string, NetHash32>>;
		struct World {
			WorldDigestCache* sharedDigests = nullptr;
			uint64_t applied = 0;
			std::map<int64_t, int64_t> positions;
			// The seats this world's simulation handed to the AI: a hand-over or hand-back is a simulated event like any input.
			std::set<uint8_t> aiSeats;
			std::map<uint64_t, std::string> aiAt; //!< The AI-held seats at each displayed frame, named when two worlds differ.
			std::map<uint64_t, NetHash32> hashes;
			std::map<uint64_t, uint64_t> shownAt;
			bool Apply(const NetLockstepReadyFrame& ready, std::string& error, uint64_t nowMs = 0) {
				if (ready.frame != applied + 1) { error = "a displayed frame was skipped or undone: expected=" + std::to_string(applied + 1) + " actual=" + std::to_string(ready.frame); return false; }
				for (const auto* inputs: {&ready.localFrames, &ready.remoteFrames}) for (const auto& input: *inputs) positions[input.actorUniqueID] += input.analogMoveX;
				for (uint8_t peer: ready.reclaimedPeerIds) aiSeats.erase(peer);
				for (uint8_t peer: ready.aiHeldPeerIds) aiSeats.insert(peer);
				applied = ready.frame;
				shownAt[applied] = nowMs;
				std::string state = std::to_string(applied);
				for (const auto& [actor, position]: positions) state += ":" + std::to_string(actor) + ":" + std::to_string(position);
				std::string ai;
				for (uint8_t peer: aiSeats) ai += (ai.empty() ? "" : ",") + std::to_string(peer);
				if (!ai.empty()) { state += ":ai" + ai; aiAt[applied] = ai; }
				// Identical serialized worlds at this frame have an identical canonical digest. Compare the whole
				// state before reusing it; a divergent state is hashed independently and still fails the history check.
				if (sharedDigests) {
					const auto known = sharedDigests->find(applied);
					if (known != sharedDigests->end() && known->second.first == state) { hashes[applied] = known->second.second; return true; }
				}
				hashes[applied] = NetIdentity::HashCanonicalText("session-plane-fixture", {{"state", state}});
				if (sharedDigests) sharedDigests->try_emplace(applied, std::move(state), hashes.at(applied));
				return true;
			}
		};
		template<class Config> void FrameGroupConfig(Config& config, Hub& hub, uint8_t peer) {
			if constexpr (requires { config.peerFrameGroups; config.frameTieReferee; }) {
				config.peerFrameGroups = NetMatchRunner::UsesPeerFraming(config.matchConfig);
				config.peerSessionLinks = std::make_shared<typename std::decay_t<decltype(config.peerSessionLinks)>::element_type>();
				if (hub.row.adminDirectory || hub.row.adminDirectoryDown) config.hostChangeReferee = [&hub, peer](const NetHostChangeRequest& request) {
					hub.firstAdminRequestMs = std::min(hub.firstAdminRequestMs, hub.now);
					if (hub.row.adminDirectoryDown) return NetHostChangeReply{};
					if (hub.row.repeatedAdministration) {
						if (const auto prior = hub.adminHistory.find(request.generation); prior != hub.adminHistory.end()) return prior->second;
						NetHostChangeReply reply; reply.state = NetHostChangeReply::State::Waiting;
						if (request.generation != hub.directory.generation || peer == hub.row.subject || hub.now < hub.faultAt + 16500) return reply;
						auto& reports = hub.adminReportsByGeneration[request.generation]; reports[peer] = request;
						if (reports.size() != hub.row.seats - 1) return reply;
						reply.state = NetHostChangeReply::State::Decided; reply.generation = request.generation + 1; reply.boundary = UINT64_MAX;
						for (uint16_t member = 1; member <= hub.row.seats; ++member) if (member != hub.row.subject) {
							reply.members.push_back(member); reply.boundary = std::min(reply.boundary, reports.at(member).appliedFrame);
						}
						reply.host = reply.donor = reply.members.front();
						hub.directory.host = reply.host; hub.directory.generation = reply.generation;
						hub.adminHistory.emplace(request.generation, reply); return reply;
					}
					if (hub.adminDecision.state == NetHostChangeReply::State::Decided) return hub.adminDecision;
					hub.adminReports[peer] = request;
					NetHostChangeReply reply; reply.state = NetHostChangeReply::State::Waiting;
					// Deliberately withhold the verdict beyond the silence deadline:
					// frames must continue while the administrator is still undecided.
					if (hub.now < hub.faultAt + 16500 || hub.adminReports.size() != hub.row.seats - 1) return reply;
					reply.state = NetHostChangeReply::State::Decided; reply.generation = request.generation + 1;
					reply.host = reply.donor = 2; reply.boundary = UINT64_MAX;
					for (uint16_t member = 2; member <= hub.row.seats; ++member) {
						reply.members.push_back(member); reply.boundary = std::min(reply.boundary, hub.adminReports.at(member).appliedFrame);
					}
					// The directory's decided host change moves the host and generation its frame ties are checked against.
					hub.directory.host = reply.host; hub.directory.generation = reply.generation;
					hub.adminDecision = reply; return reply;
				};
				if (hub.row.internet) {
					hub.directory.persistentWorld = config.matchConfig.persistentWorld;
					hub.directory.leases.insert(hub.directory.retained.begin(), hub.directory.retained.end());
					// The round's referee is its seat's directory client: the answer it holds, Waiting until its exchange completes.
					if (hub.directory.retained.empty()) for (uint16_t seat = 1; seat <= hub.row.seats; ++seat) hub.directory.retained.push_back(seat);
					hub.directoryClients[peer].StartRound(hub.now, hub.schedule ? hub.schedule->allowancePhaseMs[peer] : (peer * 1700ULL) % 5100);
					config.frameTieReferee = [&hub, peer](const NetFrameTieRequest& request) { return hub.directoryClients[peer].Query(request, hub.now); };
					// Both directory services belong to an Internet session; silence alone cannot override a live host lease.
					if (!config.hostChangeReferee) config.hostChangeReferee = [&hub, peer](const NetHostChangeRequest& request) {
						hub.firstAdminRequestMs = std::min(hub.firstAdminRequestMs, hub.now);
						return hub.directoryClients[peer].QueryHost(request);
					};
				}
			}
		}
		// The decisions a peer let go after keeping them past the silence the round allows (0 on an engine that does not count them).
		template<class Stats> uint64_t DecisionsDropped(const Stats& stats) { if constexpr (requires { stats.decisionsDropped; }) return stats.decisionsDropped; else return 0; }
		template<class Peer> bool CatchingUp(const Peer& peer, double ordinaryWaitMs = 0) {
			// FLAGGED fixture departure: ScenarioRunner also grants legacy own-seat reclaim gaps their bounded extra work.
			if (!peer.UsesPeerFrameGroups() && peer.IsRunning() && (peer.IsOwnHostSeatHeld() ||
			    peer.IsSeatReclaimGap(peer.GetConfig().localPeerId, peer.GetResumeFrame()))) return true;
			if constexpr (requires { peer.PeerFrameCatchUpActive(ordinaryWaitMs); }) return peer.PeerFrameCatchUpActive(ordinaryWaitMs);
			else if constexpr (requires { peer.PeerFrameCatchUpActive(); }) return peer.PeerFrameCatchUpActive();
			else return false;
		}
		template<class Peer> bool CatchUpNeeded(const Peer& peer) {
			if (!peer.UsesPeerFrameGroups()) return CatchingUp(peer);
			if constexpr (requires { peer.PeerFrameCatchUpNeeded(); }) return peer.PeerFrameCatchUpNeeded(); else return CatchingUp(peer);
		}
		template<class Peer> std::optional<uint64_t> AcceptedThrough(const Peer& peer) { if constexpr (requires { peer.GetLocalInputAcceptedThrough(); }) return peer.GetLocalInputAcceptedThrough(); else return std::nullopt; }
		template<class Peer> uint32_t SenderResizes(const Peer& peer, uint8_t sender) {
			if constexpr (requires { peer.GetStats().peers.at(sender).delayResizes; }) return peer.GetStats().peers.at(sender).delayResizes;
			else return peer.GetStats().delayChangesCommitted;
		}
		template<class Peer> void Blackout(Peer& peer, uint64_t frame, uint64_t ms, bool directoryDark) {
			if constexpr (requires { peer.SetPeerFrameBlackoutForTest(frame, ms, directoryDark); }) peer.SetPeerFrameBlackoutForTest(frame, ms, directoryDark);
			else if constexpr (requires { peer.SetPeerFrameBlackoutForTest(frame, ms); }) peer.SetPeerFrameBlackoutForTest(frame, ms);
		}
		template<class Peer> constexpr bool HasBlackout() { return requires(Peer& peer) { peer.SetPeerFrameBlackoutForTest(1, 1); }; }
		template<class Result> uint64_t AdminActivation(const Result& result) {
			if constexpr (requires { result.activationFrame; }) return result.activationFrame;
			else return result.boundary + 1;
		}
		template<class Peer> std::map<uint8_t, uint64_t> Bridges(const Peer& peer) {
			if constexpr (requires { peer.SeatBridgeTimes(); }) return peer.SeatBridgeTimes();
			else { std::map<uint8_t, uint64_t> held; for (const auto& [id, hold]: peer.HeldTransactions()) held[id] = hold.cutoffFrame; return held; }
		}
		template<class Peer> bool HoldVisible(const Peer& peer, uint8_t seat, uint64_t nowMs) {
			if constexpr (requires { peer.IsSeatHoldVisible(seat, nowMs); }) return peer.IsSeatHoldVisible(seat, nowMs);
			else return peer.HasHeldAISeat(seat);
		}
		template<class Estimator> uint16_t InitialSenderDelay(Estimator& estimator, bool host, uint16_t floor) {
			if constexpr (requires { estimator.SenderRequiredFrames(c_TestTickMs, c_TestBoundTicks, floor); })
				return static_cast<uint16_t>(estimator.SenderRequiredFrames(c_TestTickMs, c_TestBoundTicks, floor));
			else return host ? floor : static_cast<uint16_t>(std::min<uint32_t>(NetMatchConfigUtil::c_MaxInputDelayFrames, estimator.RequiredFrames(c_TestTickMs, floor) + c_TestBoundTicks));
		}
		bool IsLosingContinuationDecision(bool wasSplit, uint8_t seat, const NetFrameTieRequest& request, const NetFrameTieReply& reply) {
			// FLAGGED fixture departure: a private reader queries a ruling without offering another continuation to discard.
			return wasSplit && !request.queryOnly && std::find(reply.members.begin(), reply.members.end(), seat) == reply.members.end();
		}
		// A row on a pool worker runs beside others: no plane ticks its coordinators, so it takes no plane lock and none waits on it.
		thread_local bool t_PoolWorker = false;
		// FLAGGED fixture departure: Main/TimerMan retain ordinary elapsed time separately from the bounded extra grants.
		// Every extra grant needs a phase goal and receipt runway; a receipt wait preserves the goal's bounded accrual.
		struct FixtureTickCredit {
			double ordinary = 0, accelerated = 0;
			bool wasCatchingUp = false;
			void Accrue(double elapsedMs, double ordinaryRate, double catchUpRate, bool catchingUp) {
				ordinary = std::min(6.0, ordinary + elapsedMs * ordinaryRate / c_TestTickMs);
				// FLAGGED fixture correction: the runner sets its budget once a real frame, and the frame in which catching up begins
				// still has the budget it began with; the tick the round's clock owes then is never withheld for a budget accruing from zero.
				if (catchingUp && !wasCatchingUp) accelerated = std::max(accelerated, 1.0);
				accelerated = catchingUp ? std::min(6.0, accelerated + elapsedMs * catchUpRate / c_TestTickMs) : 0;
				wasCatchingUp = catchingUp;
			}
			bool MayGrant(bool needed, bool canAccelerate) const {
				if (needed && accelerated < 1) return false;
				return ordinary >= 1 || (needed && canAccelerate && accelerated >= 1);
			}
			double NextOrdinaryTickInMs(double pollMs = 0) const {
				// FLAGGED fixture departure: an ordinary tick cannot run between the fixture's discrete polls.
				const double remaining = std::max(0.0, 1.0 - ordinary) * c_TestTickMs;
				return pollMs > 0 ? std::ceil(remaining / pollMs) * pollMs : remaining;
			}
			void Consume(bool catchingUp) {
				// GrantSimUpdates adds one whole tick only when ordinary time cannot fund it; its fractional remainder survives.
				if (ordinary >= 1) ordinary -= 1;
				if (catchingUp) accelerated -= 1;
			}
			void Reset() { ordinary = accelerated = 0; wasCatchingUp = false; }
		};
		struct Fixture {
			NetSeatRoster agreedRoster;
			WorldDigestCache sharedDigests;
			Hub hub;
			std::vector<std::unique_ptr<Wire>> wires;
			std::vector<std::unique_ptr<NetLockstepCoordinator>> peers;
			std::vector<World> worlds;
			std::vector<std::map<uint64_t, NetLockstepReadyFrame>> committed;
			std::vector<std::vector<uint32_t>> frameMembers;
			std::vector<std::vector<uint8_t>> frameAuthors;
			std::vector<HoldBoundaries> installedBoundaries;
			std::vector<std::vector<std::map<uint64_t, NetHash32>>> discardedHistories;
			std::array<std::array<uint64_t, 5>, 5> compared{}, splitFrom{};
			// A divergence with no cause yet, and since when: a late input or receipt proves its lateness only once it lands.
			std::array<std::array<std::pair<uint64_t, uint64_t>, 5>, 5> unexplained{};
			static constexpr uint64_t c_LateEvidenceMs = 2000;
			// FLAGGED fixture departure: two histories that diverged under a proven split stay apart until an image return replaces one,
			// whoever authors their later frames - a reader of one half that returns into it carries that half's separation.
			std::array<std::array<uint64_t, 5>, 5> pairSeparatedFrom{};
			std::array<std::array<std::pair<uint8_t, uint8_t>, 5>, 5> inheritedApart{}; //!< The third seat and the far side a pair is apart from by inheritance.
			std::array<std::array<std::map<uint64_t, uint64_t>, 5>, 5> inputSplits;
			std::array<std::array<std::map<uint64_t, std::pair<uint64_t, uint64_t>>, 5>, 5> inputSplitTimes; //!< When each proven separation opened and, once the pair agrees again, closed.
			struct InputSplitCause { uint8_t human, held, sender, receiver; uint64_t faultAt, observedAt; };
			std::vector<InputSplitCause> inputSplitCauses;
			std::array<std::array<uint32_t, 5>, 5> partitionOwners{}; //!< The owners whose partition between a pair has its proven cause.
			// A divergence from an input or receipt that arrived late, not lost: accepted only while the engine arbitrates it, and closed
			// when either side's image return reconverges them.
			struct LateSplit { uint8_t a = 0, b = 0, owner = 0; uint64_t frame = 0, observedAt = 0, resolvedAt = 0; };
			std::vector<LateSplit> lateSplits;
			std::vector<LateSplit> earlierLateSplits; //!< The late splits of rounds a rematch ended, kept for the return property.
			const LateSplit* OpenLateSplit(uint8_t a, uint8_t b) const {
				for (const auto& late: lateSplits) if (!late.resolvedAt && ((late.a == a && late.b == b) || (late.a == b && late.b == a))) return &late;
				return nullptr;
			}
			std::string UnreconvergedSeparations() const {
				// A seat still replaying its image at the end - one whose machine cannot catch up stays there - has not yet replaced its history.
				// A seat whose player left or was removed has no machine left to reconverge: its player comes back, if ever, through a new return.
				const auto gone = [&](uint8_t seat) { return hub.schedule && hub.schedule->Left(seat, hub.now); };
				for (uint8_t a = 1; a <= hub.row.seats; ++a) for (uint8_t b = a + 1; b <= hub.row.seats; ++b)
					if (pairSeparatedFrom[a][b] && !imageReading[a - 1] && !imageNeeded[a - 1] && !imageReading[b - 1] && !imageNeeded[b - 1] && !gone(a) && !gone(b))
					return "histories differ from displayed frame " + std::to_string(pairSeparatedFrom[a][b]) + ": peers " + std::to_string(a) + " and " + std::to_string(b) +
					       " parted under a proven split and were never reconverged by an image return";
				return {};
			}
			std::string UnresolvedLateSplits() const {
				for (const auto& late: lateSplits) if (!late.resolvedAt)
					return "histories differ at displayed frame " + std::to_string(late.frame) + ": peers " + std::to_string(late.a) + " and " + std::to_string(late.b) +
					       " parted on seat " + std::to_string(late.owner) + "'s late input at " + std::to_string(late.observedAt) + " ms and were never reconverged by an image return";
				return {};
			}
			std::vector<bool> waitPlaying, imageReading, imageNeeded, imageWithoutHeadroom;
			std::array<std::set<uint8_t>, 4> abandonedDonors; //!< The donors whose return sessions each seat closed: given up when their own continuation was set aside, or finished.
			std::array<uint64_t, 5> returnActivations{}; //!< Each returner's announced activation, which the round's one join host keeps distinct.
			std::vector<ImageReturnTask> imageTasks;
			std::vector<bool> splitParticipant, hungLastStep;
			std::vector<uint32_t> observedSplit;
			std::vector<uint64_t> playingMaxWait, lostAt, lostFrame, lossServedAt, imageReturns;
			std::vector<uint32_t> lostRuling; //!< The winners of the ruling each seat stopped on, its own or its deciding seat's.
			std::vector<size_t> decisionsRead;
			std::vector<std::set<std::tuple<uint64_t, uint64_t, NetHash32, uint64_t>>> splitDecisions;
			bool sawSplit = false, splitHistories = false;
			uint32_t directRuling = 0; //!< Independently ranked committed groups at the heal, before processing its first packet.
			std::function<void(NetLockstepCoordinator&, INetTransport&)> onFreeze;
			std::function<void()> onThaw;
			bool freezeStarted = false, thawed = false;
			uint64_t freezeWallMs = 0;
			std::vector<FixtureTickCredit> credits;
			std::vector<uint64_t> queued, waitAt, maxWait, caughtAt;
			std::vector<std::vector<NetGameCommand>> carriedCommands; //!< Commands of a sample the coordinator deferred, riding the seat's next one.
			std::vector<uint64_t> releasedAt; //!< The engine's tick that released each seat's waited frame, which the bound is judged at.
			std::vector<uint64_t> displayedAt, blackoutGap, returnedAt, postReturnWait, imageAt;
			std::vector<uint64_t> firstDisplayedAt;
			std::vector<uint64_t> startupFinishAt;
			std::vector<int64_t> preFaultPaceLeadMs;
			std::vector<bool> seenAI;
			std::vector<uint16_t> settledDelay;
			std::vector<uint32_t> settledChanges, settledHostWaits;
			std::vector<uint64_t> paceBegin, paceEnd;
			std::vector<uint64_t> firstAdminChangeMs;
			uint64_t substitutes = 0, aiFrames = 0, hostChanges = 0, firstLostPressMs = UINT64_MAX;
			std::string wronglyHeld;
			// A fixture run on a worker thread beside others takes no plane lock: no plane ticks its coordinators.
			bool parallel = false;
			// The random driver's record of a scheduled round: every wait past the bound, the spans each seat is held in any peer's view,
			// and the frames every world agrees on.
			struct Wait { uint8_t seat = 0; uint64_t startMs = 0, endMs = 0, frame = 0; bool playing = false; };
			std::vector<Wait> waits;
			std::vector<std::vector<std::pair<uint64_t, uint64_t>>> heldSpans;
			// Each image return a seat began: when, the round's lead on the image once it arrives, and the image's trip.
			struct ImageStart { uint64_t atMs = 0, gapMs = 0, transitMs = 0; uint8_t donor = 0; uint64_t spacingMs = 0; };
			std::vector<std::vector<ImageStart>> imageStarts;
			std::vector<uint64_t> heldSince;
			std::array<std::array<uint64_t, NetMatchConfigUtil::c_MaxPeerCount + 1>, NetMatchConfigUtil::c_MaxPeerCount + 1> heldByObserver{}; //!< When each seat's own engine began holding each seat, its spells included.
			std::map<std::pair<uint8_t, uint64_t>, uint8_t> lossyHoldAuthors; //!< A seat's hold at a frame, and the observer that installed it while its own links lost packets.
			uint64_t hashedThrough = 0;
			bool leaverStopped = false;
			// The round ended by the engine's own rule: every other seat left or was removed, and the last one completes it.
			bool roundOver = false;
			std::string earlierRoundRule; //!< A rule an earlier round of a rematch broke, judged before its frames were set aside.
			// A hold ends once: every peer's certified history names one return frame for it.
			std::string TwoReturnFrames() const {
				const auto check = [](const HoldBoundaries& history) -> std::string {
					for (const auto& [seat, boundaries]: history)
						for (auto at = boundaries.begin(); at != boundaries.end() && std::next(at) != boundaries.end(); ++at)
							if (!at->second && !std::next(at)->second)
								return "one hold of seat " + std::to_string(seat) + " was certified to return at " + std::to_string(at->first) + " and " + std::to_string(std::next(at)->first);
					return {};
				};
				// With no physical split every certificate belongs to one history (including jitter-only seed 131).
				if (!splitHistories) return check(hub.bridgeBoundaries);
				for (size_t observer = 0; observer < installedBoundaries.size(); ++observer)
					if (const auto why = check(installedBoundaries[observer]); !why.empty()) return why + " in peer " + std::to_string(observer + 1) + "'s installed history";
				return {};
			}
			// Any spell of the seat's own or a loss burst on its own links: the schedule's cause rule charges each to the seat.
			bool OwnTrouble(uint8_t seat) const {
				if (OwnMachineTrouble(seat)) return true;
				if (!hub.schedule) return false;
				if (hub.schedule->Active(seat, hub.now)) return true;
				for (const auto& burst: hub.schedule->bursts) if (burst.seat == seat && hub.now >= burst.atMs && hub.now < burst.atMs + burst.ms) return true;
				return false;
			}
			// A scheduled fault of the seat's own at a past moment: its spell or its loss burst.
			bool OwnTroubleAt(uint8_t seat, uint64_t at) const {
				if (!hub.schedule) return false;
				if (hub.schedule->Active(seat, at)) return true;
				for (const auto& burst: hub.schedule->bursts) if (burst.seat == seat && at >= burst.atMs && at < burst.atMs + burst.ms) return true;
				return false;
			}
			// The observer holds the seat from inside a fault, now its own or since a hold it installed during its own or the seat's.
			bool HoldFromOwnFault(uint8_t observer, uint8_t seat) const {
				const uint64_t since = heldByObserver[observer][seat];
				return OwnTrouble(observer) || (since && (OwnTroubleAt(observer, since) || OwnTroubleAt(seat, since)));
			}
			bool OwnMachineTrouble(uint8_t seat) const {
				if (hub.schedule) {
					const auto* spell = hub.schedule->Active(seat, hub.now);
					return spell && (spell->kind == Spell::Kind::Hung || spell->kind == Spell::Kind::Stopped || spell->kind == Spell::Kind::Slow);
				}
				return seat == hub.row.subject && ((hub.DuringFault() && (hub.row.frozen || hub.row.hung || hub.row.slowPercent)) || hub.InRefault());
			}
			uint32_t RankDirectHistories() const {
				uint32_t humans = 0, winner = 0;
				const auto& config = peers.front()->GetConfig().matchConfig;
				for (const auto& player: config.players) if (!player.cpu && !(config.dedicated && player.peerId == config.hostPeerId)) humans |= 1U << (player.peerId - 1);
				const uint32_t host = 1U << (config.hostPeerId - 1);
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
					if (!worlds[seat - 1].applied || frameMembers[seat - 1].empty() || OwnMachineTrouble(seat)) continue;
					const uint32_t group = frameMembers[seat - 1].back();
					if (!(group & (1U << (seat - 1)))) continue;
					const int votes = std::popcount(group & humans), most = std::popcount(winner & humans);
					if (!winner || votes > most || (votes == most && ((group & host) > (winner & host) ||
					    ((group & host) == (winner & host) && std::countr_zero(group) < std::countr_zero(winner))))) winner = group;
				}
				return winner;
			}
			bool ImageDonorCanAnswer(uint8_t donor) const {
				// FLAGGED fixture departure: Running describes the coordinator, not whether its process can serve bytes.
				// A hung process keeps its queued socket traffic but cannot answer an image request until it resumes; a departed seat serves nothing.
				return peers[donor - 1]->IsRunning() && !hub.ProcessHung(donor) && !(hub.schedule && hub.schedule->Left(donor, hub.now));
			}
			uint8_t ImageReturnProposer(uint8_t donor) const {
				// FLAGGED fixture departure: the image source can be any donor; only the first playing survivor proposes reclaim.
				const uint8_t leader = peers[donor - 1]->PeerReturnLeader();
				return leader ? leader : donor;
			}
			std::pair<INetTransport*, NetPeerId> ImageReturnRoute(uint8_t sender, uint8_t receiver) const {
				if (peers[sender - 1]->GetConfig().peerFrameGroups) return s_ReturnRoute(*peers[sender - 1], receiver);
				// FLAGGED fixture departure: the legacy image service retains its authenticated session route while the seat is held.
				for (const auto& [route, connection]: wires[sender - 1]->connections)
					if (connection.first->owner == receiver) return {wires[sender - 1].get(), route};
				return {nullptr, c_InvalidNetPeerId};
			}
			std::vector<uint8_t> ImageReturnDonors() const {
				// FLAGGED fixture departure: a case that requires another donor must actually request that donor first.
				// Eligibility, ruling membership and the native tail-traffic assertion still decide whether it succeeds.
				std::vector<uint8_t> donors;
				if (hub.row.donorSeat && hub.row.donorSeat <= hub.row.seats) donors.push_back(hub.row.donorSeat);
				for (uint8_t donor = 1; donor <= hub.row.seats; ++donor) if (donor != hub.row.donorSeat) donors.push_back(donor);
				return donors;
			}
			bool RecoverImages(std::string& error) {
				// FLAGGED fixture departure: independent private returners advance on the same shared tick.
				for (auto& task: imageTasks) if (!task.Done() && !task.Resume()) return false;
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
					if (!imageNeeded[seat - 1] || imageReading[seat - 1] || OwnMachineTrouble(seat)) continue;
					bool connected = true;
					for (uint8_t other = 1; other <= hub.row.seats; ++other) connected &= !hub.LiveSplit(seat, other);
					if (!connected) continue;
					uint32_t ruling = (1U << hub.row.seats) - 1;
					if (lostAt[seat - 1] && lostAt[seat - 1] != lossServedAt[seat - 1] && hub.row.internet) {
						ruling = lostRuling[seat - 1];
						// FLAGGED fixture departure: a ruling half that lost a ruling of its own since rejoins another continuation through its
						// image; this seat asks again from that half, wherever it now plays.
						for (uint32_t asked = 0; asked != ruling;) {
							asked = ruling;
							for (uint8_t winner = 1; winner <= hub.row.seats; ++winner) {
								const auto client = hub.directoryClients.find(winner);
								if (!(asked & (1U << (winner - 1))) || !lostAt[winner - 1] || !(imageNeeded[winner - 1] || imageReading[winner - 1]) ||
								    client == hub.directoryClients.end()) continue;
								ruling |= lostRuling[winner - 1];
							}
						}
					} else if (!hub.row.internet && sawSplit && directRuling) ruling = directRuling;
					for (uint8_t donor: ImageReturnDonors()) {
						if (donor == seat || !(ruling & (1U << (donor - 1))) || !ImageDonorCanAnswer(donor) ||
						    !peers[donor - 1]->HasHeldAISeat(seat) || !(s_FrameMembers(*peers[donor - 1], worlds[donor - 1].applied + 1) & (1U << (donor - 1)))) continue;
						if (!ImageDonorCanAnswer(ImageReturnProposer(donor))) continue;
						// FLAGGED fixture departure: a member whose own image return the round has already activated keeps its world in that
						// return's replay until the return ends, so the capture every playing seat committed waits for it.
						const uint32_t group = s_FrameMembers(*peers[donor - 1], worlds[donor - 1].applied + 1);
						bool adopted = true;
						for (uint8_t member = 1; member <= hub.row.seats; ++member)
							adopted &= member == seat || !(group & (1U << (member - 1))) || !imageReading[member - 1] || !returnActivations[member] ||
							    returnActivations[member] > worlds[donor - 1].applied + 1;
						if (!adopted) continue;
						imageTasks[seat - 1] = ReadImageReturn(*this, seat, donor, error);
						if (!imageTasks[seat - 1].Resume()) return false;
						break;
					}
				}
				return true;
			}
			bool FinishImageReturns(std::string& error) {
				while (std::any_of(imageTasks.begin(), imageTasks.end(), [](const ImageReturnTask& task) { return !task.Done(); }))
					if (!Step(error) || !CheckBound(error) || !RecoverImages(error)) return false;
				return CheckImageOutcomes(error);
			}
			bool CheckImageOutcomes(std::string& error) const {
				if (std::any_of(imageTasks.begin(), imageTasks.end(), [](const ImageReturnTask& task) { return !task.Done(); })) {
					error = "authority ruling image return: round ended before the private return and its healthy observation finished"; return false;
				}
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
					if (imageNeeded[seat - 1] && !imageWithoutHeadroom[seat - 1]) {
						error = "authority ruling: peer " + std::to_string(seat) + " stopped but never completed its private image return"; return false;
					}
					if (sawSplit && !hub.schedule) {
						if (!peers[seat - 1]->IsRunning() || !Bridges(*peers[seat - 1]).empty()) {
							std::string bridged;
							for (const auto& [held, from]: Bridges(*peers[seat - 1])) bridged += " " + std::to_string(held) + "@" + std::to_string(from);
							error = "authority ruling: peer " + std::to_string(seat) + " did not rejoin live play after the split healed: running=" +
							        std::to_string(peers[seat - 1]->IsRunning()) + " bridged=" + (bridged.empty() ? " none" : bridged); return false;
						}
					}
				}
				return true;
			}
			// The game takes a frame the engine releases inside the update that waited for it: the wait ends at that tick, not at the
			// fixture's next step.
			uint64_t WaitEnd(size_t index) const { return index < releasedAt.size() && releasedAt[index] ? releasedAt[index] : hub.now; }
			void FinishWait(uint8_t seat, uint64_t frame) {
				const size_t index = seat - 1;
				if (!waitAt[index]) return;
				const uint64_t endAt = WaitEnd(index), duration = endAt - waitAt[index];
				maxWait[index] = std::max(maxWait[index], duration);
				if (waitPlaying[index]) playingMaxWait[index] = std::max(playingMaxWait[index], duration);
				if (duration > static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs))) waits.push_back({seat, waitAt[index], endAt, frame, waitPlaying[index]});
				if (returnedAt[index]) postReturnWait[index] = std::max(postReturnWait[index], duration);
				waitAt[index] = 0;
				if (index < releasedAt.size()) releasedAt[index] = 0;
			}
			void ObserveBoundRole(size_t index, bool playing) {
				if (!waitAt[index] || !waitPlaying[index] || (playing && !OwnMachineTrouble(static_cast<uint8_t>(index + 1)))) return;
				// FLAGGED fixture departure: an own hold or injected own-machine pause ends this wait's playing portion.
				// As when a wait begins during that pause, its raw time remains, as does any preceding playing overrun.
				playingMaxWait[index] = std::max(playingMaxWait[index], WaitEnd(index) - waitAt[index]);
				waitPlaying[index] = false;
			}
			bool CheckBound(std::string& error) const {
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
					const size_t index = seat - 1;
					const uint64_t wait = std::max(playingMaxWait[index], waitAt[index] && waitPlaying[index] ? WaitEnd(index) - waitAt[index] : 0);
					if (wait > static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs))) {
						error = "authority ruling: playing peer " + std::to_string(seat) + " waited " + std::to_string(wait) +
						    "ms; the local hold/stop bound is 50ms, including both split halves and a pending or unreachable directory" +
						    "; now_ms=" + std::to_string(hub.now) +
						    (index < worlds.size() ? " applied=" + std::to_string(worlds[index].applied) : " world=unconstructed") +
						    (index < peers.size() ? s_WaitState(*peers[index]) : " peer=unconstructed");
						return false;
					}
				}
				return true;
			}
			void ObserveSplit(uint8_t a, uint8_t b) {
				sawSplit = true;
				auto& from = splitFrom[a][b];
				// FLAGGED fixture departure: only the minimum displayed cursor is a prefix both peers committed before this cut.
				if (!from) from = std::min(worlds[a - 1].applied, worlds[b - 1].applied) + 1;
			}
			// An image return replaces the seat's history: every comparison with it starts again.
			void ForgetComparisons(uint8_t seat) {
				for (auto& late: lateSplits) if (!late.resolvedAt && (late.a == seat || late.b == seat)) late.resolvedAt = hub.now;
				for (uint8_t other = 1; other <= hub.row.seats; ++other) {
					compared[std::min(seat, other)][std::max(seat, other)] = 0; pairSeparatedFrom[std::min(seat, other)][std::max(seat, other)] = 0;
					unexplained[std::min(seat, other)][std::max(seat, other)] = {};
				}
			}
			bool InputSplitCausedHold(uint8_t seat, uint64_t since, uint64_t until, uint8_t troubled, uint64_t from, uint64_t to) const {
				// Section 11.1: the halves a proven partition parted hold each other's seats from it to their reconverging return.
				const auto separatedAt = [&](uint8_t a, uint8_t b, uint64_t at) {
					if (a > b) std::swap(a, b);
					return std::any_of(inputSplitTimes[a][b].begin(), inputSplitTimes[a][b].end(), [&](const auto& separation) {
						return separation.second.first <= at && (separation.second.second == 0 || at <= separation.second.second);
					});
				};
				return std::any_of(inputSplitCauses.begin(), inputSplitCauses.end(), [&](const InputSplitCause& cause) {
					return (seat == cause.human || seat == cause.held) && (troubled == cause.sender || troubled == cause.receiver) &&
					    cause.faultAt >= from && cause.faultAt < to && cause.faultAt <= since && (since <= cause.observedAt || separatedAt(cause.human, cause.held, since)) &&
					    cause.observedAt <= until;
				});
			}
			bool CompareHistories(std::string& error, bool finalVerdict = false) {
				for (uint8_t a = 1; a <= hub.row.seats; ++a) for (uint8_t b = a + 1; b <= hub.row.seats; ++b) {
					const auto& left = worlds[a - 1]; const auto& right = worlds[b - 1];
					const uint64_t common = std::min(left.applied, right.applied);
					bool awaitingEvidence = false;
					for (uint64_t frame = compared[a][b] + 1; frame <= common; ++frame) {
						const uint32_t l = frameMembers[a - 1].at(frame), r = frameMembers[b - 1].at(frame);
						const uint8_t authorA = frameAuthors[a - 1].at(frame), authorB = frameAuthors[b - 1].at(frame);
						if (left.hashes.at(frame) == right.hashes.at(frame)) {
							if (l == r && (l & (1U << (a - 1))) && (l & (1U << (b - 1))))
								for (auto& [from, through]: inputSplits[a][b]) if (!through && frame >= from) { through = frame - 1; inputSplitTimes[a][b][from].second = hub.now; }
							pairSeparatedFrom[a][b] = 0; inheritedApart[a][b] = {};
							continue;
						}
						if (const auto* late = OpenLateSplit(a, b); late && frame >= late->frame) { splitHistories = true; continue; }
						const bool authors = authorA && authorA <= hub.row.seats && authorB && authorB <= hub.row.seats && authorA != authorB;
						uint64_t separated = authors ? splitFrom[std::min(authorA, authorB)][std::max(authorA, authorB)] : 0;
						if (!separated && pairSeparatedFrom[a][b] && frame >= pairSeparatedFrom[a][b]) separated = pairSeparatedFrom[a][b];
						if (authors && !separated) {
							const auto& intervals = inputSplits[std::min(authorA, authorB)][std::max(authorA, authorB)];
							const auto after = intervals.upper_bound(frame);
							if (after != intervals.begin()) {
								const auto& [from, through] = *std::prev(after);
								if (!through || frame <= through) separated = from;
							}
						}
						const uint32_t first = authors ? 1U << (authorA - 1) : 0, second = authors ? 1U << (authorB - 1) : 0;
						// The proven cause of one owner's seat played on one side and held on the other: its input, or the receipt covering it, lost on the wire.
						const auto recordPartition = [&](uint8_t owner) {
							const uint32_t bit = 1U << (owner - 1);
							if (((l ^ r) & bit) == 0) return false;
							const bool humanLeft = (l & bit) != 0;
							const auto& human = humanLeft ? left : right; const auto& held = humanLeft ? right : left;
							const auto ai = held.aiAt.find(frame);
							if (ai == held.aiAt.end() || ("," + ai->second + ",").find("," + std::to_string(owner) + ",") == std::string::npos) return false;
							// FLAGGED fixture departure: an exact impaired receipt separates histories at the held seat's bound as an input does.
							const bool heldInHuman = ((humanLeft ? l : r) & (1U << ((humanLeft ? b : a) - 1))) != 0;
							const auto* cause = hub.InputPartition(owner, humanLeft ? a : b, humanLeft ? b : a, frame, human.shownAt.at(frame), held.shownAt.at(frame));
							if (!cause && heldInHuman) cause = hub.ReceiptPartition(owner, humanLeft ? b : a, frame, held.shownAt.at(frame));
							if (!cause) return false;
							partitionOwners[a][b] |= bit;
							inputSplitCauses.push_back({humanLeft ? a : b, humanLeft ? b : a, cause->packet.senderPeerId, cause->receiver,
							    cause->at, std::max(human.shownAt.at(frame), held.shownAt.at(frame))});
							DiagnosticLine() << "[session-plane-input-partition] frame=" << frame << " owner=" << unsigned(owner)
							    << " human_author=" << unsigned(humanLeft ? a : b) << " held_author=" << unsigned(humanLeft ? b : a)
							    << (cause->packet.type == NetHostMigrationMessageType::PeerReceipt ? " exact_impaired_receipt=1" : " exact_impaired_input=1") << " delivered_before_hold=0" << std::endl;
							return true;
						};
						// FLAGGED fixture departure: exact wire loss can separate inputs at the local bound without a total transport cut.
						if (!separated && authorA == a && authorB == b && authors && l != r && (l & first) && (r & second) &&
						    left.shownAt.contains(frame) && right.shownAt.contains(frame)) {
							for (uint8_t owner = 1; owner <= hub.row.seats; ++owner) {
								if (!recordPartition(owner)) continue;
								inputSplits[a][b][frame] = 0; inputSplitTimes[a][b][frame] = {hub.now, 0}; separated = frame; sawSplit = true;
								break;
							}
						}
						// A pair apart under that cause from an earlier frame stays apart on frames where their member sets agree again, until an
						// image return replaces one history; the round's end requires that return.
						if (separated && frame >= separated && (l & first) && (r & second) && (l != r || (pairSeparatedFrom[a][b] && frame > pairSeparatedFrom[a][b]))) {
							// FLAGGED fixture departure: each half holds the seat it cannot hear, so an owner first held on one side after the split has its own proven divergence.
							if (authorA == a && authorB == b && frame > 0 && left.shownAt.contains(frame) && right.shownAt.contains(frame)) {
								const uint32_t before = frameMembers[a - 1].at(frame - 1) ^ frameMembers[b - 1].at(frame - 1);
								for (uint8_t owner = 1; owner <= hub.row.seats; ++owner)
									if (((l ^ r) & ~before & ~partitionOwners[a][b] & (1U << (owner - 1))) != 0) (void)recordPartition(owner);
							}
							if (!pairSeparatedFrom[a][b]) pairSeparatedFrom[a][b] = frame;
							splitHistories = true; continue;
						}
						// Section 11.1: the side that holds a seat the other plays showed the frame without traffic the other had - an input for it, or a
						// receipt covering it, that reached this side only afterwards: late, never lost. The two sides' own bounds parted them.
						const std::string leftAi = left.aiAt.contains(frame) ? left.aiAt.at(frame) : "", rightAi = right.aiAt.contains(frame) ? right.aiAt.at(frame) : "";
						const auto seatsOf = [](const std::string& list) { uint32_t mask = 0; for (char c: list) if (c >= '1' && c <= '9') mask |= 1U << (c - '1'); return mask; };
						const uint32_t leftHeld = seatsOf(leftAi), rightHeld = seatsOf(rightAi);
						const bool rightHolds = (l & ~r) != 0 || (rightHeld & ~leftHeld) != 0, leftHolds = (r & ~l) != 0 || (leftHeld & ~rightHeld) != 0;
						if (left.shownAt.contains(frame) && right.shownAt.contains(frame) && (rightHolds || leftHolds)) {
							std::string late;
							if (rightHolds) late = hub.LateTraffic(b, frame, right.shownAt.at(frame));
							if (late.empty() && leftHolds) late = hub.LateTraffic(a, frame, left.shownAt.at(frame));
							if (!late.empty()) {
								lateSplits.push_back({a, b, 0, frame, hub.now, 0});
								sawSplit = splitHistories = true;
								DiagnosticLine() << "[session-plane-late-input-split] frame=" << frame << " a=" << unsigned(a) << " b=" << unsigned(b) << " members=" << l << "/" << r
								    << " ai=" << (leftAi.empty() ? "none" : leftAi) << "/" << (rightAi.empty() ? "none" : rightAi) << " " << late << " lost=0" << std::endl;
								unexplained[a][b] = {};
								continue;
							}
						}
						// The verdict on this frame waits for an input or receipt still in flight to land, then judges it as before.
						auto& waiting = unexplained[a][b];
						if (waiting.first != frame) {
							waiting = {frame, hub.now};
							if (NetPeerFrameDetail::ReturnInputDetails()) {
								std::string seen;
								for (const auto& [at, observation]: hub.receiptEvidence) if (at + 8 >= frame && at <= frame + 8)
									seen += " receipt@" + std::to_string(at) + ":" + std::to_string(observation.packet.senderPeerId) + ">" + std::to_string(observation.receiver) +
									        "/succ" + std::to_string(observation.packet.successorPeerId) + (observation.impaired ? "/impaired" : "") + "@" + std::to_string(observation.at);
								DiagnosticLine() << "[session-plane-unexplained] frame=" << frame << " a=" << unsigned(a) << " b=" << unsigned(b) << " members=" << l << "/" << r
								    << " authors=" << unsigned(authorA) << "/" << unsigned(authorB) << " shown=" << (left.shownAt.contains(frame) ? left.shownAt.at(frame) : 0) << "/"
								    << (right.shownAt.contains(frame) ? right.shownAt.at(frame) : 0) << " ai=" << (left.aiAt.contains(frame) ? left.aiAt.at(frame) : "none") << "/"
								    << (right.aiAt.contains(frame) ? right.aiAt.at(frame) : "none") << seen << std::endl;
							}
						}
						if (!finalVerdict && hub.now < waiting.second + c_LateEvidenceMs) { compared[a][b] = frame - 1; awaitingEvidence = true; break; }
						// Section 11.1: once no late traffic explains the frame, a seat that plays in one group with a seat proven apart from this pair's
						// other side holds what its group holds, and so is apart from that side by the same proven cause.
						const auto apart = [&](uint8_t x, uint8_t y) {
							const uint8_t low = std::min(x, y), high = std::max(x, y);
							return (splitFrom[low][high] && splitFrom[low][high] <= frame) || (pairSeparatedFrom[low][high] && pairSeparatedFrom[low][high] <= frame);
						};
						bool inherited = false;
						if (const auto [third, far] = inheritedApart[a][b]; third && apart(third, far)) inherited = true;
						if (authors) for (uint8_t third = 1; third <= hub.row.seats && !inherited; ++third) {
							if (third == authorA || third == authorB) continue;
							for (const auto& [near, far, members]: {std::tuple{authorA, authorB, l}, std::tuple{authorB, authorA, r}}) {
								if (!(members & (1U << (near - 1))) || !(members & (1U << (third - 1))) || frameMembers[third - 1].size() <= frame ||
								    !(frameMembers[third - 1][frame] & (1U << (near - 1))) || !apart(third, far)) continue;
								inheritedApart[a][b] = {third, far}; inherited = true; break;
							}
						}
						if (inherited) { splitHistories = true; unexplained[a][b] = {}; continue; }
						error = "histories differ at displayed frame " + std::to_string(frame) + ": peer " + std::to_string(a) + " members=" + std::to_string(l) +
						    " peer " + std::to_string(b) + " members=" + std::to_string(r) + "; authority ruling requires equal history within a half and after image return" +
						    "; now_ms=" + std::to_string(hub.now) + " first_seen_ms=" + std::to_string(waiting.second) + " live_split=" + std::to_string(hub.LiveSplit(a, b)) + " separated_frame=" + std::to_string(separated) +
						    " authors=" + std::to_string(authorA) + "/" + std::to_string(authorB) + " ai=" + (left.aiAt.contains(frame) ? left.aiAt.at(frame) : "none") + "/" + (right.aiAt.contains(frame) ? right.aiAt.at(frame) : "none");
						return false;
					}
					if (!awaitingEvidence) compared[a][b] = common;
				}
				uint64_t floor = UINT64_MAX;
				for (uint8_t a = 1; a <= hub.row.seats; ++a) for (uint8_t b = a + 1; b <= hub.row.seats; ++b) floor = std::min(floor, compared[a][b]);
				if (floor != UINT64_MAX) hub.inputEvidence.erase(hub.inputEvidence.begin(), hub.inputEvidence.upper_bound(floor));
				if (floor != UINT64_MAX) hub.receiptEvidence.erase(hub.receiptEvidence.begin(), hub.receiptEvidence.upper_bound(floor));
				return true;
			}
			// The round is under way once every seat has displayed its first second; waits before that are the start's, not a hold's.
			uint64_t settledAt = 0, startupWaitMs = 0, roundStartMs = 0;
			std::vector<std::pair<uint64_t, uint64_t>> startups; //!< Each round's start, to the moment every seat displayed its first second.
			bool InStartup(uint64_t at) const {
				for (const auto& [from, to]: startups) if (at >= from && at < to) return true;
				return at >= roundStartMs && (startups.empty() || startups.back().first != roundStartMs);
			}
			bool Start(const Case& row, std::string& error) {
				hub.row = row; hub.faultEnd = hub.faultAt + (row.steadyMs ? row.steadyMs : row.gapMs);
				// Image donors retain their actual committed records. A snapshot never replaces a survivor's world.
				const bool maySplit = hub.schedule ? !hub.schedule->cuts.empty() || std::any_of(hub.schedule->spells.begin(), hub.schedule->spells.end(),
				    [](const Spell& spell) { return spell.kind == Spell::Kind::Silent || spell.kind == Spell::Kind::Hung; }) :
				    row.tieBeforeAdministration || (row.gapMs && !row.frozen && !row.slowPercent && !row.inputsLost && !row.keepInputEvery);
				// FLAGGED fixture departure: a stopped machine can require the same automatic image return as a split link.
				if (maySplit || row.imageReturn || row.gapMs) { hub.row.captureFrames = true; hub.row.checkpointEveryFrames = 60; }
				worlds.resize(row.seats); for (auto& world: worlds) world.sharedDigests = &sharedDigests; committed.resize(row.seats); credits.resize(row.seats); queued.resize(row.seats); carriedCommands.assign(row.seats, {}); waitAt.resize(row.seats); releasedAt.assign(row.seats, 0);
				maxWait.resize(row.seats); caughtAt.resize(row.seats); settledDelay.resize(row.seats); settledChanges.resize(row.seats);
				displayedAt.resize(row.seats); blackoutGap.resize(row.seats); returnedAt.resize(row.seats); postReturnWait.resize(row.seats); seenAI.resize(row.seats);
				imageAt.resize(row.seats);
				frameMembers.assign(row.seats, {0}); frameAuthors.assign(row.seats, {0}); installedBoundaries.resize(row.seats); discardedHistories.resize(row.seats);
				waitPlaying.resize(row.seats); playingMaxWait.resize(row.seats); imageReading.resize(row.seats); imageNeeded.resize(row.seats);
				imageWithoutHeadroom.resize(row.seats); splitDecisions.resize(row.seats);
				imageTasks.clear(); imageTasks.resize(row.seats);
				splitParticipant.resize(row.seats); hungLastStep.assign(row.seats, false); observedSplit.resize(row.seats);
				lostAt.resize(row.seats); lostFrame.resize(row.seats); lossServedAt.resize(row.seats); imageReturns.resize(row.seats); decisionsRead.resize(row.seats);
				lostRuling.resize(row.seats);
				firstDisplayedAt.resize(row.seats); startupFinishAt.resize(row.seats); preFaultPaceLeadMs.resize(row.seats);
				settledHostWaits.resize(row.seats); paceBegin.resize(row.seats); paceEnd.resize(row.seats);
				firstAdminChangeMs.assign(row.seats, UINT64_MAX);
				heldSpans.assign(row.seats + 1, {}); imageStarts.assign(row.seats + 1, {}); heldSince.assign(row.seats + 1, UINT64_MAX); heldByObserver = {};
				for (uint8_t peer = 1; peer <= row.seats; ++peer) { wires.push_back(std::make_unique<Wire>(hub, peer)); peers.push_back(std::make_unique<NetLockstepCoordinator>()); }
				if (!wires[0]->StartHost(47901, &error)) return false;
				for (uint8_t peer = 2; peer <= row.seats; ++peer) if (!wires[peer - 1]->Connect("fixture", 47901, &error)) return false;
				if (row.rosterStage) {
					agreedRoster.matchId = 0xF001; agreedRoster.roundNo = 77; agreedRoster.revision = 1;
					for (uint8_t peer = 1; peer <= row.seats; ++peer) {
						NetRosterSeat seat; seat.seatId = peer; seat.owner = peer; seat.ticket = 100 + peer; seat.incarnation = 1;
						agreedRoster.seats.push_back(seat);
					}
					NetRosterEvent formed; formed.kind = NetRosterEventKind::RematchFormed;
					auto result = ApplyRosterEvent(agreedRoster, formed);
					if (result.refused) { error = result.reason; return false; }
					agreedRoster = std::move(result.roster);
					if (row.rosterStage == 2) {
						NetRosterEvent placed; placed.kind = NetRosterEventKind::PlacementStarted;
						auto placement = ApplyRosterEvent(agreedRoster, placed);
						if (placement.refused) { error = placement.reason; return false; }
						agreedRoster = std::move(placement.roster);
					}
				}
				return StartPeers(77, error);
			}
			// Starts every seat's round on the fixture's links: the first round, or a rematch's next.
			bool StartPeers(uint64_t roundId, std::string& error) {
				const Case& row = hub.row;
				auto match = NetMatchConfigUtil::MakeDefault(0xF001);
				match.peerCount = row.seats; match.players.clear(); match.successorOrder.clear(); match.migrationPeers.clear();
				match.inputDelayFrames = 1; match.peerInputDelayFrames.assign(row.seats, 1); match.roundId = roundId;
				if (row.mode >= 2) match.dedicated = true;
				if (row.mode == 3) {
					match.version = NetMatchConfigUtil::c_PersistentWorldVersion;
					match.persistentWorld = true; match.worldId = "01234567-89ab-cdef-0123-456789abcdef"; match.worldBoot = 1;
				}
				for (uint8_t peer = 1; peer <= row.seats; ++peer) {
					uint32_t rtt = 0;
					for (uint8_t other = 1; other <= row.seats; ++other) if (peer != other) rtt = std::max<uint32_t>(rtt, static_cast<uint32_t>(2 * hub.Delay(peer, other)));
					NetInputDelayEstimator estimator; estimator.Observe(0, rtt);
					match.peerInputDelayFrames[peer - 1] = InitialSenderDelay(estimator, peer == 1, match.inputDelayFrames);
				}
				for (uint8_t peer = 1; peer <= row.seats; ++peer) {
					if (!match.dedicated || peer != 1) match.players.push_back({peer, static_cast<uint8_t>(peer - 1), false, "Player"});
					if (!match.dedicated) {
						if (peer != 1) match.successorOrder.push_back(peer);
						match.migrationPeers.push_back({peer, static_cast<uint16_t>(47900 + peer), {"fixture"}});
					}
				}
				if (row.mode && !NetMatchConfigUtil::ValidateLocalAlpha(match, &error)) return false;
				hub.directory.configHash = NetMatchConfigUtil::HashConfig(match);
				for (uint8_t peer = 1; peer <= row.seats; ++peer) {
					NetLockstepConfig config; config.sessionId = match.sessionId; config.matchConfig = match;
					config.localPeerId = peer; config.peerCount = row.seats; config.roundId = peer == 1 ? roundId : 0;
					config.startFrame = 1; config.inputDelayFrames = match.peerInputDelayFrames[peer - 1]; config.timeoutMs = 60000;
					// FLAGGED fixture departure: Main/NetMatchRunner publish activity startup before agreeing the cold round.
					// Starting each seat on its last individual handshake can put a client ahead of the host's pace reference.
					config.requirePublishedStart = true;
					config.frameLane = NetTransportLane::InputUnreliable; config.substituteSlowPeers = match.slowPlayerPolicy == NetSlowPlayerPolicy::Substitute; config.adaptiveInputDelay = true;
					config.slowPlayerBoundTicks = c_TestBoundTicks; config.simTickMs = c_TestTickMs; config.relayToOtherPeers = peer == 1;
					config.migrationKey.fill(0x39);
					for (uint8_t other = 1; other <= row.seats; ++other) { config.peerInputDelayFrames[other] = match.peerInputDelayFrames[other - 1]; config.peerIncarnations[other] = 1; }
					if (peer == 1) for (uint8_t other = 2; other <= row.seats; ++other) config.remoteTransportPeerIds[other] = other - 1;
					else config.remoteTransportPeerIds[1] = 1;
					config.migrationTransportFactory = [&hub = hub, peer] { return std::make_unique<Wire>(hub, peer); };
					FrameGroupConfig(config, hub, peer);
					if (row.rosterStage) peers[peer - 1]->SetRosterReader([this] { return &agreedRoster; });
					if (!peers[peer - 1]->Start(*wires[peer - 1], config, &error)) return false;
					peers[peer - 1]->DeferStopsToTickBoundary();
					// FLAGGED fixture departure: measured loading finishes before its startup publication, as in Main.
					const uint32_t loadingMs = row.delayStartup ? (peer - 1) * 70 : 0;
					startupFinishAt[peer - 1] = loadingMs ? hub.now + loadingMs : 0;
					if (!loadingMs) peers[peer - 1]->NoteLocalStartPark(0);
				}
				return true;
			}
			bool HasSplitHistoryAt(uint8_t peer, uint64_t frame) const {
				if (peer == 0 || peer > hub.row.seats) return false;
				for (uint8_t other = 1; other <= hub.row.seats; ++other) {
					const auto a = std::min(peer, other), b = std::max(peer, other);
					if (splitFrom[a][b] && splitFrom[a][b] <= frame) return true;
					// FLAGGED fixture departure: a proven input partition discards its losing history just as a total cut does.
					const auto after = inputSplits[a][b].upper_bound(frame);
					if (after != inputSplits[a][b].begin() && (!std::prev(after)->second || frame <= std::prev(after)->second)) return true;
					// Histories that already differ are split while their cause is still being judged, or while a late input's split is arbitrated.
					if ((unexplained[a][b].first && unexplained[a][b].first <= frame) || (OpenLateSplit(a, b) && OpenLateSplit(a, b)->frame <= frame)) return true;
				}
				return false;
			}
			bool IsLosingDecision(uint8_t seat, const NetFrameTieRequest& request, const NetFrameTieReply& reply) const {
				if (seat == 0 || seat > worlds.size()) return false;
				return IsLosingContinuationDecision(HasSplitHistoryAt(seat, worlds[seat - 1].applied), seat, request, reply);
			}
			bool IsAffected(uint8_t peer) const {
				const bool second = (hub.row.secondSubject && peer == hub.row.secondSubject) || (hub.row.silentVoter && peer == hub.row.silentVoter);
				return peer == hub.row.subject || second || peer == hub.row.lateReceiptsFrom;
			}
			bool CheckCommittedInputs(const NetLockstepReadyFrame& ready, uint8_t observer, std::string& error) {
				// The 2026-10-17 ruling replaces the old two-second repeated-human-input interval: the installed hold is AI/empty now.
				const auto& peer = *peers[observer - 1];
				for (const auto& [seat, changes]: s_HoldBoundaries(peer)) {
					const auto after = changes.upper_bound(ready.frame);
					if (after == changes.begin() || !std::prev(after)->second) continue;
					if (!peer.IsSeatUnderAI(seat, ready.frame)) {
						error = "authority ruling: peer " + std::to_string(observer) + " installed seat " + std::to_string(seat) + "'s hold at " +
						    std::to_string(std::prev(after)->first) + " but frame " + std::to_string(ready.frame) + " is not AI-owned; the old two-second continuity interval is withdrawn";
						return false;
					}
					size_t inputs = ready.localPeerId == seat ? ready.localFrames.size() : 0;
					if (ready.localPeerId != seat) for (const auto& [sender, count]: ready.remoteFrameCounts) if (sender == seat) inputs += count;
					if (inputs) {
						error = "authority ruling: a held incarnation supplied human input at frame " + std::to_string(ready.frame) + "; HoldAtFrame requires empty input: observer=" +
						    std::to_string(observer) + " seat=" + std::to_string(seat) + " hold_frame=" + std::to_string(std::prev(after)->first);
						return false;
					}
					++aiFrames; ++substitutes;
					firstLostPressMs = std::min(firstLostPressMs, hub.now >= hub.faultAt ? hub.now - hub.faultAt : 0);
				}
				return true;
			}
			bool Step(std::string& error) {
				if (hub.row.realFrozen && hub.DuringFault()) {
					if (!freezeStarted) {
						freezeStarted = true; freezeWallMs = NetLockstepNowMs();
						if (onFreeze) onFreeze(*peers[hub.row.subject - 1], *wires[hub.row.subject - 1]);
					}
					// The actual background sender runs while this seat's main loop
					// does no simulation or plane work for the entire ten seconds.
					while (NetLockstepNowMs() - freezeWallMs < hub.now - hub.faultAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				} else if (freezeStarted && !thawed) { if (onThaw) onThaw(); thawed = true; }
				std::optional<NetLockstepPlaneGuard> plane;
				if (!parallel && !t_PoolWorker) plane.emplace();
				std::lock_guard lock(hub.mutex);
				const Schedule* const scheduled = hub.schedule;
				const auto hung = [&](uint8_t id) { return hub.ProcessHung(id); };
				const auto stopped = [&](uint8_t id) { return scheduled && scheduled->Is(id, hub.now, Spell::Kind::Stopped); };
				if (hub.row.adminDirectory && hub.now == hub.faultAt - 1) {
					// Put the last authenticated host traffic at the silence lever's
					// boundary, rather than at the preceding 250 ms heartbeat slot.
					std::vector<uint8_t> heartbeat;
					if (!NetProtocol::Encode({static_cast<uint32_t>(hub.now), 0, NetHeartbeat{hub.now, 0, 3}}, heartbeat, nullptr)) return false;
					for (uint8_t id = 2; id <= hub.row.seats; ++id)
						peers[id - 1]->InjectEvent({NetTransportEventType::PacketReceived, 1, NetTransportLane::ControlReliable, heartbeat, {}}, hub.now);
				}
				// A stopped main loop polls the directory too, from the session's liveness thread.
				if (scheduled && scheduled->leaver && hub.now == scheduled->leaveAtMs) peers[scheduled->leaver - 1]->Leave("Match left");
				// A kick: the host removes the seat and its player's links close; the directory drops its lease an exchange later.
				if (scheduled && scheduled->kicked && hub.now == scheduled->kickAtMs) {
					peers[0]->EvictRemovedPeer(scheduled->kicked, "removed from the session", hub.now);
					for (auto* wire: std::vector<Wire*>(hub.wires.begin(), hub.wires.end())) if (wire->owner == scheduled->kicked) wire->Stop();
				}
				if (scheduled && scheduled->kicked && hub.now == scheduled->kickAtMs + hub.DirectoryExchangeMs(1)) { hub.directory.removed.insert(scheduled->kicked); std::erase(hub.directory.retained, scheduled->kicked); }
				// A rematch: the host ends the round, and 300 ms later every seat starts the next one on the same links and directory clients.
				const bool between = scheduled && scheduled->rematchAtMs && hub.now >= scheduled->rematchAtMs && hub.now < scheduled->rematchAtMs + 300;
				if (scheduled && scheduled->rematchAtMs && hub.now == scheduled->rematchAtMs) peers[0]->Complete("round ended");
				if (scheduled && scheduled->rematchAtMs && hub.now == scheduled->rematchAtMs + 300) {
					// A return never dies with its round: a seat still returning stays held into the next round, whose start offers it its
					// seat again, and the return property judges the hold the round's end closes.
					if (earlierRoundRule.empty()) earlierRoundRule = TwoReturnFrames();
					for (auto& peer: peers) peer = std::make_unique<NetLockstepCoordinator>();
					for (uint8_t id = 1; id <= hub.row.seats; ++id) {
						worlds[id - 1] = World{}; worlds[id - 1].sharedDigests = &sharedDigests; credits[id - 1].Reset(); queued[id - 1] = 0; waitAt[id - 1] = releasedAt[id - 1] = 0;
						if (heldSince[id] != UINT64_MAX) { heldSpans[id].push_back({heldSince[id], hub.now}); heldSince[id] = UINT64_MAX; }
					}
					hashedThrough = 0; sharedDigests.clear(); roundStartMs = hub.now;
					if (const auto unresolved = UnresolvedLateSplits(); !unresolved.empty()) { error = unresolved + " before the round ended"; return false; }
					compared = {}; splitFrom = {}; inputSplits = {}; inputSplitTimes = {}; inputSplitCauses.clear(); partitionOwners = {}; earlierLateSplits.insert(earlierLateSplits.end(), lateSplits.begin(), lateSplits.end()); lateSplits.clear(); unexplained = {}; sawSplit = splitHistories = false;
					imageTasks.clear(); imageTasks.resize(hub.row.seats);
					// As the service's tail log, the committed record starts again with the round: frame numbers restart.
					committed.assign(hub.row.seats, {}); for (auto& donors: abandonedDonors) donors.clear(); returnActivations = {}; carriedCommands.assign(hub.row.seats, {});
					directRuling = 0;
					std::fill(lostAt.begin(), lostAt.end(), 0); std::fill(lostFrame.begin(), lostFrame.end(), 0); std::fill(lossServedAt.begin(), lossServedAt.end(), 0);
					std::fill(lostRuling.begin(), lostRuling.end(), 0);
					std::fill(imageAt.begin(), imageAt.end(), 0); std::fill(imageReturns.begin(), imageReturns.end(), 0);
					std::fill(imageNeeded.begin(), imageNeeded.end(), false);
					std::fill(imageReading.begin(), imageReading.end(), false); std::fill(imageWithoutHeadroom.begin(), imageWithoutHeadroom.end(), false);
					frameMembers.assign(hub.row.seats, {0}); frameAuthors.assign(hub.row.seats, {0}); installedBoundaries.assign(hub.row.seats, {});
					// The wire's records count frames, which start again: the round that ended is judged on them first.
					hub.bridgeBoundaries.clear(); hub.inputSentThrough.clear(); hub.returnsProposed.clear(); hub.inputEvidence.clear(); hub.receiptEvidence.clear();
					if (!StartPeers(78, error)) return false;
				}
				const auto gone = [&](uint8_t id) { return scheduled && scheduled->Left(id, hub.now); };
				for (uint8_t a = 1; a <= hub.row.seats; ++a) for (uint8_t b = a + 1; b <= hub.row.seats; ++b)
					if (hub.LiveSplit(a, b)) ObserveSplit(a, b);
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
					uint32_t cut = 0;
					for (uint8_t other = 1; other <= hub.row.seats; ++other) if (hub.LiveSplit(seat, other)) cut |= 1U << (other - 1);
					if (cut != observedSplit[seat - 1]) {
						observedSplit[seat - 1] = cut;
						// A seat whose cut appears as its own hung process resumes was away when the split began: it never played a half of it.
						splitParticipant[seat - 1] = cut && peers[seat - 1]->IsRunning() && !CatchingUp(*peers[seat - 1]) &&
						    (s_FrameMembers(*peers[seat - 1], worlds[seat - 1].applied + 1) & (1U << (seat - 1))) && !hungLastStep[seat - 1];
					}
					if (OwnMachineTrouble(seat)) splitParticipant[seat - 1] = false;
					hungLastStep[seat - 1] = hub.ProcessHung(seat);
				}
				// Direct arbitration is defined only at the heal. The oracle ranks the actually committed groups, never transport silence.
				if (!hub.row.internet && !scheduled && sawSplit && !hub.DuringFault() && !directRuling) directRuling = RankDirectHistories();
				for (uint8_t id = 1; id <= hub.row.seats; ++id) if (!hung(id) && !gone(id)) hub.PollDirectory(id);
				for (uint8_t id = 1; id <= hub.row.seats; ++id) {
					if (startupFinishAt[id - 1] && hub.now >= startupFinishAt[id - 1]) {
						peers[id - 1]->NoteLocalStartPark((id - 1) * 70);
						startupFinishAt[id - 1] = 0;
					}
					if ((hub.now + id * 3) % hub.row.planeIntervalMs != 0) continue;
					if (hub.row.realFrozen && id == hub.row.subject && hub.DuringFault()) continue;
					if (hung(id) || gone(id)) continue;
					// Between rounds every seat's runner has left the round: its coordinator stops with its simulation.
					if (between) continue;
					if (stopped(id) || (hub.row.frozen && id == hub.row.subject && hub.DuringFault())) peers[id - 1]->PlaneTick(hub.now);
					else peers[id - 1]->Tick(hub.now);
				}
				if (hub.row.inputsLost) hub.dropInputsFrom = hub.DuringFault() ? hub.row.subject : 0;
				if (hub.row.joinerInputsLostFirstMs && hub.now == hub.faultAt - hub.row.joinerInputsLostFirstMs) hub.dropInputsFrom = 2;
				if (hub.row.joinerInputsLostFirstMs && hub.now == hub.faultAt) hub.dropInputsFrom = 0;
				if (hub.row.blackout && hub.now == hub.faultAt) {
					Blackout(*peers[hub.row.subject - 1], worlds[hub.row.subject - 1].applied, hub.row.gapMs, hub.row.internet && !hub.row.directoryBoth);
					if (!HasBlackout<NetLockstepCoordinator>()) hub.row.blackout = false;
				}
				for (uint8_t id = 1; id <= hub.row.seats; ++id) {
					auto& peer = *peers[id - 1]; auto& world = worlds[id - 1];
					if (gone(id)) { leaverStopped |= peer.IsStopped(); continue; }
					if (between) { waitAt[id - 1] = releasedAt[id - 1] = 0; continue; }
					if (peer.IsFailed()) { error = "peer " + std::to_string(id) + " failed: " + peer.GetStats().timeoutReason; return false; }
					const bool directHostlessGroup = !hub.row.internet && sawSplit && (s_HeldSeats(peer, world.applied + 1) & 1U);
					const bool keptDirectMigration = !hub.row.internet && sawSplit && peer.GetConfig().migrationGeneration &&
					    std::any_of(firstAdminChangeMs.begin(), firstAdminChangeMs.end(), [](uint64_t at) { return at != UINT64_MAX; });
					// Section 11.3(c): a direct group, a lone peer included, that has held the host through the hold window migrates and keeps the
					// game. A scheduled round's window runs from when the host's seat was first held, not from a fixed row's fault.
					// The host's silence began up to one link delay and the bound before the fixture saw its seat held.
					const auto heldThroughWindow = [&](uint64_t since, uint64_t until) {
						return until >= since && until - since + hub.Delay(1, id) + static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs)) >= c_NetHostLossSilenceMs;
					};
					bool heldHostWindow = hub.schedule && heldSince[1] != UINT64_MAX && heldThroughWindow(heldSince[1], hub.now);
					if (hub.schedule) for (const auto& [since, until]: heldSpans[1]) heldHostWindow |= until != UINT64_MAX && heldThroughWindow(since, until);
					// The round's held view waits out an observer's own spell; the migrating seat's engine counts from the hold it installed itself.
					if (hub.schedule && heldByObserver[id][1]) heldHostWindow |= heldThroughWindow(heldByObserver[id][1], hub.now);
					const bool scheduledDirectMigration = !hub.row.internet && heldHostWindow && peer.GetConfig().migrationGeneration;
					// The revised direct contract also migrates a hostless minority and a lone peer, on their own continuation.
					if (!hub.row.repeatedAdministration && !directHostlessGroup && !keptDirectMigration && !scheduledDirectMigration && peer.GetHostPeerId() != 1 &&
					    (!hub.row.gapMs || hub.row.subject != 1 || hub.row.gapMs < 15000)) {
						error = "lag changed the lobby host (host seat held since " + (heldSince[1] == UINT64_MAX ? std::string("never") : std::to_string(heldSince[1])) +
						    " ms, migration generation " + std::to_string(peer.GetConfig().migrationGeneration) + ", peer " + std::to_string(id) + ")"; return false;
					}
					if (!hub.row.repeatedAdministration && peer.GetHostPeerId() != 1 && firstAdminChangeMs[id - 1] == UINT64_MAX) {
						firstAdminChangeMs[id - 1] = hub.now;
						if (hub.schedule ? !heldHostWindow : hub.now + hub.Delay(1, id) < hub.faultAt + 15000) {
							error = "admin host changed before fifteen seconds of authenticated silence"; return false;
						}
					}
					if (peer.IsStopped() && scheduled && peer.GetStats().timeoutReason.starts_with("Complete:")) {
						bool alone = true;
						for (uint8_t other = 1; other <= hub.row.seats; ++other) alone &= other == id || gone(other);
						// A round that ended while this seat waited for its image owes it no return.
						if (alone) { roundOver = true; imageNeeded[id - 1] = imageReading[id - 1] = false; continue; }
					}
					for (const auto& [seat, boundaries]: s_HoldBoundaries(peer))
						for (const auto& [frame, held]: boundaries) installedBoundaries[id - 1][seat][frame] = held;
					if (const auto client = hub.directoryClients.find(id); client != hub.directoryClients.end()) {
						const auto& decisions = client->second.consumed;
						for (auto& read = decisionsRead[id - 1]; read < decisions.size(); ++read) {
							const auto& decision = decisions[read];
							const auto key = std::make_tuple(decision.request.generation, decision.request.roundId, decision.request.configHash, decision.request.frame);
							// A ruling for a continuation whose every player this seat has heard leave names no machine to return to: it plays on.
							const uint32_t playing = s_FrameMembers(peer, world.applied);
							// One seat decides for its continuation how it takes a ruling - the round's host when it plays there, else the lowest
							// seat - and every other seat of it takes that decision when it arrives, so only the deciding seat stops on reading it.
							const uint8_t host = peer.GetHostPeerId();
							const uint8_t decider = (playing & (1U << (host - 1))) ? host : static_cast<uint8_t>(std::countr_zero(playing) + 1);
							bool winnersElsewhere = false, winnersLeft = true;
							for (const uint16_t winner: decision.reply.members) {
								if (winner == 0 || winner > hub.row.seats || (playing & (1U << (winner - 1))) != 0) continue;
								winnersElsewhere = true;
								winnersLeft &= hub.schedule && hub.schedule->Left(static_cast<uint8_t>(winner), hub.now) && peer.HasHeardLeave(static_cast<uint8_t>(winner));
							}
							winnersLeft &= winnersElsewhere;
							if (IsLosingDecision(id, decision.request, decision.reply) && !winnersLeft && decider == id &&
							    splitDecisions[id - 1].insert(key).second) {
								if (NetPeerFrameDetail::ReturnInputDetails()) DiagnosticLine() << "[session-plane-decision-class] seat=" << unsigned(id)
								    << " query_only=" << decision.request.queryOnly << " question=" << decision.request.frame << " now_ms=" << hub.now
								    << " image_reading=" << imageReading[id - 1] << " image_returns=" << imageReturns[id - 1]
								    << " displayed=" << world.applied << " resume=" << peer.GetResumeFrame() << std::endl;
								// FLAGGED fixture departure: preparation can lead the world by hundreds of ticks. The ruling's decision
								// boundary is the actual completed cursor, as in the direct oracle; never compare a stop to prepared input.
								lostAt[id - 1] = hub.now; lostFrame[id - 1] = peer.GetResumeFrame(); imageNeeded[id - 1] = true;
								for (uint16_t member: decision.reply.members) lostRuling[id - 1] |= 1U << (member - 1);
							}
						}
					}
					if (peer.IsStopped()) {
						// A losing half stops at its decision boundary. Its subsequent replay is private; survivors never stop for it.
						const bool directLoser = !hub.row.internet && directRuling && !(directRuling & (1U << (id - 1)));
						const bool imageStop = peer.IsLocalSeatHeld() && peer.GetStats().timeoutReason.starts_with("PeerHeld:");
						if (imageStop && directLoser && !lostAt[id - 1]) {
							lostAt[id - 1] = hub.now; lostFrame[id - 1] = peer.GetResumeFrame();
						}
						// A seat that follows its continuation's deciding seat stops on that seat's ruling, never on one the deciding seat stood on.
						if (imageStop && hub.row.internet && !lostAt[id - 1] && peer.GetStats().timeoutReason.find("selected the other split") != std::string::npos) {
							const uint32_t playing = s_FrameMembers(peer, world.applied);
							const uint8_t host = peer.GetHostPeerId();
							const uint8_t decider = (playing & (1U << (host - 1))) ? host : static_cast<uint8_t>(std::countr_zero(playing) + 1);
							if (decider != id) {
								if (!lostAt[decider - 1] && !imageNeeded[decider - 1] && !imageReading[decider - 1]) {
									error = "authority ruling: peer " + std::to_string(id) + " stopped on a ruling its continuation's deciding seat " + std::to_string(decider) + " did not stop on"; return false;
								}
								if (lostAt[decider - 1]) { lostAt[id - 1] = hub.now; lostFrame[id - 1] = peer.GetResumeFrame(); lostRuling[id - 1] = lostRuling[decider - 1]; }
							}
						}
						// FLAGGED fixture departure: every genuine held stop enters recovery, including a case's other affected seats.
						if (imageStop) {
							if (lostAt[id - 1] && lostAt[id - 1] != lossServedAt[id - 1] && (peer.GetResumeFrame() != lostFrame[id - 1] || world.applied + 1 != lostFrame[id - 1])) {
								error = "authority ruling: losing peer " + std::to_string(id) + " stopped away from its engine decision frame " + std::to_string(lostFrame[id - 1]); return false;
							}
							FinishWait(id, world.applied + 1);
							if (!imageAt[id - 1] || !imageNeeded[id - 1]) imageAt[id - 1] = hub.now;
							imageNeeded[id - 1] = true;
							continue;
						}
						error = "lag ended participation or required a manual rejoin: peer " + std::to_string(id) + " stopped: " + peer.GetStats().timeoutReason; return false;
					}
					if (lostAt[id - 1] && imageNeeded[id - 1] && !imageReading[id - 1]) {
						error = "authority ruling: losing peer " + std::to_string(id) + " did not stop at decision frame " + std::to_string(lostFrame[id - 1]); return false;
					}
					if (!peer.IsRunning() || peer.AwaitingAlignedStart() || imageReading[id - 1]) continue;
					const uint64_t boundNext = world.applied + 1;
					// As the engine reads it, a consumer catching up with a hold of its own seat already pending is the seat its round holds: its
					// playing portion ends there, as a private reader's does.
					ObserveBoundRole(id - 1, !peer.WaitsForPlacement() && !peer.IsLocalPrivateReaderAt(boundNext) && !peer.IsSeatUnderAI(id, boundNext) &&
					    (s_FrameMembers(peer, boundNext) & (1U << (id - 1))) && !(CatchingUp(peer) && s_OwnHoldPending && s_OwnHoldPending(peer, boundNext)));
					if (splitParticipant[id - 1] && observedSplit[id - 1] && !imageNeeded[id - 1] &&
					    (!(s_FrameMembers(peer, world.applied + 1) & (1U << (id - 1))) || peer.IsLocalPrivateReaderAt(world.applied + 1))) {
						error = "authority ruling: playing split peer " + std::to_string(id) + " became a private reader before a losing decision; a pending referee never suspends its half"; return false;
					}
					if ((hub.row.frozen && id == hub.row.subject && hub.DuringFault()) || hung(id) || stopped(id)) { credits[id - 1].Reset(); continue; }
					const Spell* const spell = scheduled ? scheduled->Active(id, hub.now) : nullptr;
					const bool refault = hub.row.refaultPercent && id == hub.row.subject && hub.InRefault();
					const bool slowed = refault || (hub.row.slowPercent && id == hub.row.subject && hub.DuringFault()) || (spell && spell->kind == Spell::Kind::Slow);
					const uint32_t ratePercent = spell ? spell->percent : refault ? hub.row.refaultPercent : hub.row.slowPercent;
					const uint32_t capacity = scheduled ? scheduled->CapacityPercent(id, hub.now) : 0;
					const bool atRoundRate = id == hub.row.subject && hub.row.roundRateAfterMs && hub.now >= hub.faultEnd + hub.row.roundRateAfterMs;
					const double catchUpRate = scheduled ? capacity / 100.0 : atRoundRate ? 1.0 : hub.row.catchUpPercent && id == hub.row.subject ? hub.row.catchUpPercent / 100.0 : c_TestCatchUpRate;
					credits[id - 1].Accrue(hub.row.stepMs, slowed ? ratePercent / 100.0 : 1, catchUpRate, !slowed && CatchUpNeeded(peer));
					while (credits[id - 1].MayGrant(!slowed && CatchUpNeeded(peer), !slowed && CatchingUp(peer, credits[id - 1].NextOrdinaryTickInMs(hub.row.stepMs)))) {
						const bool catchingUp = !slowed && CatchUpNeeded(peer);
						const uint64_t next = world.applied + 1;
						if (queued[id - 1] != next && !peer.TimingDecisionPendingAt(next)) {
							ControllerFrame input; input.actorUniqueID = 1000 + id; input.analogMoveX = id * 100;
							input.stateMask = (1ULL << MOVE_RIGHT) | (1ULL << WEAPON_FIRE) | (1ULL << BODY_JUMP);
							const std::vector<ControllerFrame> controls = peer.GetConfig().matchConfig.dedicated && id == 1 ? std::vector<ControllerFrame>{} : std::vector<ControllerFrame>{input};
							std::vector<NetGameCommand> commands = std::exchange(carriedCommands[id - 1], {});
							if (id == 1 && hub.row.checkpointEveryFrames && next % hub.row.checkpointEveryFrames == 0)
								commands.push_back({1, NetGameCheckpoint{NetGameCheckpoint::Capture, next}});
							// As the runner does, a sample the coordinator defers carries its commands to the seat's next one.
							if (peer.DeferLocalInput(next, controls)) carriedCommands[id - 1] = std::move(commands);
							else if (!peer.QueueLocalInput(next, controls, commands, &error)) return false;
							queued[id - 1] = next;
						}
						NetLockstepReadyFrame ready;
						if (next < peer.GetStats().effectiveStartFrame) { ready.frame = next; ready.localPeerId = id; }
						else if (!peer.PopReadyFrame(ready)) {
							if (!waitAt[id - 1]) {
								waitAt[id - 1] = hub.now;
								// FLAGGED fixture departure: unplaced brains are at a barrier; its no-AI wait is not combat play.
								// Raw waits remain measured, and CombatStarted restores the complete playing bound on these same peers.
								waitPlaying[id - 1] = world.applied >= 60 && !peer.WaitsForPlacement() && !OwnMachineTrouble(id) && !CatchingUp(peer) &&
								    !peer.IsLocalPrivateReaderAt(next) && !peer.IsSeatUnderAI(id, next) &&
								    (s_FrameMembers(peer, next) & (1U << (id - 1)));
							}
							if (peer.NoteFrameWait(next, hub.now) && !releasedAt[id - 1]) releasedAt[id - 1] = hub.now;
							break;
						}
						// The game shows a frame the engine releases inside the update that waited for it: at that tick, as a wait ends.
						const uint64_t shownAt = WaitEnd(id - 1);
						FinishWait(id, next);
						peer.FinishFrameWait(hub.now);
						if (!CheckCommittedInputs(ready, id, error) || !world.Apply(ready, error, hub.now)) return false;
						// As the game does, every thirtieth applied frame's hash goes to the round's live desync check.
						if (ready.frame % 30 == 0) (void)peer.SubmitLocalChecksum(ready.frame, world.hashes.at(ready.frame));
						frameMembers[id - 1].push_back(s_FrameMembers(peer, ready.frame));
						// FLAGGED fixture departure: a held reader copies a witnessed committer's history; it does not author that continuation.
						frameAuthors[id - 1].push_back(s_FrameAuthor(peer, ready));
						if (hub.row.captureFrames) committed[id - 1][ready.frame] = ready;
						peer.FinishSimulationTick(ready.frame); credits[id - 1].Consume(catchingUp);
						if (NetPeerFrameDetail::ReturnInputDetails()) {
							const char* watched = ObservedTargetText();
							const uint64_t target = watched ? std::strtoull(watched, nullptr, 10) : UINT64_MAX;
							if (ready.frame <= 3 || (watched && ready.frame <= target && target - ready.frame <= 16))
								DiagnosticLine() << "[session-plane-display-clock] local=" << unsigned(id) << " frame=" << ready.frame
								    << " now_ms=" << hub.now << " acceleration=" << CatchingUp(peer) << " next=" << peer.GetStats().nextFrame << std::endl;
						}
						// What the tick cost this machine, as the engine reports it: a light tick, or a slow machine's whole share of its rate.
						// A scheduled machine reports its own capacity: a fast one a light tick, one with little headroom its share of the tick.
						peer.NoteLocalTickCost(ready.frame, slowed ? c_TestTickMs * 100.0 / ratePercent : scheduled && capacity < 300 ? c_TestTickMs * 100.0 / capacity : 1.0);
						if (displayedAt[id - 1] && hub.DuringFault()) blackoutGap[id - 1] = std::max(blackoutGap[id - 1], shownAt - std::min(shownAt, displayedAt[id - 1]));
						displayedAt[id - 1] = shownAt;
						if (!firstDisplayedAt[id - 1]) firstDisplayedAt[id - 1] = hub.now;
						if (peer.IsSeatUnderAI(hub.ReturningSeat(), ready.frame)) seenAI[id - 1] = true;
						else if (seenAI[id - 1] && !returnedAt[id - 1]) returnedAt[id - 1] = hub.now;
					}
					if (scheduled) continue;
					if (hub.now < hub.faultEnd && hub.row.gapMs < 5000 && HoldVisible(peer, hub.row.subject, hub.now)) { error = "a short hiccup showed a held seat"; return false; }
					if (hub.row.frozen && HoldVisible(peer, hub.row.subject, hub.now)) { error = "flowing keepalives showed a held seat"; return false; }
					if (hub.now == hub.faultAt - 1) {
						if (NetPeerFrameDetail::ReturnInputDetails()) DiagnosticLine() << "[session-plane-pre-fault] seat=" << unsigned(id) << " applied=" << world.applied << " now_ms=" << hub.now << std::endl;
						preFaultPaceLeadMs[id - 1] = static_cast<int64_t>(std::llround((world.applied - 1) * c_TestTickMs)) -
						    static_cast<int64_t>(displayedAt[id - 1] - firstDisplayedAt[id - 1]);
						settledDelay[id - 1] = peer.InputDelayAt(id, world.applied + 1); settledChanges[id - 1] = SenderResizes(peer, id);
						settledHostWaits[id - 1] = peer.GetStats().peers.contains(1) ? peer.GetStats().peers.at(1).waits : 0;
						paceBegin[id - 1] = world.applied;
					}
					if (hub.now == hub.faultEnd - 1) paceEnd[id - 1] = world.applied;
				}
				if (scheduled) return StepSchedule(error);

				// A seat is held only for its own lateness: one that keeps the round's rate on its own link is never held for another's.
				// The seat in the fault holds what it cannot hear at its own bound (section 11.1): that view is its own fault's.
				if (!hub.row.tie && !hub.row.partition && !sawSplit && wronglyHeld.empty())
					for (uint8_t id = 1; id <= hub.row.seats; ++id) if (!IsAffected(id))
						for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
							if (IsAffected(seat) || seat == hub.ReturningSeat() || !(Bridges(*peers[id - 1]).contains(seat) || peers[id - 1]->HasHeldAISeat(seat))) continue;
							wronglyHeld = "peer " + std::to_string(id) + " held seat " + std::to_string(seat) + " at " + std::to_string(hub.now) + " ms (fault at " +
							              std::to_string(hub.faultAt) + " ms) while only seat " + std::to_string(hub.row.subject) + " lagged";
						}
				const uint64_t common = std::min_element(worlds.begin(), worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied;
				if (!CompareHistories(error)) return false;
				if (hub.row.refaultPercent && hub.refaultAt == UINT64_MAX && hub.now >= hub.faultEnd &&
				    !Bridges(*peers[0]).contains(hub.row.subject) && !peers[0]->HasHeldAISeat(hub.row.subject)) {
					hub.refaultAt = hub.now + hub.row.refaultAfterMs;
					hub.refaultEnd = hub.refaultAt + hub.row.refaultMs;
				}
				if (NetPeerFrameDetail::ReturnInputDetails() && hub.now + 1000 >= hub.faultAt && hub.now < hub.faultEnd && hub.now % 250 == 0) {
					DiagnosticLine line; line << "[session-plane-fault-pace] now_ms=" << hub.now;
					for (uint8_t id = 1; id <= hub.row.seats; ++id) line << " seat" << unsigned(id) << "=" << worlds[id - 1].applied;
				}
				if (hub.now >= hub.faultEnd) {
					// A dedicated host has no seat and relays every input: its clients trail it by that relay, so a returned seat catches up with
					// the seats that play through it, not with the host.
					const bool relayed = peers.front()->GetConfig().matchConfig.dedicated;
					uint64_t highest = 0;
					for (uint8_t id = relayed ? 2 : 1; id <= hub.row.seats; ++id) highest = std::max(highest, worlds[id - 1].applied);
					if (NetPeerFrameDetail::ReturnInputDetails() && (hub.now - hub.faultEnd) % 1000 == 0) {
						DiagnosticLine line; line << "[session-plane-caught-up] now_ms=" << hub.now << " highest=" << highest;
						for (uint8_t id = 1; id <= hub.row.seats; ++id) line << " seat" << unsigned(id) << "=" << worlds[id - 1].applied << (caughtAt[id - 1] ? "+" : "");
					}
					for (uint8_t id = 1; id <= hub.row.seats; ++id) if (!caughtAt[id - 1] && worlds[id - 1].applied + 1 >= highest && common > 0 &&
					    !peers[id - 1]->HasHeldAISeat(hub.ReturningSeat()) && !Bridges(*peers[id - 1]).contains(hub.ReturningSeat()) &&
					    (!hub.row.secondSubject || (!peers[id - 1]->HasHeldAISeat(hub.row.secondSubject) && !Bridges(*peers[id - 1]).contains(hub.row.secondSubject))) &&
					    (!hub.row.silentVoter || (hub.silentAt != UINT64_MAX && !peers[id - 1]->HasHeldAISeat(hub.row.silentVoter) && !Bridges(*peers[id - 1]).contains(hub.row.silentVoter)))) caughtAt[id - 1] = hub.now;
				}
				hub.now += hub.row.stepMs; return true;
			}
			// A scheduled round's own record for its step: every frame all worlds have displayed compared, and each seat's hold in any view.
			bool StepSchedule(std::string& error) {
				// A seat that left has its history compared as far as it displayed it.
				uint64_t common = UINT64_MAX;
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) if (!hub.schedule->Left(seat, hub.now)) common = std::min(common, worlds[seat - 1].applied);
				if (common == UINT64_MAX) common = 0;
				if (common >= 60 && (startups.empty() || startups.back().first != roundStartMs)) { startups.push_back({roundStartMs, hub.now}); if (!settledAt) settledAt = hub.now; }
				if (!CompareHistories(error)) return false;
				// A seat is held while it holds itself or any seat that plays holds it: a seat held itself reads the round late, so its view
				// of the others' holds is not the round's.
				std::array<bool, NetMatchConfigUtil::c_MaxPeerCount + 1> held{};
				std::array<std::map<uint8_t, uint64_t>, NetMatchConfigUtil::c_MaxPeerCount + 1> views;
				for (uint8_t observer = 1; observer <= hub.row.seats; ++observer) if (peers[observer - 1]->IsRunning()) views[observer] = Bridges(*peers[observer - 1]);
				// A stopped seat keeps the hold time it had: its view is the one it stopped with.
				for (uint8_t observer = 1; observer <= hub.row.seats; ++observer) for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
					if (!peers[observer - 1]->IsRunning()) continue;
					const bool holds = views[observer].contains(seat) || peers[observer - 1]->HasHeldAISeat(seat);
					auto& since = heldByObserver[observer][seat];
					since = holds ? (since ? since : hub.now) : 0;
				}
				// The round's head: an observer that displays frames further back than its own delay and the bound reads the round late, as a
				// seat held itself does, so the holds it shows are ones the round has already passed.
				uint64_t head = 0;
				for (uint8_t observer = 1; observer <= hub.row.seats; ++observer)
					if (peers[observer - 1]->IsRunning() && !views[observer].contains(observer)) head = std::max(head, worlds[observer - 1].applied);
				for (uint8_t observer = 1; observer <= hub.row.seats; ++observer) {
					const auto& peer = *peers[observer - 1];
					if (!peer.IsRunning()) continue;
					if (views[observer].contains(observer)) { held[observer] = true; continue; }
					if (worlds[observer - 1].applied + peer.GetConfig().inputDelayFrames + peer.GetConfig().slowPlayerBoundTicks < head) continue;
					// A seat cut off or stopped hears no hold or return until it is back: its view waits for it.
					if (const auto* spell = hub.schedule->Active(observer, hub.now); spell && spell->kind != Spell::Kind::Slow && spell->kind != Spell::Kind::Late) continue;
					// Section 11.1: a hold an observer installed while its own links lose packets is that loss's, not the held seat's. A slow or late
					// observer still hears the seats it holds.
					const auto lossAt = [&](uint64_t at) {
						return std::any_of(hub.schedule->bursts.begin(), hub.schedule->bursts.end(), [&](const Burst& burst) { return burst.seat == observer && at >= burst.atMs && at < burst.atMs + burst.ms; });
					};
					// A view of that same hold taken over with the continuation this observer read from the lossy one is the same loss's.
					const auto ownFault = [&](uint8_t seat) {
						const uint64_t since = heldByObserver[observer][seat];
						uint64_t frame = 0;
						if (const auto boundaries = installedBoundaries[observer - 1].find(seat); boundaries != installedBoundaries[observer - 1].end())
							for (auto boundary = boundaries->second.rbegin(); boundary != boundaries->second.rend(); ++boundary) if (boundary->second) { frame = boundary->first; break; }
						if (lossAt(hub.now) || (since && lossAt(since))) {
							if (frame) lossyHoldAuthors.try_emplace({seat, frame}, observer);
							return true;
						}
						const auto author = lossyHoldAuthors.find({seat, frame});
						return frame && author != lossyHoldAuthors.end() && author->second != observer &&
						    std::any_of(imageStarts[observer].begin(), imageStarts[observer].end(), [&](const ImageStart& start) { return start.donor == author->second; });
					};
					for (const auto& [seat, since]: views[observer]) if (seat >= 1 && seat <= hub.row.seats && !ownFault(seat)) held[seat] = true;
					for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) if (seat != observer && peer.HasHeldAISeat(seat) && !ownFault(seat)) held[seat] = true;
				}
				// A seat whose image return activates at a frame the round's head has not reached is still held there, whatever view
				// already installed the return.
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) if (returnActivations[seat] > head && heldSince[seat] != UINT64_MAX) held[seat] = true;
				// A seat still in its image return has not returned before the round reaches its activation, though every view that holds it stands.
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat)
					if ((imageReading[seat - 1] || imageNeeded[seat - 1]) && heldSince[seat] != UINT64_MAX && !(returnActivations[seat] && returnActivations[seat] <= head)) held[seat] = true;
				for (uint8_t seat = 1; seat <= hub.row.seats; ++seat) {
					if (held[seat] && heldSince[seat] == UINT64_MAX && NetPeerFrameDetail::ReturnInputDetails()) {
						std::string by;
						for (uint8_t observer = 1; observer <= hub.row.seats; ++observer) if (peers[observer - 1]->IsRunning())
							by += " observer" + std::to_string(observer) + "=" + (views[observer].contains(seat) ? "bridge" : "") + (peers[observer - 1]->HasHeldAISeat(seat) ? "+ai" : "") +
							      "@" + std::to_string(worlds[observer - 1].applied);
						DiagnosticLine() << "[session-plane-held-view] seat=" << unsigned(seat) << " now_ms=" << hub.now << " head=" << head << by << std::endl;
					}
					if (held[seat] && heldSince[seat] == UINT64_MAX) heldSince[seat] = hub.now;
					if (!held[seat] && heldSince[seat] != UINT64_MAX) { heldSpans[seat].push_back({heldSince[seat], hub.now}); heldSince[seat] = UINT64_MAX; }
				}
				hub.now += hub.row.stepMs; return true;
			}
			// What each seat's referee answered: the questions its round asked, those answered Waiting, and how long the first decision took.
			std::string RefereeEvidence() const {
				std::string text;
				for (const auto& [seat, client]: hub.directoryClients) {
					if (!client.questionsAsked) continue;
					text += " referee_seat" + std::to_string(seat) + "=asked:" + std::to_string(client.questionsAsked) + ",waiting:" + std::to_string(client.waitingAnswers) +
					        ",decided:" + std::to_string(client.decidedAnswers) + ",requests:" + std::to_string(client.tieRequestCount) + ",first_decided_ms:" +
					        (client.firstDecidedMs == UINT64_MAX ? std::string("never") : std::to_string(client.firstDecidedMs - client.firstQuestionMs));
				}
				return text;
			}
			bool Run(const Case& row, std::string& error) {
				if (!Start(row, error)) return false;
				return RunStarted(row, error);
			}
			bool RunStarted(const Case& row, std::string& error) {
				const uint64_t end = hub.faultEnd + (row.gapMs > 5000 ? row.gapMs / 2 : 0) + c_TestReturnAllowanceMs + 4000 + row.postReturnMs;
				while (hub.now <= end) if (!Step(error) || !CheckBound(error) || !RecoverImages(error)) return false;
				if (row.refaultPercent && !hub.refaultEnd) { error = "the subject never came back, so its second slowdown never began"; return false; }
				while (hub.refaultEnd && hub.now <= hub.refaultEnd + c_TestReturnAllowanceMs + 4000) if (!Step(error) || !CheckBound(error) || !RecoverImages(error)) return false;
				if (!FinishImageReturns(error) || !CompareHistories(error, true)) return false;
				if (error = UnresolvedLateSplits(); !error.empty()) return false;
				if (error = UnreconvergedSeparations(); !error.empty()) return false;
				if (row.hung && row.subject == 1 && row.gapMs > 15000)
					for (uint8_t seat = 2; seat <= row.seats; ++seat) if (firstAdminChangeMs[seat - 1] == UINT64_MAX) {
						error = "authority ruling: a hung host outlasted the hold window without roster administration migrating"; return false;
					}
				if (row.asymmetricBridge && row.asymmetricBridge < 3 && !hub.asymmetricReleased) { error = "the asymmetric input lever did not force both pending bridge votes"; return false; }
				if (row.returnByTailOnly && !hub.droppedReturns) { error = "the tail-only lever withheld no proposal or certificate of the subject's return"; return false; }
				if (!hub.returnInsideInputs.empty()) { error = hub.returnInsideInputs; return false; }
				if (row.silentVoter && hub.silentAt == UINT64_MAX) { error = "the leader never voted the subject's return, so no voter fell silent at it"; return false; }
				if (row.heldOnce) {
					const auto boundaries = hub.bridgeBoundaries.find(row.subject);
					const auto holds = boundaries == hub.bridgeBoundaries.end() ? 0 : std::count_if(boundaries->second.begin(), boundaries->second.end(), [](const auto& change) { return change.second; });
					if (holds != 1) { error = "the slow seat was held " + std::to_string(holds) + " times, not once and kept"; return false; }
				}
				// A delayed referee never gates a local hold. Certified decisions still must not be dropped, and a return needs its installed hold.
				if (row.noRefusals) for (uint8_t id = 1; id <= row.seats; ++id) {
					const auto& stats = peers[id - 1]->GetStats();
					const uint64_t dropped = DecisionsDropped(stats);
					if (dropped || stats.returnsRefusedNotHeld) {
						error = "peer " + std::to_string(id) + " let go " + std::to_string(dropped) + " decisions and set aside " + std::to_string(stats.returnsRefusedNotHeld) +
						        " returns of holds it had not installed (" + std::to_string(stats.decisionsRefusedForAuthority) + " decisions waited for authority)";
						return false;
					}
				}
				if (row.donorSeat && !hub.tailsSentBy[row.donorSeat]) { error = "the held seat's tail never came from seat " + std::to_string(row.donorSeat); return false; }
				if (const auto twice = TwoReturnFrames(); !twice.empty()) { error = twice; return false; }
				const uint32_t requesters = ((1U << row.seats) - 1) & ~(1U << (row.subject - 1)) & ~(1U << 2);
				if (row.asymmetricBridge == 3 && (hub.asymmetricDropped != requesters || hub.asymmetricResent == 0)) { error = "the lost-forward lever did not recover an accepted input from its surviving witness"; return false; }
				if (!CheckBound(error)) return false;
				// A machine slower than the others may cost them at most half a tick a second while they run the round's rate.
				if (row.slowPercent) for (uint8_t id = 1; id <= row.seats; ++id) {
					if (IsAffected(id)) continue;
					const double tps = 1000.0 * static_cast<double>(paceEnd[id - 1] - paceBegin[id - 1]) / static_cast<double>(row.gapMs);
					if (tps < 59.5) {
						error = "peer " + std::to_string(id) + " ran " + std::to_string(tps) + " ticks/s while seat " + std::to_string(row.subject) + "'s machine ran at " +
						        std::to_string(row.slowPercent) + "% of the round's rate";
						return false;
					}
				}
				if (!wronglyHeld.empty()) { error = "a seat was held for another's lateness: " + wronglyHeld; return false; }
				if (!hub.directory.refusals.empty()) {
					error = std::to_string(hub.directory.refusals.size()) + " tie check-ins the directory refused, the first " + hub.directory.refusals.front(); return false;
				}
				if (row.imageReturn && !sawSplit && !imageAt[row.subject - 1]) { error = "the absent seat never handed itself to a private image after the donor's history ran out"; return false; }
				for (uint8_t id = 1; id <= row.seats; ++id) {
					if (row.imageReturn && imageReturns[id - 1]) continue;
					// The survivor keeps the absent seat under the AI until the private image brings it back.
					if (sawSplit) continue; // Split participants are checked by their delivered decision and completed private image, below.
					if (!caughtAt[id - 1]) {
						error = "a retained seat did not return automatically: peer=" + std::to_string(id) + " applied=" + std::to_string(worlds[id - 1].applied) +
						    " prepared=" + std::to_string(peers[id - 1]->GetStats().nextFrame) + " bridge_count=" + std::to_string(Bridges(*peers[id - 1]).size()) +
						    " holds_returning_seat=" + std::to_string(peers[id - 1]->HasHeldAISeat(hub.ReturningSeat())) + " image_returns=" + std::to_string(imageReturns[id - 1]) +
						    " highest_applied=" + std::to_string(std::max_element(worlds.begin(), worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied);
						return false;
					}
					const uint8_t held = row.subject;
					if (peers[id - 1]->HasHeldAISeat(held) || Bridges(*peers[id - 1]).contains(held)) { error = "the returning human seat stayed bridged after catch-up"; return false; }
					if (row.gapMs <= 3000 && !row.refaultPercent && caughtAt[id - 1] > hub.faultEnd + (row.returnWithinMs ? row.returnWithinMs : c_TestReturnAllowanceMs)) {
						error = "return exceeded the spike plus its catch-up allowance"; return false;
					}
					if (row.returnWithinMs && hub.now < caughtAt[id - 1] + 20000) { error = "the run ended before the returned seat played twenty seconds back"; return false; }
				}
				if (!sawSplit && row.gapMs >= 3000 && !row.keepInputEvery && !aiFrames) {
					error = "authority ruling: the missing seat never entered the local AI hold"; return false;
				}
				if (row.lossPercent) {
					for (const auto& peer: peers) if (peer->GetStats().peers.at(row.subject).substitutions || peer->HasHeldAISeat(row.subject) || Bridges(*peer).contains(row.subject)) { error = "steady packet loss substituted a sender"; return false; }
				}
				if (row.steadyMs && !row.lossPercent) {
					const auto& peer = *peers[row.subject - 1];
					const double delayMs = peer.InputDelayAt(row.subject, worlds[row.subject - 1].applied + 1) * c_TestTickMs;
					const auto& link = peer.GetStats().peers.at(row.subject);
					if (delayMs < row.rttMs || delayMs > row.rttMs + link.jitterMs || SenderResizes(peer, row.subject) != settledChanges[row.subject - 1]) { error = "steady RTT did not settle once inside its delay and jitter interval"; return false; }
					for (const auto& other: peers) if (other->GetStats().peers.at(row.subject).substitutions || Bridges(*other).contains(row.subject)) { error = "steady RTT committed a substitute"; return false; }
				}
				if (row.measurePace) for (uint8_t id = 1; id <= row.seats; ++id) {
					const uint64_t ticks = paceEnd[id - 1] - paceBegin[id - 1];
					if (ticks != row.steadyMs * 60 / 1000) { error = "peer " + std::to_string(id) + " pace=" + std::to_string(1000.0 * ticks / row.steadyMs) + "tps, expected 60"; return false; }
					if (id != 1 && peers[id - 1]->GetStats().peers.at(1).waits != settledHostWaits[id - 1]) { error = "a steady host path made a joiner wait"; return false; }
				}
				const uint64_t greatestWait = *std::max_element(maxWait.begin(), maxWait.end());
				DiagnosticLine() << "[session-plane-measure] name=" << row.name << " gap_ms=" << row.gapMs << " own_max_hitch_ms=" << maxWait[row.subject - 1]
				          << " max_wait_ms=" << greatestWait << " catch_up_ms=" << (caughtAt[row.subject - 1] ? caughtAt[row.subject - 1] - hub.faultEnd : 0)
				          << " held_human_inputs=0 ai_inputs=" << aiFrames << " first_discarded_press_ms="
				          << (firstLostPressMs == UINT64_MAX ? -1 : static_cast<int64_t>(firstLostPressMs)) << " authority_contract=local_hold_async_split histories=equal_within_each_half winner_undone=0" << RefereeEvidence() << std::endl;
				return true;
			}
		};

		// What a scheduled round got wrong: the property it broke (history, bound, return, or one of the round's rules), the shape of the
		// failure as a word the driver groups by, and what the round showed.
		// As NetMatchService::ReturnNoticeTicksLocked: the members' trip, the returner's own and the bound.
		uint64_t ReturnNoticeTicks(const NetLockstepCoordinator& serving, uint8_t returner, uint32_t tailLinkRttMs, uint64_t head) {
			uint64_t noticeTicks = 1;
			for (const auto& [peer, stats]: serving.GetStats().peers) {
				if (peer == returner || serving.IsPeerGoneAtFrame(peer, head) || serving.IsSeatUnderAI(peer, head)) continue;
				noticeTicks = std::max(noticeTicks, static_cast<uint64_t>(std::ceil((stats.pingMs + stats.jitterMs) / serving.GetConfig().simTickMs)));
			}
			return noticeTicks + static_cast<uint64_t>(std::ceil(tailLinkRttMs / serving.GetConfig().simTickMs)) + serving.GetConfig().slowPlayerBoundTicks;
		}

		// The same private image/replay/reclaim path for an absent peer and for every player on a losing half.
		ImageReturnTask ReadImageReturn(Fixture& fixture, uint8_t seat, uint8_t donor, std::string& error) {
			const auto fail = [&](const std::string& detail) { error = "authority ruling image return: " + detail; return false; };
			if (const auto twice = fixture.TwoReturnFrames(); !twice.empty()) co_return fail(twice);
			const uint8_t seats = fixture.hub.row.seats;
			uint8_t proposer = donor;
			const uint32_t rttMs = static_cast<uint32_t>(2 * fixture.hub.Delay(seat, donor));
			const uint64_t round = fixture.peers[donor - 1]->GetRoundId();
			const uint32_t incarnation = fixture.peers[donor - 1]->GetConfig().peerIncarnations.at(seat) + 1;
			std::vector<uint8_t> survivors;
			std::map<uint8_t, std::map<uint64_t, NetHash32>> untouched;
			const uint32_t members = s_FrameMembers(*fixture.peers[donor - 1], fixture.worlds[donor - 1].applied + 1);
			for (uint8_t peer = 1; peer <= seats; ++peer) if (peer != seat && (members & (1U << (peer - 1)))) {
				survivors.push_back(peer); untouched[peer] = fixture.worlds[peer - 1].hashes;
			}
			if (survivors.empty() || !fixture.peers[donor - 1]->HasHeldAISeat(seat)) co_return fail("the donor has no installed hold and playing group for this returner");
			fixture.imageReading[seat - 1] = true;
			fixture.FinishWait(seat, fixture.worlds[seat - 1].applied + 1);
			for (auto* wire: fixture.hub.wires) if (wire->owner == seat) wire->keepReturnCertificates = wire->keepImageTail = true;
			uint64_t common = fixture.worlds[donor - 1].applied;
			for (uint8_t peer: survivors) common = std::min(common, fixture.worlds[peer - 1].applied);
			// Before the first named capture the round's agreed start is the base every seat committed; its tail replays from frame 1.
			const uint64_t base = common - common % 60;
			if (base && !fixture.committed[donor - 1].contains(base))
				co_return fail("fixture retained no committed capture record at frame " + std::to_string(base) +
				            "; capture_frames=" + std::to_string(fixture.hub.row.captureFrames) + "; the image-return property could not be checked");
			World replay; replay.sharedDigests = &fixture.sharedDigests;
			// The image is the host's state at a named capture every playing seat already committed.
			for (const auto& [tick, ready]: fixture.committed[donor - 1]) {
				if (tick > base) break;
				if (!replay.Apply(ready, error)) co_return false;
			}
			if (base) for (uint8_t peer: survivors)
				if (fixture.worlds[peer - 1].hashes.at(base) != replay.hashes.at(base)) co_return fail("the image capture already has different playing-seat histories: seat " + std::to_string(peer) + " at frame " + std::to_string(base) +
					                 " (donor " + std::to_string(donor) + ", returner " + std::to_string(seat) + ", applied " + std::to_string(fixture.worlds[peer - 1].applied) + ")");
			DiagnosticLine() << "[session-plane-image] seed=0 seats=" << static_cast<unsigned>(seats) << " rtt_ms=" << rttMs
			                 << " capture=" << base << " playing_committed_through=" << common << " returner=" << unsigned(seat) << " donor=" << unsigned(donor)
			                 << " donor_members=" << members << " now_ms=" << fixture.hub.now << std::endl;
			fixture.imageStarts[seat].push_back({fixture.hub.now, static_cast<uint64_t>(std::ceil((common - base) * c_TestTickMs)) + rttMs / 2, rttMs / 2, donor});
			auto config = fixture.peers[seat - 1]->GetConfig();
			// The image's seat state and the committed tail go through the same replay coordinator as a real private return.
			auto replayConfig = config;
			// FLAGGED fixture departure: an image joins an already published round, as NetMatchRunner's world-join path does.
			replayConfig.requirePublishedStart = false;
			replayConfig.roundId = fixture.peers[donor - 1]->GetRoundId(); replayConfig.startFrame = replay.applied + 1;
			// As the service's image metadata does, seed the authority at the capture, not the losing reader's authority.
			// Otherwise the first tail record invents a handover and changes the image's already committed holds and prefix.
			if (base) replayConfig.authorityPeerId = fixture.committed[donor - 1].at(base).authorityPeerId;
			replayConfig.migrationGeneration = 0;
			for (const auto& [tick, ready]: fixture.committed[donor - 1]) {
				if (tick > base) break;
				for (const auto* commands: {&ready.localCommands, &ready.remoteCommands}) for (const auto& command: *commands)
					if (const auto* authority = std::get_if<NetGameHostAuthority>(&command.payload))
						replayConfig.migrationGeneration = std::max(replayConfig.migrationGeneration, authority->generation);
			}
			replayConfig.peerFrameGroups = false; replayConfig.peerSessionLinks.reset();
			replayConfig.originalRoundConfigHash = fixture.peers[donor - 1]->GetRoundConfigHash();
			replayConfig.initialSeatHolds = fixture.peers[donor - 1]->HeldTransactions();
			replayConfig.initialSeatReclaims = fixture.peers[donor - 1]->ReclaimTransactions();
			replayConfig.initialPeerLeaves = fixture.peers[donor - 1]->GetPeerLeaveFrames();
			replayConfig.initialDelayChanges = fixture.peers[donor - 1]->GetDelayChanges();
			// Later committed changes arrive through the replay, rather than being put into the older image.
			std::erase_if(replayConfig.initialSeatHolds, [base](const auto& entry) { return entry.second.cutoffFrame > base; });
			std::erase_if(replayConfig.initialSeatReclaims, [base](const auto& entry) { return entry.second.activationFrame > base; });
			std::erase_if(replayConfig.initialPeerLeaves, [base](const auto& entry) { return entry.second > base; });
			for (auto& [peer, changes]: replayConfig.initialDelayChanges) changes.erase(changes.upper_bound(base), changes.end());
			LoopbackTransport replayWire; NetLockstepCoordinator replayCoordinator;
			// The replay is chosen as the service chooses it (NetMatchService::BeginInPlaceCatchUp): only a sim short of its own hold frame
			// still holds the round's state and imports its seat transitions in place; any other returns through a fresh replay.
			const auto& returner = *fixture.peers[seat - 1];
			const uint64_t holdFrame = returner.GetLocalHoldFrame(), simulated = fixture.worlds[seat - 1].applied;
			const bool discardedBranch = !(returner.IsLocalSeatHeld() && returner.UsesBoundedWait() && !returner.IsPersistentWorldRound() &&
			    holdFrame != 0 && simulated != 0 && simulated < holdFrame);
			if (discardedBranch ? !replayCoordinator.StartReplay(replayWire, replayConfig, &error) :
			    !replayCoordinator.StartCatchUpReplay(replayWire, replayConfig, *fixture.peers[seat - 1], &error)) co_return false;
			NetWorldJoinHost join;
			if (config.matchConfig.persistentWorld) {
				// FLAGGED fixture departure: a persistent held seat uses the world's credentialed bootstrap, as the service does.
				NetWorldIdentity identity{config.matchConfig.worldId, config.matchConfig.worldBoot, round};
				if (!join.Configure(config.matchConfig, identity, &error) || !join.Membership().Hold(seat, seat - 1, "Returner", &error)) co_return false;
				join.NoteReclaimHolds({seat});
				if (!join.BeginJoin(12, seat - 1, "Returner", fixture.hub.now, &error, true, seat)) co_return false;
			} else if (!join.ConfigureMatchRejoins(config.matchConfig, round, c_TestTickMs, &error) ||
			           !join.BeginRejoin(12, seat - 1, seat, incarnation, "Returner", fixture.hub.now, &error)) co_return false;
			NetWorldCheckpointImage image; image.round = round; image.tick = replay.applied;
			if (config.matchConfig.persistentWorld) { image.worldId = config.matchConfig.worldId; image.boot = config.matchConfig.worldBoot; }
			else image.privateSessionId = config.sessionId;
			image.bytes = 8; image.checkpointConfig = "fixture world"; image.sideState = "fixture seats";
			join.PublishImage(image);
			if (!join.NoteTransferComplete(12, 8, &error)) co_return false;
			join.NoteRejoinLinkFit(12, true); join.NoteTailLinkRtt(12, rttMs);
			uint64_t tailRecorded = base;
			std::map<uint64_t, NetLockstepFrame> receivedTail;
			std::vector<uint8_t> partialTail;
			uint64_t activation = 0, reported = replay.applied, reportAt = fixture.hub.now, work = 0, workUs = 0, reportedWork = 0, replayTracedAt = 0;
			struct ReplayReport { uint64_t arrival, applied, ticks, elapsed, work, workUs; };
			std::deque<ReplayReport> reports;
			unsigned reportsRead = 0;
			double credit = 0, maximumReplayCapacity = 0;
			bool proposed = false, agreed = false;
			bool liveStarted = false, certificateReported = false, refusalReported = false;
			std::map<uint64_t, NetLockstepReadyFrame> replayedFrames; //!< The replay's own frames, named when its certificate is refused.
			std::string lastRefusal;
			const char* lastGate = nullptr;
			uint64_t proposalAttempts = 0, lastSeenHead = 0;
			const auto startLive = [&] {
				// The real service starts live play from the winning replay's configuration, including authority and every member's start terms.
				config = replayCoordinator.GetConfig();
				const auto& previous = fixture.peers[seat - 1]->GetConfig();
				config.peerFrameGroups = previous.peerFrameGroups; config.peerSessionLinks = previous.peerSessionLinks;
				config.frameTieReferee = previous.frameTieReferee; config.hostChangeReferee = previous.hostChangeReferee;
				config.migrationTransportFactory = previous.migrationTransportFactory; config.migrationIceDial = previous.migrationIceDial; config.migrationIceHost = previous.migrationIceHost;
				config.startFrame = activation; config.roundId = round; config.joinsRunningRound = config.continuesMatch = true;
				config.originalRoundConfigHash = fixture.peers[donor - 1]->GetRoundConfigHash();
				config.initialSeatHolds = replayCoordinator.HeldTransactions();
				config.initialSeatBridgeSinceMs = replayCoordinator.SeatBridgeTimes();
				config.initialSeatReclaims = replayCoordinator.ReclaimTransactions();
				config.initialPeerLeaves = replayCoordinator.GetPeerLeaveFrames();
				NetLockstepCoordinator::AdoptOpenReturns(config, fixture.peers[seat - 1]->ReclaimTransactions(), activation);
				if (config.peerFrameGroups) {
					const NetHostMigrationMessage* proof = nullptr;
					for (const auto* wire: fixture.hub.wires) if (wire->owner == seat) {
						const auto found = wire->receivedReturnCertificates.find(activation);
						if (found != wire->receivedReturnCertificates.end()) { proof = &found->second; break; }
					}
					if (!proof) return true;
					if (!certificateReported && NetPeerFrameDetail::ReturnInputDetails()) {
						DiagnosticLine() << "[session-plane-image-certificate] returner=" << unsigned(seat) << " activation=" << activation
						    << " boundary=" << proof->boundary << " replay_start=" << replayCoordinator.GetConfig().startFrame << " replay_applied=" << replay.applied << std::endl;
						certificateReported = true;
					}
					if (!replayCoordinator.AdoptPeerReturn(config, replayCoordinator, *proof, activation, &lastRefusal)) {
						if (NetPeerFrameDetail::ReturnInputDetails() && !refusalReported) {
							refusalReported = true;
							const auto describe = [](const NetLockstepReadyFrame& ready) {
								std::string text = "authority=" + std::to_string(ready.authorityPeerId) + "/" + std::to_string(ready.updateAuthorityPeerId) + " departed=";
								for (uint8_t peer: ready.departedPeerIds) text += std::to_string(peer);
								text += " ai="; for (uint8_t peer: ready.aiHeldPeerIds) text += std::to_string(peer);
								text += " reclaimed="; for (uint8_t peer: ready.reclaimedPeerIds) text += std::to_string(peer);
								text += " local=" + std::to_string(ready.localPeerId) + ":" + std::to_string(ready.localFrames.size()) + " remote=";
								for (const auto& [peer, count]: ready.remoteFrameCounts) text += std::to_string(peer) + ":" + std::to_string(count) + ",";
								for (const auto& [peer, frame]: ready.committedPeerLeaves) text += " leave" + std::to_string(peer) + "@" + std::to_string(frame);
								text += " leaves=" + std::to_string(ready.committedPeerLeaves.size()) + " waivers=" + std::to_string(ready.committedFrameWaivers.size()) +
								        " inputs=" + std::to_string(ready.remoteFrames.size() + ready.localFrames.size()) + " commands=" + std::to_string(ready.remoteCommands.size() + ready.localCommands.size());
								const NetLockstepFrame packed = PackWorldJoinReadyFrame(ready);
								std::vector<uint8_t> record;
								(void)EncodeCommittedJoinFrame(packed, record);
								uint32_t digest = 2166136261U;
								for (uint8_t byte: record) digest = (digest ^ byte) * 16777619U;
								text += " packed_frames=" + std::to_string(packed.frames.size()) + " packed_commands=" + std::to_string(packed.commands.size()) +
								        " observations=" + std::to_string(packed.observations.size()) + "/" + std::to_string(packed.valueObservations.size()) +
								        " record_bytes=" + std::to_string(record.size()) + " record_fnv=" + std::to_string(digest) + " update_authority=" + std::to_string(packed.replayUpdateAuthorityPeerId);
								for (const auto& input: packed.frames) text += " actor" + std::to_string(input.actorUniqueID) + ":" + std::to_string(input.stateMask);
								return text;
							};
							const auto replayed = replayedFrames.find(proof->boundary);
							const auto donated = fixture.committed[donor - 1].find(proof->boundary);
							DiagnosticLine() << "[session-plane-image-adoption] refusal=" << lastRefusal << " boundary=" << proof->boundary << " replay={" << (replayed != replayedFrames.end() ? describe(replayed->second) : std::string("none"))
							    << "} donor" << unsigned(donor) << "={" << (donated != fixture.committed[donor - 1].end() ? describe(donated->second) : std::string("none")) << "}" << std::endl;
						}
						return true;
					}
				} else {
					const auto found = fixture.wires[seat - 1]->receivedTimingReturns.find({activation, seat});
					if (found == fixture.wires[seat - 1]->receivedTimingReturns.end()) return true;
					const auto& timing = found->second;
					if (timing.senderPeerId != config.authorityPeerId || timing.sessionId != config.sessionId || timing.roundId != round ||
					    timing.authorityGeneration != config.migrationGeneration || timing.seatIncarnations[seat - 1] != incarnation)
						return fail("the received legacy return certificate does not describe this image and incarnation");
					config.initialSeatReclaims[seat] = {seat, timing.authorityGeneration, timing.revision, incarnation,
					    timing.applyFrame, timing.delayFrames, timing.neutralThroughFrame};
				}
				config.initialDelayChanges = replayCoordinator.GetDelayChanges();
				for (const auto& [peer, changes]: fixture.peers[seat - 1]->GetDelayChanges())
					for (const auto& [tick, delay]: changes) config.initialDelayChanges[peer].try_emplace(tick, delay);
				// The live start carries the delays the replay actually crossed, as InPlaceLiveRoundLocked does.
				for (const auto& [peer, changes]: config.initialDelayChanges) {
					const auto at = changes.upper_bound(activation);
					if (at != changes.begin()) config.matchConfig.peerInputDelayFrames[peer - 1] = std::prev(at)->second;
				}
				config.inputDelayFrames = config.matchConfig.peerInputDelayFrames[seat - 1];
				config.relayToOtherPeers = config.localPeerId == config.authorityPeerId;
				for (uint8_t peer = 1; peer <= seats; ++peer) config.peerInputDelayFrames[peer] = config.matchConfig.peerInputDelayFrames[peer - 1];
				config.peerIncarnations[seat] = incarnation; config.seatStateThroughFrame = replay.applied;
				if (NetPeerFrameDetail::ReturnInputDetails()) {
					std::string held, reclaimed;
					for (const auto& [peer, hold]: config.initialSeatHolds) held += " " + std::to_string(peer) + "@" + std::to_string(hold.cutoffFrame);
					for (const auto& [peer, back]: config.initialSeatReclaims) reclaimed += " " + std::to_string(peer) + "@" + std::to_string(back.activationFrame);
					DiagnosticLine() << "[session-plane-image-live] returner=" << unsigned(seat) << " activation=" << activation << " holds=" << (held.empty() ? " none" : held)
					    << " reclaims=" << (reclaimed.empty() ? " none" : reclaimed) << " replay_applied=" << replay.applied << " now_ms=" << fixture.hub.now << std::endl;
				}
				fixture.peers[seat - 1] = std::make_unique<NetLockstepCoordinator>();
				if (!fixture.peers[seat - 1]->Start(*fixture.wires[seat - 1], config, &error)) return false;
				// FLAGGED fixture departure: import completed replay seat state during the live handshake, as the runner does.
				fixture.peers[seat - 1]->AdoptReplayedSeatTransitions(replayCoordinator, replay.applied);
				fixture.peers[seat - 1]->DeferStopsToTickBoundary();
				liveStarted = true;
				return true;
			};

			const uint32_t recoveryPercent = fixture.hub.schedule ? fixture.hub.schedule->recoverPercent[seat] :
			    fixture.hub.row.catchUpPercent ? fixture.hub.row.catchUpPercent : c_TestCatchUpRate * 100;
			const uint64_t replayBudget = 12000 + (recoveryPercent > 100 && recoveryPercent < 300 ? 100000 / (recoveryPercent - 100) : 0);
			for (const uint64_t until = fixture.hub.now + replayBudget; fixture.hub.now < until;) {
				// FLAGGED fixture departure: use the production image-tail log, datagrams and codecs on the donor's real
				// fixture link. The reader below can consume only records delivered after that link's delay and loss.
				for (auto next = fixture.committed[donor - 1].upper_bound(tailRecorded); next != fixture.committed[donor - 1].end(); ++next) {
					auto record = PackWorldJoinReadyFrame(next->second); record.roundId = round;
					if (!join.Tail().Append(record, &error)) co_return fail(error);
					tailRecorded = next->first;
				}
				// A seat that leaves, or that the host removes, while it reads its image never returns: the AI keeps it.
				if (fixture.hub.schedule && fixture.hub.schedule->Left(seat, fixture.hub.now)) {
					fixture.imageReading[seat - 1] = fixture.imageNeeded[seat - 1] = false;
					co_return true;
				}
				const auto [tailWire, tailRoute] = fixture.ImageReturnRoute(donor, seat);
				if (!tailWire || tailRoute == c_InvalidNetPeerId) co_return fail("the image donor lost its authenticated return route");
				// As the service notes every unfinished return it serves (NetMatchService's world-join pump).
				fixture.peers[donor - 1]->NoteReturningLink(seat, tailRoute, fixture.hub.now);
				for (unsigned datagram = 0; datagram < 32; ++datagram) {
					std::vector<uint8_t> packed, bytes; bool large = false;
					const bool whole = join.NextTailDatagram(12, fixture.hub.now, packed, &large);
					if (!whole && !(large && join.NextTailChunk(12, packed))) break;
					if (!NetLobbyProtocol::Encode({MakeWorldTailChunk(round, packed)}, bytes)) co_return fail("the native image-tail chunk did not encode");
					if (!tailWire->Send(tailRoute, whole ? NetTransportLane::BulkUnreliable : NetTransportLane::ControlReliable, bytes)) {
						join.NoteTailDatagramRefused(12); break;
					}
					if (!whole) { join.NoteTailChunkSent(12, packed.size()); break; }
				}
				co_await std::suspend_always{};
				if (!fixture.CheckBound(error)) co_return false;
				// As the donor's service drops every return it serves when its own round is set aside, a donor that starts its own image
				// return before this return is agreed ends it, and the seat asks again from the ruling half. So does a donor held out of
				// its own playing group: its committed history then grows only as fast as its own private catch-up.
				const bool donorHeld = !(s_FrameMembers(*fixture.peers[donor - 1], fixture.worlds[donor - 1].applied + 1) & (1U << (donor - 1)));
				// A donor whose player leaves closes every return it serves.
				const bool donorLeft = fixture.hub.schedule && fixture.hub.schedule->Left(donor, fixture.hub.now);
				// One whose link to its donor went dark before its certificate reached it misses its activation once the donor's round
				// reaches it: the donor's service drops that return too, and the seat asks again once it reaches its ruling half.
				const bool donorCut = !liveStarted && activation && fixture.hub.LiveSplit(seat, donor) && fixture.worlds[donor - 1].applied + 1 >= activation;
				if ((!proposed && (fixture.imageReading[donor - 1] || donorHeld || donorLeft)) || donorCut) {
					DiagnosticLine() << "[session-plane-image-donor-lost] returner=" << unsigned(seat) << " donor=" << unsigned(donor)
					                 << " replay_applied=" << replay.applied << " now_ms=" << fixture.hub.now << std::endl;
					fixture.imageReading[seat - 1] = false;
					fixture.abandonedDonors[seat - 1].insert(donor);
					co_return true;
				}
				if (!liveStarted && fixture.peers[seat - 1]->IsStopped()) {
					for (auto* wire: fixture.hub.wires) if (wire->owner == seat) {
						wire->keepReturnCertificates = true;
						(void)wire->PollEvents();
					}
				}
				for (auto* wire: fixture.hub.wires) if (wire->owner == seat) {
					while (!wire->receivedImageTail.empty()) {
						const auto delivery = std::move(wire->receivedImageTail.front()); wire->receivedImageTail.pop_front();
						// A donor this seat gave up keeps its datagrams in flight; the reader serves only its current session.
						if (delivery.donor != donor && fixture.abandonedDonors[seat - 1].contains(delivery.donor)) continue;
						if (!ReadImageTail(delivery, donor, round, replay.applied, partialTail, receivedTail, error)) co_return fail(error);
					}
				}
				double capacity = fixture.hub.schedule ? fixture.hub.schedule->CapacityPercent(seat, fixture.hub.now) / 100.0 :
				    fixture.hub.row.catchUpPercent ? fixture.hub.row.catchUpPercent / 100.0 : c_TestCatchUpRate;
				if (seat == fixture.hub.row.subject && fixture.hub.row.roundRateAfterMs && fixture.hub.now >= fixture.hub.faultEnd + fixture.hub.row.roundRateAfterMs) capacity = 1;
				if (fixture.hub.schedule) if (const auto* spell = fixture.hub.schedule->Active(seat, fixture.hub.now)) {
					if (spell->kind == Spell::Kind::Slow) capacity = spell->percent / 100.0;
					if (spell->kind == Spell::Kind::Stopped || spell->kind == Spell::Kind::Hung) capacity = 0;
				}
				maximumReplayCapacity = std::max(maximumReplayCapacity, capacity);
				credit = capacity > 0 ? std::min(6.0, credit + fixture.hub.row.stepMs * capacity / c_TestTickMs) : 0;
				while (credit >= 1 && (!activation || replay.applied + 1 < activation)) {
					const auto next = receivedTail.find(replay.applied + 1);
					if (next == receivedTail.end()) break;
					const auto& recorded = next->second;
					if (!replayCoordinator.QueueReplayFrame(recorded.targetFrame, recorded.frames, recorded.commands, &error,
					                                      recorded.observations, recorded.valueObservations, recorded.senderPeerId,
					                                      recorded.replayUpdateAuthorityPeerId)) co_return false;
					replayCoordinator.Tick(fixture.hub.now);
					NetLockstepReadyFrame ready;
					if (!replayCoordinator.PopReadyFrame(ready)) co_return fail("the private coordinator did not consume committed frame " +
					    std::to_string(recorded.targetFrame) + ": state=" + std::to_string(static_cast<int>(replayCoordinator.GetState())) +
					    " next=" + std::to_string(replayCoordinator.GetStats().nextFrame) + " reason=" + replayCoordinator.GetStats().timeoutReason);
					if (NetPeerFrameDetail::ReturnInputDetails()) replayedFrames[ready.frame] = ready;
					if (!replay.Apply(ready, error)) co_return false;
					replayCoordinator.FinishSimulationTick(ready.frame);
					if (liveStarted) fixture.peers[seat - 1]->AdoptReplayedSeatTransitions(replayCoordinator, replay.applied);
					receivedTail.erase(next);
					credit -= 1; ++work;
					// FLAGGED fixture departure: a private replay reports the same work cost as its supplied machine capacity.
					workUs += static_cast<uint64_t>(std::ceil(c_TestTickMs * 1000 / capacity));
				}
				if (NetPeerFrameDetail::ReturnInputDetails() && fixture.hub.now >= replayTracedAt + 250) {
					replayTracedAt = fixture.hub.now;
					DiagnosticLine() << "[session-plane-image-replay] returner=" << unsigned(seat) << " applied=" << replay.applied << " received=" << receivedTail.size()
					    << " lowest=" << (receivedTail.empty() ? 0 : receivedTail.begin()->first) << " highest=" << (receivedTail.empty() ? 0 : receivedTail.rbegin()->first)
					    << " horizon=" << fixture.worlds[donor - 1].applied << " credit=" << credit << " now_ms=" << fixture.hub.now << std::endl;
				}
				// As the service's pump does, the replay reports once a frame whenever its state moved, and every 250 ms when it stands.
				if (fixture.hub.now >= reportAt + (work != reportedWork ? static_cast<uint64_t>(std::ceil(c_TestTickMs)) : 250)) {
					// Flagged fixture correction: production receives this cursor after its link transit. Comparing
					// an instantaneous callback to a transported report invents an extra half-trip of producer lag.
					reports.push_back({fixture.hub.now + fixture.hub.Delay(seat, donor), replay.applied, replay.applied - reported,
					                   fixture.hub.now - reportAt, work, workUs});
					reported = replay.applied; reportedWork = work; reportAt = fixture.hub.now;
				}
				while (!reports.empty() && reports.front().arrival <= fixture.hub.now) {
					const auto report = reports.front(); reports.pop_front();
					if (!join.NoteRejoinCapacity(12, report.work, report.workUs, 0)) co_return fail("the private replay published invalid cumulative work");
					if (join.FindSession(12)->headroom.Ratio() > maximumReplayCapacity + 1e-6)
						co_return fail("the private replay claimed more capacity than its supplied tick budget: reported=" +
						    std::to_string(join.FindSession(12)->headroom.Ratio()) + " supplied=" + std::to_string(maximumReplayCapacity));
					uint64_t announced = 0;
					// As the service does, the coordinator that will propose the return sizes how far it may trail, and the donor's notice how far ahead.
					const auto& noticing = *fixture.peers[donor - 1];
					const uint64_t noticeHead = std::max(noticing.GetStats().nextFrame, noticing.SentInputThrough());
					const uint64_t placedFrom = fixture.worlds[donor - 1].applied;
					join.NoteReturnNotice(12, ReturnNoticeTicks(noticing, seat, join.FindSession(12)->tailLinkRttMs, noticeHead) + (noticeHead > placedFrom ? noticeHead - placedFrom : 0));
					const uint8_t sizing = fixture.ImageReturnProposer(donor);
					join.NoteReturnTrailRoom(12, fixture.peers[sizing - 1]->PeerReturnTrailRoom(seat, fixture.ImageReturnRoute(sizing, seat).second, fixture.worlds[donor - 1].applied));
					// FLAGGED fixture departure: each return has its own join host here; the service's one host sees every return's activation.
					std::set<uint64_t> others;
					for (uint8_t other = 1; other <= seats; ++other) if (other != seat && fixture.returnActivations[other]) others.insert(fixture.returnActivations[other]);
					join.ReserveActivations(others);
					if (!join.NoteCatchUpProgress(12, report.applied, report.ticks, report.elapsed,
					                            fixture.worlds[donor - 1].applied, &announced, &error)) co_return false;
					if (announced && announced != activation) {
						const auto* announcing = join.FindSession(12);
						DiagnosticLine() << "[session-plane-image-activation] returner=" << unsigned(seat) << " announced=" << announced << " previous=" << activation
						                 << " applied=" << report.applied << " horizon=" << fixture.worlds[donor - 1].applied << " closing_rate=" << announcing->closingRate
						                 << " capacity_ratio=" << announcing->headroom.Ratio() << " trail=" << announcing->activationTrailFrames << " now_ms=" << fixture.hub.now << std::endl;
					}
					if (announced) activation = fixture.returnActivations[seat] = announced;
					// Concurrent returns start a notice and the bound apart: the frames another return moved this one count against its deadline.
					if (announced) fixture.imageStarts[seat].back().spacingMs = static_cast<uint64_t>(std::ceil(join.FindSession(12)->activationSpacing * c_TestTickMs));
					if (const char* gate = join.FindSession(12)->catchUpGate; gate && gate != lastGate) {
						DiagnosticLine() << "[session-plane-image-gate] returner=" << unsigned(seat) << " gate=" << gate << " applied=" << report.applied
						                 << " horizon=" << fixture.worlds[donor - 1].applied << " closing_rate=" << join.FindSession(12)->closingRate
						                 << " capacity_ratio=" << join.FindSession(12)->headroom.Ratio() << " now_ms=" << fixture.hub.now << std::endl;
						lastGate = gate;
					}
					join.NoteCatchUpClock(12, fixture.hub.now);
					join.AcknowledgeTailDatagrams(12, fixture.hub.now);
					if (reportsRead++ < 3) DiagnosticLine() << "[session-plane-image-report] returner=" << unsigned(seat)
					    << " arrival_ms=" << fixture.hub.now << " transit_ms=" << fixture.hub.Delay(seat, donor)
					    << " applied=" << report.applied << " horizon=" << fixture.worlds[donor - 1].applied
					    << " capacity_ratio=" << join.FindSession(12)->headroom.Ratio() << std::endl;
				}
				// The donor's service runs on its main loop: hung or stopped, it proposes nothing until it resumes.
				if (activation && !proposed && fixture.ImageDonorCanAnswer(donor) && !(fixture.hub.schedule && fixture.hub.schedule->Is(donor, fixture.hub.now, Spell::Kind::Stopped))) {
					// Flagged fixture correction: the service carries the measured private-reader trail into the
					// temporary activation gap. Dropping it asks for input before that reader can produce it.
					const auto* session = join.FindSession(12);
					if (!session || session->activationTick != activation) co_return fail("the image activation lost its measured session");
					proposer = fixture.ImageReturnProposer(donor);
					auto& coordinator = *fixture.peers[proposer - 1];
					// As the donor's service does, the reclaim is proposed one notice ahead of the donor's head, so an activation that can still
					// come forward or must move later is not fixed early; one the round passed or that the replay trails is announced again.
					const auto& serving = *fixture.peers[donor - 1];
					const uint64_t head = std::max(serving.GetStats().nextFrame, serving.SentInputThrough());
					const uint64_t noticeTicks = ReturnNoticeTicks(serving, seat, session->tailLinkRttMs, head);
					const uint64_t previousHead = lastSeenHead; lastSeenHead = head;
					if (activation > head + noticeTicks + 1) continue;
					// The trail is judged against the same horizon the progress reports are, the donor's applied world.
					const uint64_t roundFrame = fixture.worlds[donor - 1].applied;
					if (activation <= head || (roundFrame > session->acknowledgedThrough ? roundFrame - session->acknowledgedThrough : 0) >
					                              session->activationTrailFrames + serving.GetConfig().slowPlayerBoundTicks) {
						uint64_t later = 0;
						std::set<uint64_t> others;
						for (uint8_t other = 1; other <= seats; ++other) if (other != seat && fixture.returnActivations[other]) others.insert(fixture.returnActivations[other]);
						join.ReserveActivations(others);
						if (join.ReannounceActivation(12, roundFrame, &later, nullptr)) {
							DiagnosticLine() << "[session-plane-image-activation] returner=" << unsigned(seat) << " announced=" << later << " previous=" << activation
							                 << " waits_for_tail=1 attempts=" << proposalAttempts << " last_head=" << previousHead << " head=" << head << " notice=" << noticeTicks << " round=" << roundFrame << " acknowledged=" << session->acknowledgedThrough
							                 << " trail=" << session->activationTrailFrames << " replay=" << replay.applied << " now_ms=" << fixture.hub.now << std::endl;
							activation = fixture.returnActivations[seat] = later;
						}
						continue;
					}
					// As the service does, an activation the returner's last report no longer reaches is not proposed.
					if (!session->activationReachable) continue;
					// FLAGGED fixture departure: a refused proposal remains private and retries, as the service does.
					std::string refusal;
					++proposalAttempts;
					if (!coordinator.SchedulePeerReclaim(seat, fixture.ImageReturnRoute(proposer, seat).second, incarnation, activation, &refusal, session->activationTrailFrames)) {
						if (lastRefusal != refusal) DiagnosticLine() << "[session-plane-image-proposal] waits=" << refusal << std::endl;
						lastRefusal = refusal;
						continue;
					}
					DiagnosticLine() << "[session-plane-image-reclaim] returner=" << unsigned(seat) << " activation=" << activation
					                 << " trail=" << session->activationTrailFrames << " head=" << head << " notice=" << noticeTicks << " donor_next=" << serving.GetStats().nextFrame
					                 << " donor_applied=" << fixture.worlds[donor - 1].applied << " acknowledged=" << session->acknowledgedThrough << " now_ms=" << fixture.hub.now << std::endl;
					proposed = true;
					join.MarkActivationProposed(12);
				}
				if (proposed && !agreed) {
					agreed = fixture.peers[proposer - 1]->HasAgreedSeatReclaim(seat, activation);
					// FLAGGED fixture departure: an intervening hold changes the activation's actual electorate.
					const uint32_t playing = s_FrameMembers(*fixture.peers[proposer - 1], activation);
					for (uint8_t peer = 1; peer <= seats; ++peer)
						if (peer != seat && (playing & (1U << (peer - 1))) && !fixture.peers[proposer - 1]->IsSeatReclaimGap(peer, activation))
							agreed &= fixture.peers[peer - 1]->HasAgreedSeatReclaim(seat, activation);
					if (agreed) {
						fixture.peers[donor - 1]->NoteReturnerCatchingUp(seat, fixture.hub.now);
						(void)fixture.peers[donor - 1]->SendReturnerTheRoundFrom(seat, activation);
					}
				}
				// As in the service, the live connection handshakes ahead of the return while the private replay finishes E-1.
				if (agreed && !liveStarted && !startLive()) co_return false;
				if (liveStarted && replay.applied + 1 == activation && fixture.peers[seat - 1]->IsRunning()) break;
			}
			if (!activation && fixture.hub.schedule && (recoveryPercent <= 100 || fixture.hub.now > fixture.hub.schedule->endMs)) {
				// The driver's original capacity property gives a machine with no surplus no finite catch-up deadline, and one whose surplus
				// has not closed its replay when the case ends a deadline past it. It stays a private reader under the AI; every survivor is
				// still checked on every step, with no wait exemption, and the return property judges its hold.
				fixture.imageWithoutHeadroom[seat - 1] = true;
				// The arbitration stopped this side and it plays no continuation of its own: its return is the capacity property's to judge.
				for (auto& late: fixture.lateSplits) if (!late.resolvedAt && (late.a == seat || late.b == seat)) late.resolvedAt = fixture.hub.now;
				co_return fixture.CheckBound(error);
			}
			if (!agreed || replay.applied + 1 != activation) co_return fail("the private replay did not reach an activation every survivor certified: donor=" +
			    std::to_string(donor) + " activation=" + std::to_string(activation) + " replay=" + std::to_string(replay.applied) + " agreed=" + std::to_string(agreed) + s_WaitState(*fixture.peers[donor - 1]));
			if (!liveStarted) co_return fail("the returning seat could not adopt its received return certificate: " + (lastRefusal.empty() ? "none received" : lastRefusal));
			if (!fixture.peers[seat - 1]->IsRunning()) co_return fail("the returning seat did not finish its live handshake before activation: state=" +
			    std::to_string(static_cast<int>(fixture.peers[seat - 1]->GetState())) + " starts_received=" + std::to_string(fixture.peers[seat - 1]->GetStats().startPacketsReceived) + s_WaitState(*fixture.peers[seat - 1]));
			fixture.peers[seat - 1]->AdoptReplayedSeatTransitions(replayCoordinator, activation - 1);
			if (NetPeerFrameDetail::ReturnInputDetails()) DiagnosticLine() << "[session-plane-image-adopted] returner=" << unsigned(seat) << " activation=" << activation
			    << " replay_applied=" << replay.applied << " live_running=" << fixture.peers[seat - 1]->IsRunning()
			    << s_WaitState(*fixture.peers[seat - 1]) << " now_ms=" << fixture.hub.now << std::endl;
			fixture.discardedHistories[seat - 1].push_back(fixture.worlds[seat - 1].hashes);
			fixture.worlds[seat - 1] = std::move(replay); fixture.queued[seat - 1] = activation - 1; fixture.credits[seat - 1].Reset(); fixture.waitAt[seat - 1] = fixture.releasedAt[seat - 1] = 0;
			fixture.frameMembers[seat - 1].assign(fixture.frameMembers[donor - 1].begin(), fixture.frameMembers[donor - 1].begin() + activation);
			fixture.frameAuthors[seat - 1].assign(fixture.frameAuthors[donor - 1].begin(), fixture.frameAuthors[donor - 1].begin() + activation);
			fixture.committed[seat - 1].clear();
			fixture.committed[seat - 1].insert(fixture.committed[donor - 1].begin(), fixture.committed[donor - 1].lower_bound(activation));
			fixture.installedBoundaries[seat - 1] = fixture.installedBoundaries[donor - 1];
			fixture.ForgetComparisons(seat);
			fixture.imageReading[seat - 1] = fixture.imageNeeded[seat - 1] = false; ++fixture.imageReturns[seat - 1];
			// A later stop of this seat is a new one: its ruling and frame are no longer this loss's.
			fixture.lossServedAt[seat - 1] = fixture.lostAt[seat - 1];
			// The finished session's donor keeps datagrams in flight; a later return of this seat reads only its own session.
			fixture.abandonedDonors[seat - 1].insert(donor);
			fixture.waitPlaying[seat - 1] = false;
			if (!fixture.hub.schedule && fixture.hub.row.frozen) fixture.hub.faultEnd = fixture.hub.now;
			fixture.peers[donor - 1]->NoteReturnerCaughtUp(seat, fixture.hub.now);
			(void)fixture.peers[donor - 1]->SendReturnerTheRoundFrom(seat, activation);
			const uint64_t observationAt = fixture.hub.now;
			// FLAGGED fixture departure: a newly injected fault of the returner's own - its machine, its links' spell or loss burst -
			// belongs to the next hold, not this return's healthy observation.
			// FLAGGED fixture departure: a survivor split off before the activation plays its own continuation, which never took this
			// return; its view of the seat is that half's, ruled at its heal. A playing survivor that has not yet applied the activation
			// still shows the hold the return ends. Every seat playing at the activation is checked from the activation on.
			// A member in its own return's neutral interval at the activation plays none of it, as the engine counts no vote from it.
			uint32_t playingAtActivation = s_FrameMembers(*fixture.peers[donor - 1], activation);
			for (uint8_t peer = 1; peer <= fixture.hub.row.seats; ++peer)
				if (fixture.peers[donor - 1]->IsSeatReclaimGap(peer, activation)) playingAtActivation &= ~(1U << (peer - 1));
			// FLAGGED fixture correction: an observer in a fault of its own (cut off, silent, hung, stopped, slow, late or lossy) holds a seat
			// it cannot hear at its own bound, as section 11.1 has it; that view, and the hold it installed then, are its own fault's.
			// Section 11.1: a late input that parts the round after the activation, with this seat on one side, holds it as that input's,
			// as the return property has it; the observation ends there.
			const auto lateSplitSince = [&] {
				return std::any_of(fixture.lateSplits.begin(), fixture.lateSplits.end(), [&](const Fixture::LateSplit& late) {
					return late.frame >= activation && (late.a == seat || late.b == seat || late.owner == seat);
				});
			};
			const uint64_t closingTrail = join.FindSession(12) ? join.FindSession(12)->activationTrailFrames : 0;
			// An observer the returner trailed past the trail its activation allowed, at any moment since the activation, may hold it for an
			// input that trailing made late, though the returner has caught up by the hold.
			std::array<bool, NetMatchConfigUtil::c_MaxPeerCount> trailedPast{};
			for (const uint64_t until = observationAt + 1000; fixture.hub.now <= until && !fixture.OwnTrouble(seat) && !lateSplitSince();) {
				for (uint8_t peer: survivors)
					if (fixture.worlds[peer - 1].applied > fixture.worlds[seat - 1].applied + closingTrail + c_TestBoundTicks) trailedPast[peer - 1] = true;
				for (uint8_t peer: survivors)
					if ((playingAtActivation & (1U << (peer - 1))) != 0 && fixture.worlds[peer - 1].applied >= activation && fixture.peers[peer - 1]->IsRunning() &&
					    !fixture.HoldFromOwnFault(peer, seat) &&
					    (fixture.peers[peer - 1]->HasHeldAISeat(seat) || Bridges(*fixture.peers[peer - 1]).contains(seat))) {
						// The round takes back a return its returner never closed on - still trailing it past the trail its activation allowed: as
						// the service's catch-up carries on toward the next one, the seat catches up in place and the return property judges its hold.
						if (trailedPast[peer - 1]) co_return fixture.CheckBound(error);
						co_return fail("the image return was held again during its healthy observation: observer=" + std::to_string(peer) +
						    " playing_at_activation=" + std::to_string((s_FrameMembers(*fixture.peers[donor - 1], activation) & (1U << (peer - 1))) != 0) +
						    " observer_members=" + std::to_string(s_FrameMembers(*fixture.peers[peer - 1], fixture.worlds[peer - 1].applied + 1)) +
						    " observer_applied=" + std::to_string(fixture.worlds[peer - 1].applied) + " activation=" + std::to_string(activation) + " now_ms=" + std::to_string(fixture.hub.now) +
						    " returner_running=" + std::to_string(fixture.peers[seat - 1]->IsRunning()) + " returner_aligned_wait=" + std::to_string(fixture.peers[seat - 1]->AwaitingAlignedStart()) +
						    " returner_starts=" + std::to_string(fixture.peers[seat - 1]->HasReceivedAllRemoteStarts()) + " returner_applied=" + std::to_string(fixture.worlds[seat - 1].applied) +
						    s_WaitState(*fixture.peers[seat - 1]));
					}
				co_await std::suspend_always{};
				if (!fixture.CheckBound(error)) co_return false;
			}
			// A hold the returner's own fault caused is judged with every other hold, by its cause and its return in time.
			const bool ownTrouble = fixture.OwnTrouble(seat);
			for (uint8_t peer: survivors) {
				// A seat that left the round has stopped and plays no part in it.
				if (fixture.hub.schedule && fixture.hub.schedule->Left(peer, fixture.hub.now)) continue;
				const bool playing = (playingAtActivation & (1U << (peer - 1))) != 0;
				if (playing && !fixture.HoldFromOwnFault(peer, seat) && (!fixture.peers[peer - 1]->IsRunning() || (!ownTrouble &&
				    (fixture.peers[peer - 1]->HasHeldAISeat(seat) || Bridges(*fixture.peers[peer - 1]).contains(seat)))))
					co_return fail("the image return was held again after its certified activation: observer=" + std::to_string(peer) +
					            " returner_state=" + std::to_string(static_cast<int>(fixture.peers[seat - 1]->GetState())) +
					            " starts_received=" + std::to_string(fixture.peers[seat - 1]->GetStats().startPacketsReceived));
				for (const auto& [frame, hash]: untouched.at(peer))
					if (fixture.worlds[peer - 1].hashes.at(frame) != hash) co_return fail("the ruling half rewrote displayed history during a private image return");
			}
			if (!fixture.CompareHistories(error) || !fixture.CheckBound(error)) co_return false;
			DiagnosticLine() << "[session-plane-image] authority_contract=local_hold_async_split returner=" << unsigned(seat)
			                 << " donor=" << unsigned(donor) << " activation=" << activation << " winner_history_untouched=1"
			                 << " healthy_observation_ms=" << fixture.hub.now - observationAt << " new_own_fault=" << fixture.OwnMachineTrouble(seat) << std::endl;
			co_return true;
		}
		bool ReturnThroughImage(Fixture& fixture, uint8_t seat, uint8_t donor, std::string& error) {
			auto task = ReadImageReturn(fixture, seat, donor, error);
			if (!task.Resume()) return false;
			while (!task.Done()) if (!fixture.Step(error) || !fixture.CheckBound(error) || !task.Resume()) return false;
			return true;
		}

		bool CheckImageActivation(uint8_t seats, uint32_t rttMs, std::string& error) {
			Case row{"image_activation"}; row.seats = seats; row.subject = 2; row.gapMs = 60000; row.frozen = row.internet = row.captureFrames = true;
			row.rttMs = row.otherRttMs = rttMs; row.checkpointEveryFrames = 60;
			Fixture fixture; fixture.parallel = true;
			if (!fixture.Start(row, error)) return false;
			while (fixture.hub.now < 9000 && (!fixture.peers[0]->HasHeldAISeat(2) || fixture.hub.now < 7000))
				if (!fixture.Step(error) || !fixture.CheckBound(error)) return false;
			if (!fixture.peers[0]->HasHeldAISeat(2)) { error = "authority ruling: the frozen seat did not enter AI hold without directory permission"; return false; }
			if (!ReturnThroughImage(fixture, 2, 1, error)) return false;
			for (const uint64_t until = fixture.hub.now + 9000; fixture.hub.now <= until;)
				if (!fixture.Step(error) || !fixture.CheckBound(error)) return false;
			return true;
		}
		bool CheckStartedSplitContract(Fixture& fixture, std::string& error, const std::function<bool(std::string&)>& observe = {}) {
			const Case row = fixture.hub.row;
			std::vector<bool> heldBeforeAnswer(row.seats), playedWhilePending(row.seats), won(row.seats), migrated(row.seats);
			std::vector<uint64_t> heldAtFrame(row.seats), stoppedAtFrame(row.seats);
			std::map<uint8_t, std::map<uint64_t, NetHash32>> winningHistory;
			for (const uint64_t until = fixture.hub.faultEnd + 16000; fixture.hub.now < until;) {
				if (!fixture.Step(error) || !fixture.CheckBound(error)) return false;
				if (observe && !observe(error)) return false;
				for (uint8_t seat = 1; seat <= row.seats; ++seat) {
					const size_t index = seat - 1;
					const auto& peer = *fixture.peers[index];
					const uint64_t frame = fixture.worlds[index].applied + 1;
					const uint32_t members = s_FrameMembers(peer, frame), held = s_HeldSeats(peer, frame);
					if (row.directMigration && fixture.hub.DuringFault() && fixture.hub.now >= fixture.hub.faultAt + 20000 && members) {
						const uint8_t expected = members & 1U ? 1 : static_cast<uint8_t>(std::countr_zero(members) + 1);
						migrated[index] = peer.IsRunning() && peer.GetHostPeerId() == expected && (expected == 1 || peer.GetMigrationResult().generation != 0);
					}
					const auto client = fixture.hub.directoryClients.find(seat);
					const bool pending = client == fixture.hub.directoryClients.end() || client->second.firstDecidedMs == UINT64_MAX;
					uint32_t absent = 0;
					for (uint8_t other = 1; other <= row.seats; ++other) if (fixture.hub.LiveSplit(seat, other)) absent |= 1U << (other - 1);
					if (absent && (held & absent) == absent && (members & (1U << index)) && pending) {
						heldBeforeAnswer[index] = true;
						if (!heldAtFrame[index]) heldAtFrame[index] = frame;
						playedWhilePending[index] = fixture.worlds[index].applied >= heldAtFrame[index];
					}
					if (peer.IsStopped() && !stoppedAtFrame[index]) stoppedAtFrame[index] = fixture.worlds[index].applied;
					if (client != fixture.hub.directoryClients.end()) for (const auto& answer: client->second.consumed)
						if (std::find(answer.reply.members.begin(), answer.reply.members.end(), seat) != answer.reply.members.end() && !won[index]) {
							won[index] = true; winningHistory[seat] = fixture.worlds[index].hashes;
						}
					if (!row.internet && fixture.directRuling && (fixture.directRuling & (1U << index)) && !won[index]) {
						won[index] = true; winningHistory[seat] = fixture.worlds[index].hashes;
					}
				}
				if (!fixture.RecoverImages(error)) return false;
			}
			if (!fixture.FinishImageReturns(error)) return false;
			bool lost = false;
			for (uint8_t seat = 1; seat <= row.seats; ++seat) {
				const size_t index = seat - 1;
				if (row.directMigration && !migrated[index]) { error = "direct ruling: the reachable group did not migrate within the hold window: seat=" + std::to_string(seat); return false; }
				if (!heldBeforeAnswer[index] || !playedWhilePending[index]) {
					error = "authority ruling: split peer " + std::to_string(seat) + " did not install its local hold and play before its arbitration"; return false;
				}
				// A half still querying as the smaller one stops on the ruling it reads like any loser: a ruling it consumed without it, and
				// the stop and image return that followed, are its answer.
				bool ruledOut = false;
				if (const auto client = fixture.hub.directoryClients.find(seat); client != fixture.hub.directoryClients.end())
					for (const auto& answer: client->second.consumed)
						ruledOut |= !answer.reply.members.empty() && std::find(answer.reply.members.begin(), answer.reply.members.end(), seat) == answer.reply.members.end();
				if (fixture.lostAt[index] || (ruledOut && stoppedAtFrame[index] && fixture.imageReturns[index]) ||
				    (!row.internet && fixture.directRuling && !(fixture.directRuling & (1U << index)))) {
					lost = true;
					if (!stoppedAtFrame[index] || !fixture.imageReturns[index] ||
					    (fixture.lostFrame[index] && stoppedAtFrame[index] + 1 != fixture.lostFrame[index])) {
						error = "authority ruling: losing peer " + std::to_string(seat) + " must stop at its decision frame and complete an image return" +
						    "; sampled_stop_frame=" + std::to_string(stoppedAtFrame[index]) + " decision_frame=" + std::to_string(fixture.lostFrame[index]) +
						    " decision_ms=" + std::to_string(fixture.lostAt[index]) + " image_stop_ms=" + std::to_string(fixture.imageAt[index]) +
						    " image_returns=" + std::to_string(fixture.imageReturns[index]); return false;
					}
				} else {
					if (!won[index]) { error = "authority ruling: a healed split never received its arbitration answer: seat " + std::to_string(seat); return false; }
					if (fixture.imageReturns[index] || stoppedAtFrame[index]) { error = "authority ruling: the ruling half stopped or replaced its world"; return false; }
					if (const auto saved = winningHistory.find(seat); saved != winningHistory.end())
						for (const auto& [frame, hash]: saved->second) if (fixture.worlds[index].hashes.at(frame) != hash) {
							error = "authority ruling: the ruling half rewrote its displayed history"; return false;
						}
				}
			}
			if (!lost) { error = "authority ruling: the split produced no losing half and no image return"; return false; }
			const uint64_t common = std::min_element(fixture.worlds.begin(), fixture.worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied;
			for (uint64_t frame = 1; frame <= common; ++frame) for (const auto& world: fixture.worlds)
				if (world.hashes.at(frame) != fixture.worlds[0].hashes.at(frame)) { error = "authority ruling: the completed image return did not adopt the ruling history"; return false; }
			if (const auto twice = fixture.TwoReturnFrames(); !twice.empty()) { error = twice; return false; }
			DiagnosticLine() << "[session-plane-split] authority_contract=local_hold_async_split both_halves_bound_ms=50 local_holds_before_reply=1"
			                 << " losing_half_stopped_at_decision=1 image_return=1 winner_history_untouched=1" << fixture.RefereeEvidence() << std::endl;
			return true;
		}
		bool CheckSplitContract(Case row, std::string& error) {
			row.captureFrames = true; row.checkpointEveryFrames = 60;
			Fixture fixture; fixture.parallel = true;
			return fixture.Start(row, error) && CheckStartedSplitContract(fixture, error);
		}
		bool CheckPlacementCombatReturn(std::string& error) {
			Case row{"placement_then_combat_image_return"}; row.seats = 3; row.subject = 1; row.gapMs = 1500;
			row.rosterStage = 2; row.captureFrames = true; row.checkpointEveryFrames = 60;
			Fixture fixture; fixture.parallel = true;
			if (!fixture.Start(row, error)) return false;
			// One continuous set of coordinators: a placement cut must neither assign any unplaced brain to AI nor replace history.
			while (fixture.hub.now < fixture.hub.faultEnd) {
				if (!fixture.Step(error)) return false;
				for (const auto& peer: fixture.peers) {
					if (!peer->WaitsForPlacement()) { error = "a peer bypassed the placement barrier"; return false; }
					for (uint8_t seat = 1; seat <= row.seats; ++seat) if (peer->HasHeldAISeat(seat)) { error = "an unplaced brain was assigned to AI"; return false; }
				}
			}
			const uint64_t resumed = std::max_element(fixture.worlds.begin(), fixture.worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied + 60;
			const uint64_t recoverUntil = fixture.hub.now + 4000;
			while (std::any_of(fixture.worlds.begin(), fixture.worlds.end(), [&](const World& world) { return world.applied < resumed; })) {
				if (fixture.hub.now >= recoverUntil) { error = "placement did not recover its original history"; return false; }
				if (!fixture.Step(error)) return false;
			}
			if (fixture.splitHistories || std::any_of(fixture.imageReturns.begin(), fixture.imageReturns.end(), [](uint64_t count) { return count != 0; })) {
				error = "placement recovery manufactured a second history or image"; return false;
			}
			for (const auto& peer: fixture.peers) for (uint8_t seat = 1; seat <= row.seats; ++seat)
				if (peer->HasHeldAISeat(seat)) { error = "placement recovery held an unplaced brain"; return false; }
			NetRosterEvent event{NetRosterEventKind::CombatStarted}; event.frame = resumed;
			auto combat = ApplyRosterEvent(fixture.agreedRoster, event);
			if (combat.refused) { error = "placement did not enter combat: " + combat.reason; return false; }
			fixture.agreedRoster = std::move(combat.roster);
			for (const auto& peer: fixture.peers) if (peer->WaitsForPlacement()) { error = "combat retained the placement guard"; return false; }
			// The completed placement fault has one equal history; the following combat cut is a new physical event.
			fixture.sawSplit = fixture.splitHistories = false; fixture.splitFrom = {}; fixture.directRuling = 0;
			std::fill(fixture.observedSplit.begin(), fixture.observedSplit.end(), 0);
			std::fill(fixture.splitParticipant.begin(), fixture.splitParticipant.end(), false);
			fixture.hub.row.gapMs = 8000; fixture.hub.row.blackout = fixture.hub.row.splitContract = true;
			fixture.hub.faultAt = fixture.hub.now + 1000; fixture.hub.faultEnd = fixture.hub.faultAt + 8000;
			std::vector<uint64_t> before; bool checkedPace = false;
			const bool passed = CheckStartedSplitContract(fixture, error, [&](std::string& why) {
				if (before.empty() && fixture.hub.now >= fixture.hub.faultAt) for (const auto& world: fixture.worlds) before.push_back(world.applied);
				if (!checkedPace && fixture.hub.now >= fixture.hub.faultAt + 7500) {
					checkedPace = true;
					for (uint8_t peer: {2, 3}) if (fixture.worlds[peer - 1].applied < before[peer - 1] + 440 ||
					    !fixture.peers[peer - 1]->IsSeatUnderAI(1, fixture.worlds[peer - 1].applied)) {
						why = "the placement-to-combat survivors did not retain their original 440-frame progress and host hold"; return false;
					}
				}
				return true;
			});
			if (passed && !checkedPace) { error = "the placement-to-combat progress check was not exercised"; return false; }
			if (passed) DiagnosticLine() << "[net-lockstep-selftest] PASS placement_barrier_releases_three_peer_host_blackout_hold_and_return no_placement_ai=1 combat_progress=440 bound_ms=50 actual_image=1" << std::endl;
			return passed;
		}
		bool CheckTransportRttOracle(std::string& error) {
			Hub hub; Schedule schedule; schedule.seats = 2;
			schedule.rttMs[1][2] = schedule.rttMs[2][1] = 100;
			schedule.jitterMs[1][2] = schedule.jitterMs[2][1] = 40;
			hub.schedule = &schedule;
			Wire sender(hub, 1), receiver(hub, 2);
			sender.connections[42] = {&receiver, 43}; receiver.connections[43] = {&sender, 42};
			uint32_t low = UINT32_MAX, high = 0;
			for (hub.now = 0; hub.now < 4000; hub.now += 10) {
				if (!sender.Send(42, NetTransportLane::InputUnreliable, {0x71, 0x39})) { error = "the transport oracle could not send"; return false; }
				const uint32_t rtt = sender.GetPeerPingMs(42);
				if (rtt < 100 || rtt > 180) { error = "the measured RTT escaped the physical two-way link"; return false; }
				low = std::min(low, rtt); high = std::max(high, rtt);
			}
			uint64_t digest = 14695981039346656037ULL;
			for (const auto& event: receiver.events) { digest ^= event.at; digest *= 1099511628211ULL; }
			DiagnosticLine() << "[session-plane-transport-rtt] low_ms=" << low << " high_ms=" << high
			    << " delivery_events=" << receiver.events.size() << " delivery_digest=" << digest << " application_polls=0";
			if (high <= 100 || high == low) { error = "injected physical jitter was hidden behind a constant measured RTT"; return false; }
			return true;
		}
		// A held seat that took its own input without the acceptor's receipt cannot know the acceptor took it: an exact impaired
		// receipt separates the two histories at its bound, exactly as a lost input does, and nothing weaker does.
		bool CheckReceiptHistoryOracle(std::string& error) {
			for (unsigned variant = 0; variant < 8; ++variant) {
				Fixture fixture; fixture.hub.row.seats = 2;
				fixture.worlds.resize(2);
				fixture.frameMembers = {{0, variant == 6 ? 1U : 3U}, {0, 2}};
				fixture.frameAuthors = {{0, 1}, {0, 2}};
				for (uint8_t peer = 1; peer <= 2; ++peer) {
					auto& world = fixture.worlds[peer - 1]; world.applied = 1; world.hashes[1] = {}; world.shownAt[1] = peer == 2 ? 150 : 100;
					if (peer == 2) world.hashes[1][0] = 1;
				}
				fixture.worlds[1].aiAt[1] = "1";
				NetHostMigrationMessage receipt; receipt.type = NetHostMigrationMessageType::PeerReceipt; receipt.roundId = 77;
				receipt.senderPeerId = 1; receipt.successorPeerId = variant == 4 ? 1 : 2; receipt.frame = variant == 7 ? 1 : 2;
				fixture.hub.now = variant == 3 ? 151 : 50;
				if (variant != 1) fixture.hub.ObserveInput(receipt, variant == 5 ? 1 : 2, true);
				if (variant == 2) { fixture.hub.now = 100; fixture.hub.ObserveInput(receipt, 2, false); }
				fixture.hub.now = 200;
				std::string why;
				const bool accepted = fixture.CompareHistories(why, true), expected = variant == 0;
				if (accepted != expected) {
					error = "causal receipt-history oracle variant=" + std::to_string(variant) + " expected_split=" + std::to_string(expected) + ": " + why; return false;
				}
				if (variant == 0 && (!fixture.InputSplitCausedHold(2, 150, 180, 1, 40, 60) || fixture.InputSplitCausedHold(2, 150, 180, 1, 51, 60))) {
					error = "a receipt split hold cause escaped its exact impaired link or fault interval"; return false;
				}
			}
			return true;
		}

		bool CheckReturnedReaderHistoryOracle(std::string& error) {
			// Seat 3 reads the host's half after seat 2 split from it (frame 1, authored by the host), then plays in that half itself (frame 2).
			for (unsigned variant = 0; variant < 4; ++variant) {
				Fixture fixture; fixture.hub.row.seats = 3;
				fixture.worlds.resize(3);
				fixture.frameMembers = {{0, 1, 5}, {0, 2, 2}, {0, 1, variant == 3 ? 1U : 5U}};
				fixture.frameAuthors = {{0, 1, 1}, {0, 2, 2}, {0, 1, 3}};
				for (uint8_t peer = 1; peer <= 3; ++peer) {
					auto& world = fixture.worlds[peer - 1]; world.applied = 2;
					for (uint64_t frame: {1, 2}) { world.hashes[frame] = {}; world.shownAt[frame] = 100 * frame; }
					if (peer == 2) { world.hashes[1][0] = 1; world.hashes[2][0] = 2; }
				}
				fixture.worlds[1].aiAt[1] = fixture.worlds[1].aiAt[2] = "1,3";
				if (variant != 1) fixture.splitFrom[1][2] = 1;
				// Negatives: no proven split; the two seats of one half differing; a frame whose members omit its own author.
				if (variant == 2) fixture.worlds[0].hashes[2][0] = 9;
				std::string why;
				const bool accepted = fixture.CompareHistories(why, true), expected = variant == 0;
				if (accepted != expected) {
					error = "returned-reader history oracle variant=" + std::to_string(variant) + " expected_split=" + std::to_string(expected) + ": " + why; return false;
				}
			}
			return true;
		}

		bool CheckInputHistoryOracle(std::string& error) {
			for (unsigned variant = 0; variant < 11; ++variant) {
				const bool thirdSource = variant == 8 || variant == 9;
				const uint8_t owner = thirdSource ? 3 : 1;
				Fixture fixture; fixture.hub.row.seats = thirdSource ? 3 : 2;
				fixture.worlds.resize(fixture.hub.row.seats);
				fixture.frameMembers = thirdSource ? std::vector<std::vector<uint32_t>>{{0, 7}, {0, 2}, {0, 7}} : std::vector<std::vector<uint32_t>>{{0, 3}, {0, 2}};
				fixture.frameAuthors = thirdSource ? std::vector<std::vector<uint8_t>>{{0, 1}, {0, 2}, {0, 3}} : std::vector<std::vector<uint8_t>>{{0, 1}, {0, 2}};
				for (uint8_t peer = 1; peer <= fixture.hub.row.seats; ++peer) {
					auto& world = fixture.worlds[peer - 1]; world.applied = 1; world.hashes[1] = {}; world.shownAt[1] = peer == 2 ? 150 : 100;
					if (peer == 2) world.hashes[1][0] = 1;
				}
				if (variant != 10) fixture.worlds[1].aiAt[1] = std::to_string(owner);
				if (variant == 5) fixture.frameAuthors[1][1] = 1;
				if (variant == 6) fixture.frameMembers[1][1] = 3;
				NetLockstepFrame input; input.senderPeerId = owner; input.roundId = 77; input.targetFrame = variant == 7 ? 2 : 1;
				ControllerFrame controls; controls.actorUniqueID = 1000 + owner; input.frames = {controls};
				std::vector<uint8_t> encoded;
				if (!NetLockstepCodec::EncodeRecoveryInput(input, encoded)) { error = "input-history oracle could not encode its exact source"; return false; }
				NetHostMigrationMessage packet; packet.type = NetHostMigrationMessageType::PeerInput;
				packet.senderPeerId = packet.successorPeerId = owner; packet.roundId = 77; packet.frame = input.targetFrame; packet.bytes = {1};
				for (unsigned shift = 0; shift < 32; shift += 8) packet.bytes.push_back(static_cast<uint8_t>(encoded.size() >> shift));
				packet.bytes.insert(packet.bytes.end(), encoded.begin(), encoded.end()); packet.totalBytes = packet.bytes.size();
				if (variant == 4) packet.bytes.pop_back();
				fixture.hub.now = variant == 3 ? 151 : 50;
				if (variant != 1) fixture.hub.ObserveInput(packet, 2, true);
				if (variant == 2) { fixture.hub.now = 100; fixture.hub.ObserveInput(packet, 2, false); }
				if (variant == 8) { fixture.hub.now = 75; fixture.hub.ObserveInput(packet, 1, false); }
				fixture.hub.now = 200;
				std::string why;
				const bool accepted = fixture.CompareHistories(why, true), expected = variant == 0 || variant == 8;
				if (accepted != expected) {
					error = "causal wire-history oracle variant=" + std::to_string(variant) + " expected_split=" + std::to_string(expected) + ": " + why; return false;
				}
				if (variant == 0) {
					NetFrameTieRequest request; request.frame = 0; NetFrameTieReply reply; reply.members = {2};
					if (!fixture.IsLosingDecision(1, request, reply) || fixture.IsLosingDecision(2, request, reply) || !fixture.HasSplitHistoryAt(2, 1) ||
					    fixture.HasSplitHistoryAt(0, 1) || fixture.HasSplitHistoryAt(3, 1) || fixture.HasSplitHistoryAt(1, 0)) {
						error = "a proven input-loss continuation did not select a discarded-branch image at its decision frame"; return false;
					}
					request.queryOnly = true;
					if (fixture.IsLosingDecision(1, request, reply)) { error = "a private query discarded live history"; return false; }
					if (!fixture.InputSplitCausedHold(1, 150, 180, 2, 40, 60) || !fixture.InputSplitCausedHold(2, 150, 180, 1, 40, 60) ||
					    fixture.InputSplitCausedHold(1, 150, 180, 3, 40, 60) || fixture.InputSplitCausedHold(3, 150, 180, 2, 40, 60) ||
					    fixture.InputSplitCausedHold(1, 151, 180, 2, 40, 60) || fixture.InputSplitCausedHold(1, 150, 149, 2, 40, 60) ||
					    fixture.InputSplitCausedHold(1, 150, 180, 2, 51, 60) || fixture.InputSplitCausedHold(1, 150, 180, 2, 40, 50) ||
					    fixture.InputSplitCausedHold(1, 49, 180, 2, 40, 60)) {
						error = "a split hold cause escaped its exact impaired link, fault interval or observed hold span"; return false;
					}
					for (size_t peer = 0; peer < 2; ++peer) {
						fixture.worlds[peer].applied = 2; fixture.worlds[peer].hashes[2] = {}; fixture.worlds[peer].shownAt[2] = 210;
						fixture.frameMembers[peer].push_back(3); fixture.frameAuthors[peer].push_back(peer + 1);
					}
					fixture.hub.now = 210;
					if (!fixture.CompareHistories(why, true)) { error = "a healed input partition rejected its common history"; return false; }
					fixture.compared = {};
					if (!fixture.CompareHistories(why, true)) { error = "a healed input partition lost its earlier causal history"; return false; }
					if (!fixture.HasSplitHistoryAt(1, 1) || fixture.HasSplitHistoryAt(1, 2)) { error = "a decision used causal history outside its split interval"; return false; }
					for (size_t peer = 0; peer < 2; ++peer) {
						fixture.worlds[peer].applied = 3; fixture.worlds[peer].hashes[3] = {}; fixture.worlds[peer].shownAt[3] = 220;
						fixture.frameMembers[peer].push_back(peer == 0 ? 3 : 2); fixture.frameAuthors[peer].push_back(peer + 1);
					}
					fixture.worlds[1].hashes[3][0] = 1; fixture.worlds[1].aiAt[3] = "1"; fixture.hub.now = 220;
					if (fixture.CompareHistories(why, true)) { error = "a healed input partition excused a later fork with no wire fault"; return false; }
				}
			}
			return true;
		}
		// The Pause and Wait policies are hidden in V1: no host can choose one, and no agreed config, a resumed save's included, carries one.
		bool CheckPauseNeverAgreed(std::string& error) {
			NetMatchServiceRequest request;
			request.host = true; request.slowPlayerPolicy = NetSlowPlayerPolicy::Pause;
			NetMatchConfig configured;
			if (!NetMatchService::BuildMatchConfig(request, 0x9F03, configured, &error)) { error = "a host request naming the Pause policy built no match: " + error; return false; }
			if (configured.slowPlayerPolicy != NetSlowPlayerPolicy::Substitute) { error = "a host request naming the Pause policy agreed it"; return false; }
			NetMatchConfig draft = NetMatchConfigUtil::MakeDefault(0x9F04);
			draft.slowPlayerPolicy = NetSlowPlayerPolicy::Pause;
			request.slowPlayerPolicy.reset(); request.hostDraft = draft;
			if (!NetMatchService::BuildMatchConfig(request, 0x9F04, configured, &error)) { error = "a host draft naming the Pause policy built no match: " + error; return false; }
			if (configured.slowPlayerPolicy != NetSlowPlayerPolicy::Substitute) { error = "a host draft naming the Pause policy agreed it"; return false; }
			NetMatchConfig resumed = NetMatchConfigUtil::MakeDefault(0x9F05);
			resumed.slowPlayerPolicy = NetSlowPlayerPolicy::Pause;
			const uint64_t savedRevision = resumed.configRevision;
			NetMatchConfigUtil::PrepareResumedConfig(resumed);
			if (resumed.slowPlayerPolicy != NetSlowPlayerPolicy::Substitute) { error = "a resumed save naming the Pause policy kept it"; return false; }
			if (resumed.configRevision != savedRevision + 1 || !NetMatchConfigUtil::ValidateLocalAlpha(resumed, &error)) {
				error = "a resumed save lost its new revision or its validity: " + error; return false;
			}
			NetMatchConfig legacy = NetMatchConfigUtil::MakeDefault(0x9F06);
			legacy.version = NetMatchConfigUtil::c_TimingOptionsVersion - 1; legacy.slowPlayerPolicy = NetSlowPlayerPolicy::Pause;
			legacy.slowPlayerBoundTicks = NetMatchConfigUtil::c_DefaultSlowPlayerBoundTicks;
			NetMatchConfigUtil::KeepOfferedSlowPlayerPolicy(legacy);
			if (legacy.slowPlayerPolicy != NetSlowPlayerPolicy::Pause) { error = "a legacy config without timing options was given a policy it cannot carry"; return false; }
			return true;
		}

		bool CheckAuthorityOracle(std::string& error) {
			Case authorityRow{"oracle_directory_services"}; authorityRow.seats = 2; authorityRow.internet = true;
			Fixture authority;
			if (!authority.Start(authorityRow, error)) return false;
			for (const auto& peer: authority.peers) if (!peer->GetConfig().hostChangeReferee || !peer->GetConfig().frameTieReferee) {
				error = "fixture internet session omitted one of the production directory referees"; return false;
			}
			// Main checks every extra grant again; ending catch-up cannot spend its leftover acceleration as ordinary time.
			FixtureTickCredit credit;
			credit.Accrue(2 * c_TestTickMs, 1, 3, true);
			if (!credit.MayGrant(true, true)) { error = "fixture lost its bounded accelerated grant"; return false; }
			credit.Consume(true);
			unsigned ordinary = 0;
			while (credit.MayGrant(false, false)) { credit.Consume(false); ++ordinary; }
			if (ordinary != 1) {
				error = "fixture spent accelerated credit after catch-up ended: ordinary_grants=" + std::to_string(ordinary) + " expected=1"; return false;
			}
			FixtureTickCredit fractional;
			fractional.Accrue(c_TestTickMs / 2, 1, 3, true); fractional.Consume(true);
			fractional.Accrue(c_TestTickMs / 2, 1, 3, false);
			if (!fractional.MayGrant(false, false)) { error = "fixture extra grant discarded fractional ordinary time"; return false; }
			fractional.Consume(false);
			if (fractional.MayGrant(false, false)) { error = "fixture counted its fractional ordinary time twice"; return false; }
			FixtureTickCredit awaitingReceipts;
			FixtureTickCredit polled;
			polled.Accrue(1, 1, 3, true);
			if (polled.NextOrdinaryTickInMs(1) != 16 || polled.NextOrdinaryTickInMs(0.25) != 15.75) {
				error = "fixture forecast an ordinary slot before its next eligible poll"; return false;
			}
			awaitingReceipts.Accrue(c_TestTickMs / 2, 1, 3, true);
			if (awaitingReceipts.MayGrant(true, false) || !awaitingReceipts.MayGrant(true, true)) {
				error = "fixture granted an extra tick without its receipt runway or lost an eligible grant"; return false;
			}
			awaitingReceipts.Accrue(c_TestTickMs / 2, 1, 3, true);
			if (!awaitingReceipts.MayGrant(true, false)) { error = "fixture receipt wait blocked an ordinary due tick"; return false; }
			const double budgetBefore = awaitingReceipts.accelerated;
			awaitingReceipts.Consume(true);
			if (awaitingReceipts.MayGrant(true, false) || awaitingReceipts.accelerated != budgetBefore - 1) {
				error = "fixture ordinary tick escaped the phase budget or manufactured receipt credit"; return false;
			}
			Hub fault;
			fault.row.subject = 1; fault.row.gapMs = 8000; fault.row.hung = true;
			fault.now = 7000; fault.faultEnd = 14000;
			const bool namedHungQueues = fault.ProcessHung(1) && !fault.Split(1, 2) && !fault.LiveSplit(1, 2);
			Schedule scheduledFault; scheduledFault.spells.push_back({1, Spell::Kind::Hung, 6000, 8000});
			fault.schedule = &scheduledFault;
			const bool scheduledHungQueues = fault.ProcessHung(1) && !fault.Split(1, 2) && !fault.LiveSplit(1, 2);
			scheduledFault.spells[0].kind = Spell::Kind::Silent;
			const bool actualCutRemains = !fault.ProcessHung(1) && fault.Split(1, 2) && fault.LiveSplit(1, 2);
			if (!namedHungQueues || !scheduledHungQueues || !actualCutRemains) {
				error = "fixture fault domains: named_hung_queues=" + std::to_string(namedHungQueues) +
				    " scheduled_hung_queues=" + std::to_string(scheduledHungQueues) + " actual_cut_remains=" + std::to_string(actualCutRemains);
				return false;
			}
			fault.schedule = nullptr;
			Wire sender(fault, 2), socket(fault, 1);
			sender.connections[42] = {&socket, 43}; socket.connections[43] = {&sender, 42};
			const std::vector<uint8_t> queuedPacket{0x71, 0x39};
			if (!sender.Send(42, NetTransportLane::ControlReliable, queuedPacket)) { error = "fixture could not send to the hung socket"; return false; }
			fault.now = fault.faultEnd;
			const auto queuedEvents = socket.PollEvents();
			if (queuedEvents.size() != 1 || queuedEvents.front().type != NetTransportEventType::PacketReceived || queuedEvents.front().bytes != queuedPacket) {
				error = "fixture discarded an incoming packet instead of leaving it in the hung process's socket"; return false;
			}
			Case donorRow{"oracle_donor_availability"}; donorRow.seats = 3; donorRow.subject = 1; donorRow.gapMs = 8000; donorRow.hung = true;
			Case startupRow{"oracle_measured_startup"}; startupRow.seats = 3; startupRow.delayStartup = true;
			Fixture loading;
			if (!loading.Start(startupRow, error)) return false;
			while (loading.hub.now < 140) {
				if (!loading.Step(error)) return false;
				for (const auto& peer: loading.peers) if (peer->GetAgreedStartRecord()) {
					error = "fixture published a completed startup while its measured loading work was still pending"; return false;
				}
			}
			while (loading.hub.now < 1000) if (!loading.Step(error)) return false;
			for (uint8_t seat = 1; seat <= startupRow.seats; ++seat) {
				const auto start = loading.peers[seat - 1]->GetAgreedStartRecord();
				if (!start || start->peerStartupParks[1] != 70 || start->peerStartupParks[2] != 140 || start->heldPeerMask ||
				    start->agreedFirstFrame < 1 + static_cast<uint64_t>(std::ceil(140 / c_TestTickMs)) ||
				    !loading.firstDisplayedAt[seat - 1] || loading.firstDisplayedAt[seat - 1] < 140 || !Bridges(*loading.peers[seat - 1]).empty()) {
					error = "fixture startup lost its actual loading durations, common boundary or human seats"; return false;
				}
			}
			Fixture donors;
			if (!donors.Start(donorRow, error)) return false;
			while (donors.hub.now < 500) if (!donors.Step(error)) return false;
			for (const auto& peer: donors.peers) {
				const auto start = peer->GetAgreedStartRecord();
				if (!start || start->publishedPeerMask != 7 || start->heldPeerMask != 0) {
					error = "fixture cold start: the production published-start protocol did not agree every human seat before play"; return false;
				}
			}
			donors.hub.now = donors.hub.faultAt + 100;
			if (donors.ImageDonorCanAnswer(1) || !donors.ImageDonorCanAnswer(2)) {
				error = "fixture image donor: a hung process cannot serve its image while a running survivor can"; return false;
			}
			if (donors.ImageReturnProposer(3) != 1) {
				error = "fixture image donor and reclaim proposer are distinct roles: seat3's image still needs seat1's playing-group proposal"; return false;
			}
			if (s_ReturnTransport(*donors.peers[1], 3) == c_InvalidNetPeerId) {
				error = "fixture reclaim transport: the playing successor's authenticated mesh route is not its old primary host route"; return false;
			}
			if (donors.ImageReturnDonors() != std::vector<uint8_t>{1, 2, 3}) {
				error = "fixture changed the default image donor order"; return false;
			}
			donors.hub.row.donorSeat = 3;
			if (donors.ImageReturnDonors() != std::vector<uint8_t>{3, 1, 2}) {
				error = "fixture requires donor3's actual tail traffic but never selects its configured image donor"; return false;
			}
			// A private replay sees the production committed-frame format only after its donor's transport delivers it.
			Hub imageLink; imageLink.row.seats = imageLink.row.subject = 2; imageLink.row.rttMs = imageLink.row.otherRttMs = 100;
			Wire imageDonor(imageLink, 1), imageReader(imageLink, 2);
			imageDonor.connections[42] = {&imageReader, 43}; imageReader.connections[43] = {&imageDonor, 42}; imageReader.keepImageTail = true;
			NetLockstepFrame imageFrame; imageFrame.senderPeerId = 1; imageFrame.roundId = 77; imageFrame.targetFrame = 11;
			ControllerFrame imageInput; imageInput.actorUniqueID = 73; imageInput.analogMoveX = 9; imageFrame.frames = {imageInput};
			std::vector<uint8_t> record, packed, imageBytes;
			if (!EncodeCommittedJoinFrame(imageFrame, record, &error)) return false;
			for (unsigned shift = 0; shift < 32; shift += 8) packed.push_back(static_cast<uint8_t>(record.size() >> shift));
			packed.insert(packed.end(), record.begin(), record.end());
			const auto chunk = MakeWorldTailChunk(77, packed);
			if (!NetLobbyProtocol::Encode({chunk}, imageBytes) || !imageDonor.Send(42, NetTransportLane::BulkUnreliable, imageBytes, &error)) return false;
			imageLink.now = 49; (void)imageReader.PollEvents();
			if (!imageReader.receivedImageTail.empty()) { error = "fixture image tail arrived before its link transit"; return false; }
			imageLink.now = 50; (void)imageReader.PollEvents();
			std::vector<uint8_t> partial; std::map<uint64_t, NetLockstepFrame> received;
			if (imageReader.receivedImageTail.size() != 1 || !ReadImageTail(imageReader.receivedImageTail.front(), 1, 77, 10, partial, received, error) ||
			    received.size() != 1 || received.at(11).frames.size() != 1 ||
			    ControllerFrameCodec::Encode(received.at(11).frames.front()) != ControllerFrameCodec::Encode(imageInput) || imageLink.tailsSentBy[1] != 1) {
				error = "fixture image replay did not consume its exact transported donor record"; return false;
			}
			std::string refusedTail;
			if (ReadImageTail(imageReader.receivedImageTail.front(), 2, 77, 10, partial, received, refusedTail)) {
				error = "fixture image replay accepted another donor"; return false;
			}
			// As the lobby does, a chunk of another round is dropped: nothing of it is read.
			std::map<uint64_t, NetLockstepFrame> otherRound;
			if (!ReadImageTail(imageReader.receivedImageTail.front(), 1, 78, 10, partial, otherRound, refusedTail) || !otherRound.empty()) {
				error = "fixture image replay read another round's chunk"; return false;
			}
			auto truncated = chunk; truncated.bytes.pop_back();
			if (ReadImageTail({1, NetTransportLane::BulkUnreliable, truncated}, 1, 77, 10, partial, received, refusedTail)) {
				error = "fixture image replay accepted a truncated datagram record"; return false;
			}
			Fixture fixture; fixture.hub.row.seats = 2; fixture.hub.row.internet = true;
			fixture.worlds.resize(2); fixture.frameMembers = {{0, 1}, {0, 2}}; fixture.frameAuthors = {{0, 1}, {0, 2}};
			for (auto& world: fixture.worlds) { world.applied = 1; world.hashes[1] = {}; }
			fixture.worlds[1].hashes[1][0] = 1;
			std::string why;
			if (fixture.CompareHistories(why, true)) { error = "fixture accepted different histories without a real split"; return false; }
			fixture.splitFrom[1][2] = 1;
			if (!fixture.CompareHistories(why, true)) { error = "fixture rejected two actually separated, disjoint playing groups: " + why; return false; }
			fixture.compared = {}; fixture.frameMembers[0][1] = fixture.frameMembers[1][1] = 3;
			if (fixture.CompareHistories(why, true)) { error = "fixture excused disagreement between mutually included playing authors"; return false; }
			fixture.frameMembers[0][1] = fixture.frameMembers[1][1] = 1;
			if (fixture.CompareHistories(why, true)) { error = "fixture excused disagreement inside one half"; return false; }
			fixture.frameMembers[1][1] = 2; fixture.hub.row.internet = false;
			// The revised direct contract permits two continuations before the heal; it still requires the actual losing image to adopt one history.
			if (!fixture.CompareHistories(why, true)) { error = "fixture rejected independent direct continuations before heal: " + why; return false; }
			fixture.compared = {}; fixture.frameAuthors[1][1] = 1;
			if (fixture.CompareHistories(why, true)) { error = "fixture excused different hashes in an image's adopted author history"; return false; }
			fixture.frameAuthors[1][1] = 2;
			fixture.worlds[1].hashes[1] = fixture.worlds[0].hashes[1];
			if (!fixture.CompareHistories(why, true)) { error = "fixture rejected the adopted image history"; return false; }
			// At a real cut one local hold may precede the other half consuming its inputs already in flight.
			Fixture causal; causal.hub.row.seats = 3; causal.worlds.resize(3);
			causal.frameMembers = {{0, 1}, {0, 7}, {0, 7}}; causal.frameAuthors = {{0, 1}, {0, 2}, {0, 3}};
			causal.ObserveSplit(1, 2); causal.ObserveSplit(1, 3);
			for (auto& world: causal.worlds) { world.applied = 1; world.hashes[1] = {}; }
			causal.worlds[1].hashes[1][0] = causal.worlds[2].hashes[1][0] = 1;
			const bool asymmetricHold = causal.CompareHistories(why, true);
			// Two cut authors can still include each other while one holds a third sender and the other consumes its buffered input.
			Fixture overlap; overlap.hub.row.seats = 3; overlap.worlds.resize(3);
			overlap.frameMembers = {{0, 7}, {0, 6}, {0, 7}}; overlap.frameAuthors = {{0, 1}, {0, 2}, {0, 3}};
			overlap.ObserveSplit(1, 2); overlap.ObserveSplit(2, 3);
			for (auto& world: overlap.worlds) { world.applied = 1; world.hashes[1] = {}; }
			overlap.worlds[1].hashes[1][0] = 1;
			const bool asymmetricThirdSender = overlap.CompareHistories(why, true);
			// The connected half still has one history, even while the cut pair's installed masks overlap.
			causal.compared = {}; causal.compared[1][2] = causal.compared[1][3] = 1;
			causal.worlds[2].hashes[1][0] = 2;
			const bool connectedHalfRejected = !causal.CompareHistories(why, true) && why.find("peer 2 members=7 peer 3 members=7") != std::string::npos;
			Fixture frontier; frontier.worlds.resize(2); frontier.worlds[0].applied = 7; frontier.worlds[1].applied = 9;
			frontier.ObserveSplit(1, 2);
			const bool sharedPrefix = frontier.splitFrom[1][2] == 8;
			fixture.compared = {}; fixture.worlds[1].hashes[1][0] = 1; fixture.splitFrom[1][2] = 2;
			const bool priorHistoryRejected = !fixture.CompareHistories(why, true);
			fixture.playingMaxWait = {0, 51}; fixture.waitAt = {0, 0}; fixture.waitPlaying = {true, true};
			fixture.hub.row.internet = fixture.hub.row.directoryDown = true;
			if (fixture.CheckBound(why)) { error = "fixture waived a playing half's bound during a directory outage"; return false; }
			fixture.playingMaxWait[1] = 50;
			if (!fixture.CheckBound(why)) { error = "fixture rejected a wait exactly at the agreed bound"; return false; }
			NetFrameTieRequest request; request.queryOnly = true;
			NetFrameTieReply reply; reply.state = NetFrameTieReply::State::Decided; reply.members = {1};
			const bool readerIsNotLoser = !IsLosingContinuationDecision(true, 2, request, reply);
			request.queryOnly = false;
			const bool liveLoser = IsLosingContinuationDecision(true, 2, request, reply);
			const bool nonSplit = !IsLosingContinuationDecision(false, 2, request, reply);
			const bool winner = !IsLosingContinuationDecision(true, 1, request, reply);
			fixture.playingMaxWait = {0, 0}; fixture.waitAt = {100, 0}; fixture.waitPlaying = {true, false}; fixture.hub.now = 105;
			fixture.ObserveBoundRole(0, false); fixture.hub.now = 155;
			const bool endedAtHold = !fixture.waitPlaying[0] && fixture.waitAt[0] == 100 && fixture.playingMaxWait[0] == 5 && fixture.CheckBound(why);
			fixture.playingMaxWait = {0, 0}; fixture.waitAt = {100, 0}; fixture.waitPlaying = {true, false}; fixture.hub.now = 151;
			fixture.ObserveBoundRole(0, false);
			const bool overrunRetained = !fixture.CheckBound(why);
			const auto ownPause = [&](uint64_t begins) {
				fixture.hub.row.subject = 1; fixture.hub.row.frozen = true;
				fixture.hub.faultAt = fixture.hub.now = begins; fixture.hub.faultEnd = begins + 1000;
				fixture.playingMaxWait = {0, 0}; fixture.waitAt = {100, 0}; fixture.waitPlaying = {true, false};
				fixture.maxWait = fixture.returnedAt = {0, 0}; fixture.waits.clear();
				fixture.ObserveBoundRole(0, true); fixture.hub.now = begins + 100;
				const bool bounded = fixture.CheckBound(why);
				const bool segment = !fixture.waitPlaying[0] && fixture.waitAt[0] == 100 && fixture.playingMaxWait[0] == begins - 100;
				fixture.FinishWait(1, 1);
				const bool raw = fixture.maxWait[0] == begins && fixture.waits.size() == 1 && !fixture.waits[0].playing;
				return segment && raw && bounded == (begins <= 150);
			};
			const bool ownPauseEndsPlaying = ownPause(110), ownPauseKeepsOverrun = ownPause(151);
			DiagnosticLine() << "[session-plane-oracle-causality] asymmetric_local_hold=" << asymmetricHold
			    << " asymmetric_third_sender=" << asymmetricThirdSender << " own_pause_ends_playing=" << ownPauseEndsPlaying
			    << " own_pause_keeps_51ms=" << ownPauseKeepsOverrun
			    << " first_shared_prefix=" << sharedPrefix << " connected_half_rejected=" << connectedHalfRejected << " prior_history_rejected=" << priorHistoryRejected;
			if (!readerIsNotLoser || !liveLoser || !nonSplit || !winner || !endedAtHold || !overrunRetained ||
			    !asymmetricHold || !asymmetricThirdSender || !ownPauseEndsPlaying || !ownPauseKeepsOverrun ||
			    !sharedPrefix || !connectedHalfRejected || !priorHistoryRejected) {
				error = "fixture role classification: private_query_is_not_a_loser=" + std::to_string(readerIsNotLoser) +
				    " actual_loser_checked=" + std::to_string(liveLoser) + " nonsplit_unchanged=" + std::to_string(nonSplit) + " winner_unchanged=" + std::to_string(winner) +
				    " playing_wait_ends_at_hold=" + std::to_string(endedAtHold) + " actual_51ms_overrun_rejected=" + std::to_string(overrunRetained) +
				    " asymmetric_local_hold=" + std::to_string(asymmetricHold) + " first_shared_prefix=" + std::to_string(sharedPrefix) +
				    " asymmetric_third_sender=" + std::to_string(asymmetricThirdSender) + " own_pause_ends_playing=" + std::to_string(ownPauseEndsPlaying) +
				    " own_pause_keeps_51ms=" + std::to_string(ownPauseKeepsOverrun) +
				    " connected_half_rejected=" + std::to_string(connectedHalfRejected) + " prior_history_rejected=" + std::to_string(priorHistoryRejected); return false;
			}
			return true;
		}

		// Old: a bridge at 57054 deferred AI until 57175. The ruling requires AI at the installed hold boundary itself,
		// with the same ownership in the private reader and survivor, regardless of when their certificates arrive.
		bool CheckHostHoldAuthority(std::string& error) {
			Case row{"host_hold_ai_authority_is_the_certified_frames"};
			row.subject = 1; row.seats = 2; row.gapMs = 20000; row.internet = true;
			row.slowPercent = 90; row.rttMs = row.otherRttMs = 30;
			Fixture fixture;
			if (!fixture.Start(row, error)) return false;
			for (; fixture.hub.now < 38000;) {
				if (!fixture.Step(error)) return false;
				const auto host = Bridges(*fixture.peers[0]), survivor = Bridges(*fixture.peers[1]);
				if (!host.contains(1) || !survivor.contains(1)) continue;
				const auto& changes = fixture.hub.bridgeBoundaries[1];
				if (changes.empty() || !changes.begin()->second) continue;
				const uint64_t boundary = changes.begin()->first;
				for (const auto& peer: fixture.peers) {
					if (peer->IsSeatUnderAI(1, boundary - 1) || !peer->IsSeatUnderAI(1, boundary) || peer->AiProducerOf(1, boundary) != 2) {
						error = "authority ruling: AI ownership must start at the certified hold frame, not two seconds later, frame=" + std::to_string(boundary);
						return false;
					}
				}
				return true;
			}
			error = "the two seats did not certify the host's hold before the AI boundary";
			return false;
		}
		struct Finding { std::string property, kind, detail; };
		// How far a seat's machine falls behind the round in a spell, in the round's milliseconds.
		uint64_t BacklogMs(const Spell& spell) {
			if (spell.kind == Spell::Kind::Late) return spell.percent;
			if (spell.kind == Spell::Kind::Stall) return 0;
			return spell.kind == Spell::Kind::Slow ? spell.ms * (100 - std::min<uint32_t>(spell.percent, 100)) / 100 : spell.ms;
		}
		// What a spell leaves a seat behind by from `fromMs` on: the part of it after a rewind to an image.
		uint64_t BacklogMsFrom(const Spell& spell, uint64_t fromMs) {
			if (fromMs <= spell.atMs) return BacklogMs(spell);
			if (fromMs >= spell.EndMs()) return 0;
			Spell rest = spell; rest.atMs = fromMs; rest.ms = spell.EndMs() - fromMs;
			return BacklogMs(rest);
		}
		// How long a seat takes to close a backlog at its catch-up rate; never, at the round's own rate.
		uint64_t CatchUpMs(uint64_t backlogMs, uint32_t capacity) { return backlogMs == 0 ? 0 : capacity <= 100 ? UINT64_MAX : backlogMs * 100 / (capacity - 100); }
		// Long enough to see every held seat back and playing: past the last trouble, each seat's catch-up and its allowance, and six
		// seconds more to see it stay.
		uint64_t ScheduleEndMs(const Schedule& schedule) {
			uint64_t end = 12000;
			for (const auto& spell: schedule.spells) {
				const uint64_t catchUp = CatchUpMs(BacklogMs(spell), schedule.recoverPercent[spell.seat]);
				end = std::max(end, spell.EndMs() + (catchUp == UINT64_MAX ? 0 : catchUp) + c_NetSeatCatchUpAllowanceMs + 6000);
				// A seat outside the group that keeps the round falls behind by the time the split lasts and catches up at three times.
				if (spell.kind == Spell::Kind::Silent || spell.kind == Spell::Kind::Hung || spell.kind == Spell::Kind::Stopped)
					end = std::max(end, spell.EndMs() + spell.ms / 2 + c_NetSeatCatchUpAllowanceMs + 6000);
			}
			for (const auto& burst: schedule.bursts) end = std::max(end, burst.atMs + burst.ms + 6000);
			if (schedule.leaver) end = std::max(end, schedule.leaveAtMs + 6000);
			if (schedule.kicked) end = std::max(end, schedule.kickAtMs + 6000);
			if (schedule.rematchAtMs) end = std::max(end, schedule.rematchAtMs + 300 + 6000);
			for (const auto& cut: schedule.cuts) end = std::max(end, cut.atMs + cut.ms + cut.ms / 2 + c_NetSeatCatchUpAllowanceMs + 6000);
			return end;
		}
		const char* SpellWord(Spell::Kind kind) {
			return kind == Spell::Kind::Slow ? "slow" : kind == Spell::Kind::Stopped ? "stopped" : kind == Spell::Kind::Silent ? "silent" : kind == Spell::Kind::Hung ? "hung" :
			       kind == Spell::Kind::Late ? "late" : "stall";
		}
		// A schedule as one line of text, the form a fixed row keeps and ParseSchedule reads back.
		std::string FormatSchedule(const Schedule& schedule) {
			std::string text = "seed=" + std::to_string(schedule.seed) + " seats=" + std::to_string(schedule.seats) + " internet=" + std::to_string(schedule.internet) +
			                   " exchange=" + std::to_string(schedule.directoryExchangeMs) + " end=" + std::to_string(schedule.endMs);
			for (const char* name: {"rtt", "jitter"}) {
				text += std::string(" ") + name + "=";
				for (uint8_t a = 1; a <= schedule.seats; ++a) for (uint8_t b = a + 1; b <= schedule.seats; ++b)
					text += (a == 1 && b == 2 ? "" : ",") + std::to_string((name[0] == 'r' ? schedule.rttMs : schedule.jitterMs)[a][b]);
			}
			for (const char* name: {"recover", "phase"}) {
				text += std::string(" ") + name + "=";
				for (uint8_t seat = 1; seat <= schedule.seats; ++seat) text += (seat == 1 ? "" : ",") + std::to_string((name[0] == 'r' ? schedule.recoverPercent : schedule.allowancePhaseMs)[seat]);
			}
			for (const auto& spell: schedule.spells) {
				text += " spell=" + std::to_string(spell.seat) + ":" + SpellWord(spell.kind) + ":" + std::to_string(spell.atMs) + ":" + std::to_string(spell.ms);
				if (spell.kind == Spell::Kind::Slow || spell.kind == Spell::Kind::Late || spell.kind == Spell::Kind::Stall) text += ":" + std::to_string(spell.percent);
				if (spell.kind == Spell::Kind::Silent && spell.directoryDark) text += ":dark";
			}
			for (const auto& burst: schedule.bursts)
				text += " burst=" + std::to_string(burst.seat) + ":" + std::to_string(burst.atMs) + ":" + std::to_string(burst.ms) + ":" + std::to_string(burst.percent);
			if (schedule.leaver) text += " leave=" + std::to_string(schedule.leaver) + ":" + std::to_string(schedule.leaveAtMs);
			for (const auto& cut: schedule.cuts)
				text += " cut=" + std::to_string(cut.side) + ":" + std::to_string(cut.atMs) + ":" + std::to_string(cut.ms) + ":" + std::to_string(cut.directoryDark);
			if (schedule.kicked) text += " kick=" + std::to_string(schedule.kicked) + ":" + std::to_string(schedule.kickAtMs);
			if (schedule.rematchAtMs) text += " rematch=" + std::to_string(schedule.rematchAtMs);
			return text;
		}
		bool ParseSchedule(std::string_view text, Schedule& schedule) {
			schedule = {};
			const auto numbers = [](std::string_view list) {
				std::vector<uint64_t> values; size_t at = 0;
				while (at <= list.size()) {
					const size_t end = std::min(list.find_first_of(",:", at), list.size());
					values.push_back(std::strtoull(std::string(list.substr(at, end - at)).c_str(), nullptr, 10)); at = end + 1;
				}
				return values;
			};
			std::vector<std::pair<std::string, std::string>> fields;
			for (size_t at = 0; at < text.size();) {
				const size_t end = std::min(text.find(' ', at), text.size());
				const auto token = text.substr(at, end - at); at = end + 1;
				const size_t equals = token.find('=');
				if (token.empty()) continue;
				if (equals == std::string_view::npos) return false;
				fields.emplace_back(std::string(token.substr(0, equals)), std::string(token.substr(equals + 1)));
			}
			for (const auto& [key, value]: fields) if (key == "seats") schedule.seats = static_cast<uint8_t>(std::clamp<uint64_t>(numbers(value).front(), 2, 4));
			for (const auto& [key, value]: fields) {
				const auto list = numbers(value);
				if (key == "seed") schedule.seed = list.front();
				else if (key == "internet") schedule.internet = list.front() != 0;
				else if (key == "exchange") schedule.directoryExchangeMs = static_cast<uint32_t>(list.front());
				else if (key == "end") schedule.endMs = list.front();
				else if (key == "rtt" || key == "jitter") {
					size_t index = 0;
					for (uint8_t a = 1; a <= schedule.seats; ++a) for (uint8_t b = a + 1; b <= schedule.seats; ++b, ++index) {
						if (index >= list.size()) return false;
						(key == "rtt" ? schedule.rttMs : schedule.jitterMs)[a][b] = (key == "rtt" ? schedule.rttMs : schedule.jitterMs)[b][a] = static_cast<uint32_t>(list[index]);
					}
				} else if (key == "recover" || key == "phase") {
					for (uint8_t seat = 1; seat <= schedule.seats; ++seat) {
						if (seat > list.size()) return false;
						(key == "recover" ? schedule.recoverPercent : schedule.allowancePhaseMs)[seat] = static_cast<uint32_t>(list[seat - 1]);
					}
				} else if (key == "spell") {
					Spell spell; const size_t first = value.find(':'), second = value.find(':', first + 1);
					if (first == std::string::npos || second == std::string::npos) return false;
					const std::string word = value.substr(first + 1, second - first - 1);
					spell.kind = word == "slow" ? Spell::Kind::Slow : word == "stopped" ? Spell::Kind::Stopped : word == "silent" ? Spell::Kind::Silent : word == "hung" ? Spell::Kind::Hung :
					             word == "late" ? Spell::Kind::Late : Spell::Kind::Stall;
					if (word != "slow" && word != "stopped" && word != "silent" && word != "hung" && word != "late" && word != "stall") return false;
					const auto times = numbers(value.substr(second + 1));
					spell.seat = static_cast<uint8_t>(list.front()); spell.atMs = times.at(0); spell.ms = times.at(1);
					if (spell.kind == Spell::Kind::Slow || spell.kind == Spell::Kind::Late || spell.kind == Spell::Kind::Stall) spell.percent = static_cast<uint32_t>(times.at(2));
					spell.directoryDark = value.ends_with(":dark");
					if (spell.seat < 1 || spell.seat > schedule.seats) return false;
					schedule.spells.push_back(spell);
				} else if (key == "cut") {
					if (list.size() < 3) return false;
					schedule.cuts.push_back({static_cast<uint32_t>(list[0]), list[1], list[2], list.size() > 3 ? static_cast<uint32_t>(list[3]) : 0U});
				} else if (key == "rematch") {
					schedule.rematchAtMs = list.front();
				} else if (key == "kick") {
					if (list.size() != 2 || list[0] < 2 || list[0] > schedule.seats) return false;
					schedule.kicked = static_cast<uint8_t>(list[0]); schedule.kickAtMs = list[1];
				} else if (key == "leave") {
					if (list.size() != 2 || list[0] < 2 || list[0] > schedule.seats) return false;
					schedule.leaver = static_cast<uint8_t>(list[0]); schedule.leaveAtMs = list[1];
				} else if (key == "burst") {
					if (list.size() != 4 || list[0] < 1 || list[0] > schedule.seats) return false;
					schedule.bursts.push_back({static_cast<uint8_t>(list[0]), list[1], list[2], static_cast<uint32_t>(list[3])});
				} else if (key != "seats") return false;
			}
			return true;
		}
		// A schedule's shape as a row name: its seats, links and directory, each seat's spell and how it recovers, and its loss.
		std::string NameSchedule(const Schedule& schedule) {
			static const char* const counts[] = {"", "", "two", "three", "four"};
			uint32_t rtt = 0, jitter = 0;
			for (uint8_t a = 1; a <= schedule.seats; ++a) for (uint8_t b = a + 1; b <= schedule.seats; ++b) { rtt = std::max(rtt, schedule.rttMs[a][b]); jitter = std::max(jitter, schedule.jitterMs[a][b]); }
			std::string name = std::string(counts[schedule.seats]) + (schedule.internet ? "_internet" : "_direct") + "_seats_" + std::to_string(rtt) + "ms";
			if (jitter) name += "_jitter_" + std::to_string(jitter) + "ms";
			if (schedule.internet && schedule.directoryExchangeMs != 100) name += "_directory_" + std::to_string(schedule.directoryExchangeMs) + "ms";
			for (const auto& spell: schedule.spells) {
				name += std::string("_") + (spell.seat == 1 ? "host" : "seat" + std::to_string(spell.seat)) + "_" + SpellWord(spell.kind);
				if (spell.kind == Spell::Kind::Slow) name += "_" + std::to_string(spell.percent) + "pc";
				if (spell.kind == Spell::Kind::Late || spell.kind == Spell::Kind::Stall) name += "_" + std::to_string(spell.percent) + "ms_more";
				if (spell.kind == Spell::Kind::Silent && spell.directoryDark) name += "_dark";
				name += "_" + std::to_string(spell.ms) + "ms";
				if (schedule.recoverPercent[spell.seat] != 300) name += "_back_at_" + std::to_string(schedule.recoverPercent[spell.seat]) + "pc";
			}
			for (const auto& burst: schedule.bursts) name += "_loss_" + std::to_string(burst.percent) + "pc_" + (burst.seat == 1 ? std::string("host") : "seat" + std::to_string(burst.seat));
			if (schedule.leaver) name += "_seat" + std::to_string(schedule.leaver) + "_leaves";
			for (const auto& cut: schedule.cuts) {
				name += "_split";
				for (uint8_t seat = 1; seat <= schedule.seats; ++seat) if ((cut.side >> (seat - 1)) & 1U) name += std::to_string(seat);
				name += "_from_the_rest_" + std::to_string(cut.ms) + "ms";
				if (cut.directoryDark) name += "_some_dark";
			}
			if (schedule.kicked) name += "_seat" + std::to_string(schedule.kicked) + "_kicked";
			if (schedule.rematchAtMs) name += "_rematch";
			return name;
		}
		struct Random {
			uint64_t state;
			uint64_t Next() { uint64_t z = (state += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
			uint64_t Below(uint64_t count) { return Next() % count; }
			uint64_t Between(uint64_t low, uint64_t high) { return low + Below(high - low + 1); }
			bool Chance(unsigned percent) { return Below(100) < percent; }
			template<class T> T Pick(std::initializer_list<T> values) { return *(values.begin() + Below(values.size())); }
		};
		// A random round of trouble from its seed: two to four seats, each link's round trip and jitter, one to three spells - a machine
		// slowed to 50-95 % of the round's rate, its loop stopped with its link alive, its links silent, or its process hung - on any seat,
		// the host's included, two at once at times, each machine back at 100, 105, 120 or 300 % of the round's rate, and loss bursts.
		Schedule GenerateSchedule(uint64_t seed) {
			Random random{seed * 0x2545F4914F6CDD1DULL + 1};
			Schedule schedule; schedule.seed = seed;
			schedule.seats = random.Pick<uint8_t>({2, 2, 3, 3, 4, 4, 4});
			schedule.internet = random.Chance(70);
			const uint32_t round = random.Pick<uint32_t>({20, 50, 50, 100, 100, 200, 200, 250, 250});
			for (uint8_t a = 1; a <= schedule.seats; ++a) for (uint8_t b = a + 1; b <= schedule.seats; ++b) {
				const uint32_t rtt = static_cast<uint32_t>(round * random.Between(80, 120) / 100);
				const uint32_t jitter = std::min(rtt / 2, random.Pick<uint32_t>({0, 0, 0, 5, 10, 20, 40}));
				schedule.rttMs[a][b] = schedule.rttMs[b][a] = rtt; schedule.jitterMs[a][b] = schedule.jitterMs[b][a] = jitter;
			}
			schedule.directoryExchangeMs = random.Chance(4) ? 300 : random.Pick<uint32_t>({40, 100, 160, 240});
			for (uint8_t seat = 1; seat <= schedule.seats; ++seat) {
				schedule.recoverPercent[seat] = random.Pick<uint32_t>({100, 105, 120, 300, 300});
				schedule.allowancePhaseMs[seat] = static_cast<uint32_t>(random.Below(5100));
			}
			const unsigned spells = static_cast<unsigned>(random.Between(1, 3));
			for (unsigned index = 0; index < spells; ++index) {
				Spell spell;
				spell.seat = random.Chance(25) ? 1 : static_cast<uint8_t>(random.Between(1, schedule.seats));
				const unsigned kind = static_cast<unsigned>(random.Below(100));
				spell.kind = kind < 30 ? Spell::Kind::Slow : kind < 48 ? Spell::Kind::Stopped : kind < 73 ? Spell::Kind::Silent : kind < 85 ? Spell::Kind::Hung : Spell::Kind::Late;
				spell.atMs = index > 0 && !schedule.spells.empty() && random.Chance(30) ? schedule.spells.back().atMs + random.Below(500) : random.Between(4000, 10000);
				spell.ms = spell.kind == Spell::Kind::Slow ? random.Between(1000, 12000) : random.Pick<uint64_t>({200, 400, 800, 1500, 3000, 6000}) * random.Between(80, 120) / 100;
				spell.percent = spell.kind == Spell::Kind::Slow ? static_cast<uint32_t>(random.Between(50, 95)) : spell.kind == Spell::Kind::Late ? random.Pick<uint32_t>({50, 100, 200, 400}) : 100;
				spell.directoryDark = spell.kind == Spell::Kind::Silent && random.Chance(50);
				// A machine that catches up only a little faster than the round is given a backlog it closes within fifteen seconds.
				const uint32_t capacity = schedule.recoverPercent[spell.seat];
				if (capacity > 100 && capacity < 300) {
					const uint64_t most = 15000 * (capacity - 100) / 100;
					if (BacklogMs(spell) > most) spell.ms = spell.kind == Spell::Kind::Slow ? most * 100 / (100 - spell.percent) : most;
				}
				spell.ms = std::max<uint64_t>(spell.ms, 100);
				const bool overlaps = std::any_of(schedule.spells.begin(), schedule.spells.end(), [&](const Spell& other) {
					return other.seat == spell.seat && spell.atMs < other.EndMs() + 1000 && other.atMs < spell.EndMs() + 1000;
				});
				if (!overlaps) schedule.spells.push_back(spell);
			}
			// A joiner leaves at times, its seat kept by the AI for the rest of the round.
			if (random.Chance(8)) {
				schedule.leaver = static_cast<uint8_t>(random.Between(2, schedule.seats)); schedule.leaveAtMs = random.Between(6000, 14000);
				std::erase_if(schedule.spells, [&](const Spell& spell) { return spell.seat == schedule.leaver && spell.EndMs() >= schedule.leaveAtMs; });
			}
			// A rematch at times once the spells are over, and a spell in the next round.
			if (random.Chance(8) && !schedule.leaver) {
				uint64_t over = 8000;
				for (const auto& spell: schedule.spells) over = std::max(over, spell.EndMs());
				schedule.rematchAtMs = over + random.Between(2000, 5000);
				Spell next; next.seat = static_cast<uint8_t>(random.Between(1, schedule.seats));
				next.kind = random.Pick<Spell::Kind>({Spell::Kind::Silent, Spell::Kind::Stopped, Spell::Kind::Slow});
				next.atMs = schedule.rematchAtMs + 300 + random.Between(3000, 6000);
				next.ms = next.kind == Spell::Kind::Slow ? random.Between(2000, 6000) : random.Between(300, 2000);
				next.percent = next.kind == Spell::Kind::Slow ? static_cast<uint32_t>(random.Between(50, 90)) : 100;
				if (schedule.recoverPercent[next.seat] > 100 && schedule.recoverPercent[next.seat] < 300) schedule.recoverPercent[next.seat] = 300;
				schedule.spells.push_back(next);
			}
			if (random.Chance(30)) for (uint64_t count = random.Between(1, 2); count > 0; --count)
				schedule.bursts.push_back({static_cast<uint8_t>(random.Between(1, schedule.seats)), random.Between(4000, 12000), random.Between(300, 3000), random.Pick<uint32_t>({10, 30, 60, 90})});
			schedule.endMs = ScheduleEndMs(schedule);
			return schedule;
		}
		// Authority contract: equal history within each installed playing group; every playing peer keeps the local bound, even
		// without a majority or a directory; only an actual losing split stops for arbitration, then returns through an image.
		// Nonsplit holds retain the existing own-cause, catch-up deadline, single-return and no-rehold properties.
		std::optional<Finding> RunSchedule(const Schedule& schedule, Fixture& fixture) {
			Case row{"random_hold_schedule"};
			// FLAGGED: any scheduled hold can require an image, including loss or slowdown.
			row.seats = schedule.seats; row.internet = schedule.internet; row.directoryExchangeMs = schedule.directoryExchangeMs; row.captureFrames = true;
			auto& hub = fixture.hub;
			hub.schedule = &schedule; hub.random ^= schedule.seed * 0xD1B54A32D192ED03ULL;
			std::string error;
			const auto failed = [](const std::string& why) {
				if (why.find("is not AI-owned") != std::string::npos || why.find("HoldAtFrame requires empty input") != std::string::npos)
					return Finding{"rules", "hold_not_immediate_ai", why};
				if (why.starts_with("authority ruling: playing peer")) return Finding{"bound", "survivor_waits", why};
				if (why.starts_with("authority ruling: losing peer")) return Finding{"rules", "loser_did_not_stop_at_decision", why};
				if (why.starts_with("authority ruling image return:") || why.starts_with("authority ruling: peer")) return Finding{"return", "image_return", why};
				const char* kind = why.starts_with("histories differ") ? "histories_differ" : why.find(" failed: ") != std::string::npos ? "engine_failed" :
				                   why.starts_with("lag ended") ? "participation_ended" : why.starts_with("lag changed") ? "host_changed" : "engine_rule";
				return Finding{"history", kind, why};
			};
			if (!fixture.Start(row, error)) return failed(error);
			while (hub.now <= schedule.endMs && !fixture.roundOver)
				if (!fixture.Step(error) || !fixture.CheckBound(error) || !fixture.RecoverImages(error)) return failed(error + " at " + std::to_string(hub.now) + " ms");
			if (!fixture.FinishImageReturns(error)) return failed(error);
			if (!fixture.CompareHistories(error, true)) return failed(error + " at " + std::to_string(hub.now) + " ms");
			if (const auto unresolved = fixture.UnresolvedLateSplits(); !unresolved.empty()) return failed(unresolved);
			if (const auto apart = fixture.UnreconvergedSeparations(); !apart.empty()) return failed(apart);
			const uint64_t end = hub.now;
			for (uint8_t seat = 1; seat <= schedule.seats; ++seat) {
				if (fixture.heldSince[seat] != UINT64_MAX) fixture.heldSpans[seat].push_back({fixture.heldSince[seat], UINT64_MAX});
				if (fixture.waitAt[seat - 1] && fixture.WaitEnd(seat - 1) - fixture.waitAt[seat - 1] > static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs)))
					fixture.waits.push_back({seat, fixture.waitAt[seat - 1], fixture.WaitEnd(seat - 1), fixture.worlds[seat - 1].applied + 1, fixture.waitPlaying[seat - 1]});
			}
			if (!fixture.settledAt || fixture.startups.empty() || fixture.startups.back().first != fixture.roundStartMs)
				return Finding{"history", "never_started", "the seats never all displayed their first second of a round"};
			if (!hub.directory.refusals.empty()) return Finding{"rules", "directory_refused", std::to_string(hub.directory.refusals.size()) + " tie check-ins refused, the first " + hub.directory.refusals.front()};
			if (!hub.returnInsideInputs.empty()) return Finding{"rules", "return_inside_inputs", hub.returnInsideInputs};
			if (const auto twice = fixture.earlierRoundRule.empty() ? fixture.TwoReturnFrames() : fixture.earlierRoundRule + " in the first round"; !twice.empty())
				return Finding{"rules", "two_return_frames", twice};
			// A peer's own installed state decides whether it is a playing consumer. An opponent holding its seat never waives its bound.
			const auto linkShared = [&](uint8_t troubled, uint8_t seat) { return troubled == seat || schedule.seats == 2; };
			for (const auto& wait: fixture.waits) {
				if (fixture.InStartup(wait.startMs)) { fixture.startupWaitMs = std::max(fixture.startupWaitMs, wait.endMs - wait.startMs); continue; }
				if (wait.playing) return Finding{"bound", "survivor_waits", "authority ruling: seat " + std::to_string(wait.seat) + " waited " +
				    std::to_string(wait.endMs - wait.startMs) + "ms at frame " + std::to_string(wait.frame) + "; a directory answer never gates its local hold"};
			}
			// The round stands while a seat that could serve a return stands and no other plays to serve it - each one held, standing
			// or gone: the return cannot run ahead of its round. A donor of zero is any seat.
			const auto roundStood = [&](uint8_t seat, uint8_t donor, uint64_t after, uint64_t until) {
				uint64_t stood = 0;
				for (const auto& stand: schedule.spells) {
					if (stand.seat == seat || (donor && stand.seat != donor) || (stand.kind != Spell::Kind::Stopped && stand.kind != Spell::Kind::Hung)) continue;
					const uint64_t from = std::max(stand.atMs, after), to = std::min(stand.EndMs(), until);
					if (from >= to) continue;
					bool alone = true;
					uint32_t playing = 0;
					for (uint8_t other = 1; other <= schedule.seats; ++other) {
						if (other == seat || other == stand.seat || schedule.Left(other, from)) continue;
						bool held = false;
						for (const auto& [heldFrom, heldUntil]: fixture.heldSpans[other]) held |= heldFrom <= from && (heldUntil == UINT64_MAX || heldUntil >= to);
						const Spell* own = schedule.Active(other, from);
						const bool out = held || (own && (own->kind == Spell::Kind::Stopped || own->kind == Spell::Kind::Hung) && own->EndMs() >= to);
						alone &= out;
						if (!out) playing |= 1U << (other - 1);
					}
					// Section 11.3(b): in direct play the seats that play through their original host's hang are no half that rules while they
					// are at most half of the seats owning the round, a tie going to the host's; the fault is classified at the heal, and no
					// return enters their continuation before it.
					if (!alone && !schedule.internet && stand.seat == 1) alone = 2 * std::popcount(playing) <= std::popcount(playing | 1U);
					if (alone) stood += to - from;
				}
				return stood;
			};
			// (3) Each hold has its seat's own cause, ends once the seat is back and caught up, and the seat stays back.
			for (uint8_t seat = 1; seat <= schedule.seats; ++seat) {
				// A seat that left or was removed is kept by the AI to the end; it never returns.
				if (seat == schedule.leaver || seat == schedule.kicked) continue;
				// A round with no fault holds nobody: whatever made a seat late there is the engine's own.
				if (schedule.spells.empty() && schedule.bursts.empty() && schedule.cuts.empty() && !fixture.heldSpans[seat].empty())
					return Finding{"return", "held_in_a_fault_free_round", "seat " + std::to_string(seat) + " was held at " +
					               std::to_string(fixture.heldSpans[seat].front().first) + " ms in a round with no fault"};
				for (size_t index = 0; index < fixture.heldSpans[seat].size(); ++index) {
					const auto [since, until] = fixture.heldSpans[seat][index];
					// A view that holds a seat for less than a tick holds it for no frame.
					if (until != UINT64_MAX && until - since < static_cast<uint64_t>(std::ceil(c_TestTickMs))) continue;
					uint64_t troubleEnd = 0, backlog = 0; bool caused = false;
					const uint64_t lookFrom = since > 3000 ? since - 3000 : 0, lookTo = until == UINT64_MAX ? end : until;
					// Section 11: a seat whose own continuation is set aside, or that finds the record, returns through the image return. Its
					// play since the image is discarded, so it closes the image's lead on the round plus what its trouble costs it after the rewind.
					const Fixture::ImageStart* image = nullptr;
					for (const auto& start: fixture.imageStarts[seat]) if (start.atMs >= since && start.atMs <= lookTo) image = &start;
					const uint64_t rewoundAt = image ? image->atMs : 0;
					uint64_t backlogAfterImage = 0;
					for (const auto& spell: schedule.spells) {
						const bool linkCause = spell.kind == Spell::Kind::Late && (linkShared(spell.seat, seat) ||
						    fixture.InputSplitCausedHold(seat, since, until, spell.seat, spell.atMs, spell.EndMs()));
						if (!(spell.kind == Spell::Kind::Late ? linkCause : spell.seat == seat) || spell.atMs > lookTo) continue;
						const uint64_t catchUp = CatchUpMs(BacklogMs(spell), schedule.recoverPercent[seat]);
						const uint64_t windowEnd = spell.EndMs() + (catchUp == UINT64_MAX ? end : catchUp) + c_NetSeatCatchUpAllowanceMs;
						if (windowEnd < lookFrom) continue;
						caused |= spell.atMs <= since;
						troubleEnd = std::max(troubleEnd, spell.EndMs()); backlog += BacklogMs(spell); backlogAfterImage += BacklogMsFrom(spell, rewoundAt);
					}
					// FLAGGED fixture departure: only the proven opposing input continuation shares this link-fault cause.
					for (const auto& burst: schedule.bursts) {
						if (burst.atMs + burst.ms + 3000 < lookFrom) continue;
						if (burst.atMs <= since && (linkShared(burst.seat, seat) || fixture.InputSplitCausedHold(seat, since, until, burst.seat, burst.atMs, burst.atMs + burst.ms))) {
							caused = true; troubleEnd = std::max(troubleEnd, burst.atMs + burst.ms);
						}
						// Loss on its own link while it is held slows its return as any trouble of its own does.
						else if (burst.atMs > since && burst.atMs <= lookTo && linkShared(burst.seat, seat)) troubleEnd = std::max(troubleEnd, burst.atMs + burst.ms);
						// Its catch-up's tail crosses its own link: loss there from its first hold in reach, through a return it trailed and this
						// hold, takes that share of what it reads.
						if (burst.seat == seat) {
							uint64_t catchingFrom = since;
							for (size_t span = 0; span < index; ++span) if (fixture.heldSpans[seat][span].second >= lookFrom) catchingFrom = std::min(catchingFrom, fixture.heldSpans[seat][span].first);
							const uint64_t from = std::max({burst.atMs, catchingFrom, lookFrom}), to = std::min(burst.atMs + burst.ms, lookTo);
							if (from < to) backlog += (to - from) * burst.percent / 100;
						}
						// Its image's tail crosses that link, and its donor's: the loss after the rewind starves the replay it closes on.
						if (image && (linkShared(burst.seat, seat) || burst.seat == image->donor) && burst.atMs + burst.ms > rewoundAt)
							backlogAfterImage += burst.atMs + burst.ms - std::max(burst.atMs, rewoundAt);
					}
					// A live link split makes each half hold the other. A hung process is not a split and cannot justify holding a survivor.
					for (const auto& spell: schedule.spells) {
						if (spell.seat == seat || spell.kind != Spell::Kind::Silent || spell.atMs > lookTo || spell.EndMs() + 3000 < lookFrom) continue;
						caused |= spell.atMs <= since; troubleEnd = std::max(troubleEnd, spell.EndMs()); backlog += spell.ms;
						backlogAfterImage += spell.EndMs() > rewoundAt ? spell.EndMs() - std::max(spell.atMs, rewoundAt) : 0;
					}
					for (const auto& cut: schedule.cuts) {
						if (cut.atMs > lookTo || cut.atMs + cut.ms + 3000 < lookFrom) continue;
						caused |= cut.atMs <= since; troubleEnd = std::max(troubleEnd, cut.atMs + cut.ms); backlog += cut.ms;
						backlogAfterImage += cut.atMs + cut.ms > rewoundAt ? cut.atMs + cut.ms - std::max(cut.atMs, rewoundAt) : 0;
					}
					// Section 11.1: the seat whose input arrived late past one side's bound is held there, and the side the round's arbitration
					// sets aside is held until its image return; both holds are that late input's, from it to the reconverging return.
					auto lateSplits = fixture.earlierLateSplits; lateSplits.insert(lateSplits.end(), fixture.lateSplits.begin(), fixture.lateSplits.end());
					for (const auto& late: lateSplits) {
						const uint64_t settled = late.resolvedAt ? late.resolvedAt : end;
						if ((late.a != seat && late.b != seat && late.owner != seat) || since + 3000 < late.observedAt || since > settled) continue;
						caused = true; troubleEnd = std::max(troubleEnd, settled);
					}
					if (!caused) {
						const bool again = index > 0;
						return Finding{"return", fixture.InStartup(since) ? "held_at_the_round_start" : again ? "returned_seat_held_again" : "held_without_its_own_trouble", "seat " + std::to_string(seat) + " was held at " + std::to_string(since) +
						               " ms with no trouble of its own since " + std::to_string(lookFrom) + " ms" + (again ? " after it had returned" : "")};
					}
					// Sections 11.2 and 11.3(b): the image return starts at the split's ruling, which comes at the heal or from the directory within
					// its tie deadline; from there the image's trip and its replay. A machine with no spare work returns trailing at the round's pace.
					// The image never shortens the return its trouble alone allows.
					const uint64_t troubleCatchUp = CatchUpMs(backlog, schedule.recoverPercent[seat]);
					if (troubleCatchUp == UINT64_MAX) continue;
					uint64_t deadline = troubleEnd + troubleCatchUp + roundStood(seat, 0, troubleEnd, lookTo) + c_NetSeatCatchUpAllowanceMs;
					if (image) {
						const uint32_t recover = schedule.recoverPercent[seat];
						const uint64_t imageBacklog = backlogAfterImage + (recover > 100 ? image->gapMs : 0);
						const uint64_t imageCatchUp = CatchUpMs(imageBacklog, recover);
						// A seat whose own directory link was dark learns the ruling once the question it sent into that outage times out (the
						// client's timeout cap, as the directory client sizes it) and is asked again.
						bool directoryDark = false;
						for (const auto& spell: schedule.spells) directoryDark |= spell.seat == seat && spell.directoryDark && spell.atMs <= troubleEnd && spell.EndMs() + 3000 >= since;
						const uint64_t ruledAt = std::max(troubleEnd, std::min(image->atMs, troubleEnd + c_NetFrameTieDeadlineMs + (directoryDark ? c_TestTieTimeoutCapMs : 0)));
						const uint64_t standingMs = roundStood(seat, image->donor, image->atMs, lookTo);
						// The donor's service activates the return on the returner's progress reports: the image's trip there and the reports' back.
						if (imageCatchUp != UINT64_MAX && ruledAt + 2 * image->transitMs + imageCatchUp + standingMs + image->spacingMs + c_NetSeatCatchUpAllowanceMs > deadline) {
							deadline = ruledAt + 2 * image->transitMs + imageCatchUp + standingMs + image->spacingMs + c_NetSeatCatchUpAllowanceMs;
							backlog = imageBacklog;
						} else image = nullptr;
					}
					if (deadline >= end || (until != UINT64_MAX && until <= deadline)) continue;
					return Finding{"return", until == UINT64_MAX ? "held_seat_never_returns" : "held_seat_returns_late", "seat " + std::to_string(seat) + " held at " + std::to_string(since) +
					               " ms, its trouble over at " + std::to_string(troubleEnd) + " ms and its " + std::to_string(backlog) + " ms backlog" +
					               (image ? " (an image return from " + std::to_string(image->atMs) + " ms)" : "") + " closed by " +
					               std::to_string(deadline - c_NetSeatCatchUpAllowanceMs) + " ms, " + (until == UINT64_MAX ? "still held at " + std::to_string(end) : "returned at " + std::to_string(until)) +
					               " ms against " + std::to_string(deadline) + " ms"};
				}
			}
			return std::nullopt;
		}

		// Every row runs on a worker thread. A named row's engine lines are kept for it and printed in its turn; a random round's are
		// dropped - its engines print every hold and wait.
		thread_local bool t_QuietLines = false;
		thread_local std::string* t_RowLines = nullptr;
		class QuietLines final : public std::streambuf {
		public:
			explicit QuietLines(std::ostream& stream): m_Stream(stream), m_Target(stream.rdbuf(this)) {}
			~QuietLines() override { m_Stream.rdbuf(m_Target); }
			QuietLines(const QuietLines&) = delete;
			QuietLines& operator=(const QuietLines&) = delete;
		protected:
			int overflow(int c) override {
				if (c == traits_type::eof()) return traits_type::not_eof(c);
				if (t_RowLines) { t_RowLines->push_back(static_cast<char>(c)); return c; }
				return t_QuietLines ? c : m_Target->sputc(static_cast<char>(c));
			}
			std::streamsize xsputn(const char* data, std::streamsize count) override {
				if (t_RowLines) { t_RowLines->append(data, static_cast<size_t>(count)); return count; }
				return t_QuietLines ? count : m_Target->sputn(data, count);
			}
			int sync() override { return t_QuietLines || t_RowLines ? 0 : m_Target->pubsync(); }
		private:
			std::ostream& m_Stream;
			std::streambuf* m_Target;
		};
		template<class Work> void RunParallel(size_t count, unsigned threads, Work&& work) {
			std::atomic<size_t> next{0};
			std::vector<std::thread> pool;
			for (unsigned index = 0; index < std::max(1U, threads); ++index)
				pool.emplace_back([&] { t_PoolWorker = t_QuietLines = true; for (size_t item; (item = next++) < count;) work(item); });
			for (auto& thread: pool) thread.join();
		}
		std::optional<Finding> RunScheduleQuietly(const Schedule& schedule, uint64_t* startupWaitMs = nullptr) {
			Fixture fixture; fixture.parallel = true;
			std::optional<Finding> finding;
			try { finding = RunSchedule(schedule, fixture); }
			catch (const std::exception& exception) {
				finding = Finding{"exception", "schedule_exception", "schedule raised an exception at " + std::to_string(fixture.hub.now) + "ms: " + exception.what()};
			}
			if (startupWaitMs) *startupWaitMs = fixture.startupWaitMs;
			return finding;
		}
		// Smaller schedules that may still fail the same way: a seat fewer, an event fewer, shorter spells, plainer links and machines.
		std::vector<Schedule> Shrinks(const Schedule& schedule) {
			std::vector<Schedule> candidates;
			const auto add = [&](Schedule smaller) { smaller.endMs = ScheduleEndMs(smaller); candidates.push_back(std::move(smaller)); };
			if (schedule.seats > 2) for (uint8_t gone = schedule.seats; gone >= 2; --gone) {
				Schedule smaller = schedule; smaller.seats = static_cast<uint8_t>(schedule.seats - 1); smaller.spells.clear(); smaller.bursts.clear();
				smaller.rttMs = {}; smaller.jitterMs = {};
				const auto renumber = [gone](uint8_t seat) { return static_cast<uint8_t>(seat > gone ? seat - 1 : seat); };
				for (auto spell: schedule.spells) if (spell.seat != gone) { spell.seat = renumber(spell.seat); smaller.spells.push_back(spell); }
				for (auto burst: schedule.bursts) if (burst.seat != gone) { burst.seat = renumber(burst.seat); smaller.bursts.push_back(burst); }
				smaller.leaver = schedule.leaver == gone ? 0 : renumber(schedule.leaver); if (!smaller.leaver) smaller.leaveAtMs = 0;
				smaller.kicked = schedule.kicked == gone ? 0 : renumber(schedule.kicked); if (!smaller.kicked) smaller.kickAtMs = 0;
				for (auto& cut: smaller.cuts) for (uint32_t* mask: {&cut.side, &cut.directoryDark}) {
					uint32_t kept = 0;
					for (uint8_t seat = 1; seat <= schedule.seats; ++seat) if (seat != gone && ((*mask >> (seat - 1)) & 1U)) kept |= 1U << (renumber(seat) - 1);
					*mask = kept;
				}
				for (uint8_t a = 1; a <= schedule.seats; ++a) for (uint8_t b = 1; b <= schedule.seats; ++b) if (a != gone && b != gone) {
					smaller.rttMs[renumber(a)][renumber(b)] = schedule.rttMs[a][b]; smaller.jitterMs[renumber(a)][renumber(b)] = schedule.jitterMs[a][b];
				}
				for (uint8_t seat = 1; seat <= schedule.seats; ++seat) if (seat != gone) {
					smaller.recoverPercent[renumber(seat)] = schedule.recoverPercent[seat]; smaller.allowancePhaseMs[renumber(seat)] = schedule.allowancePhaseMs[seat];
				}
				add(smaller);
			}
			for (size_t index = 0; index < schedule.spells.size(); ++index) { auto smaller = schedule; smaller.spells.erase(smaller.spells.begin() + index); add(smaller); }
			for (size_t index = 0; index < schedule.bursts.size(); ++index) { auto smaller = schedule; smaller.bursts.erase(smaller.bursts.begin() + index); add(smaller); }
			if (schedule.leaver) { auto stays = schedule; stays.leaver = 0; stays.leaveAtMs = 0; add(stays); }
			if (schedule.kicked) { auto kept = schedule; kept.kicked = 0; kept.kickAtMs = 0; add(kept); }
			if (schedule.rematchAtMs) { auto one = schedule; one.rematchAtMs = 0; add(one); }
			for (size_t index = 0; index < schedule.cuts.size(); ++index) {
				auto whole = schedule; whole.cuts.erase(whole.cuts.begin() + index); add(whole);
				if (schedule.cuts[index].ms > 200) { auto shorter = schedule; shorter.cuts[index].ms /= 2; add(shorter); }
			}
			for (size_t index = 0; index < schedule.spells.size(); ++index) {
				const auto& spell = schedule.spells[index];
				if (spell.ms > 200) { auto smaller = schedule; smaller.spells[index].ms = std::max<uint64_t>(200, spell.ms / 2); add(smaller); }
				if (spell.atMs > 6000) { auto earlier = schedule; earlier.spells[index].atMs = std::max<uint64_t>(6000, spell.atMs - 2000); add(earlier); }
				if (spell.kind == Spell::Kind::Silent && spell.directoryDark) { auto lit = schedule; lit.spells[index].directoryDark = false; add(lit); }
				if (spell.kind == Spell::Kind::Slow && spell.percent < 90) { auto milder = schedule; milder.spells[index].percent = std::min<uint32_t>(95, spell.percent + 10); add(milder); }
			}
			for (size_t index = 0; index < schedule.bursts.size(); ++index) if (schedule.bursts[index].ms > 200) { auto smaller = schedule; smaller.bursts[index].ms /= 2; add(smaller); }
			uint32_t low = UINT32_MAX, high = 0; bool jitter = false;
			for (uint8_t a = 1; a <= schedule.seats; ++a) for (uint8_t b = a + 1; b <= schedule.seats; ++b) {
				low = std::min(low, schedule.rttMs[a][b]); high = std::max(high, schedule.rttMs[a][b]); jitter |= schedule.jitterMs[a][b] != 0;
			}
			if (jitter) { auto plain = schedule; plain.jitterMs = {}; add(plain); }
			for (uint32_t rtt: {high, low}) if (low != high) {
				auto even = schedule;
				for (uint8_t a = 1; a <= schedule.seats; ++a) for (uint8_t b = 1; b <= schedule.seats; ++b) if (a != b) even.rttMs[a][b] = rtt;
				add(even);
			}
			if (schedule.directoryExchangeMs != 100) { auto plain = schedule; plain.directoryExchangeMs = 100; add(plain); }
			for (uint8_t seat = 1; seat <= schedule.seats; ++seat) if (schedule.recoverPercent[seat] != 300) { auto plain = schedule; plain.recoverPercent[seat] = 300; add(plain); }
			if (std::any_of(schedule.allowancePhaseMs.begin(), schedule.allowancePhaseMs.end(), [](uint32_t phase) { return phase != 0; })) { auto plain = schedule; plain.allowancePhaseMs = {}; add(plain); }
			if (schedule.internet) { auto direct = schedule; direct.internet = false; add(direct); }
			return candidates;
		}
		// Shrinks a failing schedule while it still fails the same way: the shrinks are tried in order, a worker's worth at once, and the
		// first in order that still fails is kept and shrunk again, until none does or the run budget is spent.
		Schedule Minimize(Schedule schedule, const Finding& finding, unsigned threads, size_t budget, size_t& runs, Finding& last, SteadyClock::time_point deadline) {
			last = finding;
			for (bool shrunk = true; shrunk && runs < budget && SteadyClock::now() < deadline;) {
				shrunk = false;
				const auto candidates = Shrinks(schedule);
				for (size_t first = 0; first < candidates.size() && !shrunk && runs < budget && SteadyClock::now() < deadline; first += threads) {
					const size_t count = std::min({static_cast<size_t>(threads), candidates.size() - first, budget - runs});
					std::vector<std::optional<Finding>> results(count);
					RunParallel(count, threads, [&](size_t index) { results[index] = RunScheduleQuietly(candidates[first + index]); });
					runs += count;
					for (size_t index = 0; index < count && !shrunk; ++index)
						if (results[index] && results[index]->property == finding.property && results[index]->kind == finding.kind) { schedule = candidates[first + index]; last = *results[index]; shrunk = true; }
				}
			}
			return schedule;
		}
		uint64_t EnvironmentNumber(const char* name, uint64_t fallback) {
			const char* text = std::getenv(name);
			if (!text || !*text) return fallback;
			char* end = nullptr; const uint64_t value = std::strtoull(text, &end, 10);
			return end && *end == '\0' ? value : fallback;
		}
		// Rows found by the random driver and kept: each a minimized schedule named after its shape, with its seed in its text.
		struct ScheduleRow { const char* name; const char* schedule; };
		const std::vector<ScheduleRow>& FixedScheduleRows() {
			// Each seat's routine check-ins went out 1.7 s apart before the round, as the fixed rows above have them.
			static const std::vector<ScheduleRow> rows = {
				// Four internet seats at 220-290 ms and no fault: the host's clock must not start a link trip ahead of its seats'.
				{"four_internet_seats_220_to_290ms_fault_free_round_holds_nobody", "seed=0 seats=4 internet=1 exchange=100 end=8000 rtt=247,217,237,292,270,285 jitter=20,20,10,10,20,10 recover=300,300,300,300 phase=4468,3027,2118,4525"},
				// Both seats of a two-player round held at once: the slow host volunteers its own hold while the silent joiner holds it.
				{"two_direct_seats_host_yields_while_the_silent_joiner_holds_it", "seed=0 seats=2 internet=0 exchange=100 rtt=200 jitter=10 recover=300,300 phase=1700,3400 spell=1:slow:6000:3000:40 spell=2:silent:6200:1500"},
				{"two_internet_seats_host_yields_while_the_silent_joiner_holds_it", "seed=0 seats=2 internet=1 exchange=100 rtt=200 jitter=10 recover=300,300 phase=1700,3400 spell=1:slow:6000:3000:40 spell=2:silent:6200:1500"},
				// Current-client groups: the minimized counterexample, plus its original seat/referee shape when shrinking changes it.
				{"four_direct_seats_120ms_jitter_40ms_host_late_100ms_more_928ms_seat3_silent_200ms_back_at_105pc_loss_10pc_host_engine_failed_seed_42", "seed=42 seats=4 internet=0 exchange=100 end=16840 rtt=90,87,106,107,97,120 jitter=5,0,20,0,10,40 recover=300,300,105,120 phase=3851,2957,1974,3572 spell=1:late:5377:928:100 spell=3:silent:4640:200 burst=1:7122:1100:10"},
				{"three_internet_seats_56ms_jitter_20ms_host_silent_200ms_back_at_100pc_host_hung_900ms_back_at_100pc_loss_90pc_seat3_loss_90pc_host_histories_differ_seed_127", "seed=127 seats=3 internet=1 exchange=100 end=17412 rtt=44,56,46 jitter=5,0,20 recover=100,300,100 phase=1745,1966,3496 spell=1:silent:5978:200 spell=1:hung:8062:900 burst=3:4947:1923:90 burst=1:7800:323:90"},
				{"two_direct_seats_42ms_host_stopped_200ms_back_at_100pc_seat2_slow_74pc_737ms_held_seat_never_returns_seed_163", "seed=163 seats=2 internet=0 exchange=100 end=16149 rtt=42 jitter=0 recover=100,300 phase=0,0 spell=1:stopped:6000:200 spell=2:slow:7317:737:74"},
				{"two_internet_seats_42ms_jitter_10ms_directory_40ms_host_stopped_464ms_back_at_100pc_seat2_slow_64pc_5902ms_host_silent_936ms_back_at_100pc_held_seat_never_returns_seed_163", "seed=163 seats=2 internet=1 exchange=40 end=22281 rtt=42 jitter=10 recover=100,300 phase=4776,3197 spell=1:stopped:6905:464 spell=2:slow:7317:5902:64 spell=1:silent:4302:936"},
				{"two_direct_seats_170ms_seat2_slow_88pc_200ms_back_at_120pc_loss_60pc_seat2_held_seat_never_returns_seed_79", "seed=79 seats=2 internet=0 exchange=100 end=18279 rtt=170 jitter=0 recover=300,120 phase=0,0 spell=2:slow:4622:200:88 burst=2:9373:2906:60"},
				{"four_internet_seats_226ms_jitter_5ms_directory_160ms_seat4_slow_68pc_4576ms_back_at_120pc_loss_60pc_seat4_held_seat_never_returns_seed_79", "seed=79 seats=4 internet=1 exchange=160 end=24518 rtt=184,226,170,164,192,202 jitter=0,0,0,5,0,0 recover=105,300,300,120 phase=1676,4320,3581,1557 spell=4:slow:4622:4576:68 burst=4:9373:2906:60"},
				{"three_internet_seats_56ms_host_slow_90pc_895ms_back_at_100pc_seat2_silent_186ms_held_seat_returns_late_seed_116", "seed=116 seats=3 internet=1 exchange=100 end=14279 rtt=56,44,54 jitter=0,0,0 recover=100,300,300 phase=0,0,0 spell=1:slow:4109:895:90 spell=2:silent:6000:186"},
				{"three_direct_seats_120ms_host_hung_200ms_back_at_105pc_held_seat_returns_late_seed_100", "seed=100 seats=3 internet=0 exchange=100 end=18200 rtt=120,120,120 jitter=0,0,0 recover=105,300,300 phase=0,0,0 spell=1:hung:6000:200"},
				{"four_direct_seats_120ms_jitter_40ms_host_hung_750ms_back_at_105pc_held_seat_returns_late_seed_100", "seed=100 seats=4 internet=0 exchange=300 end=30636 rtt=90,86,113,120,107,119 jitter=20,0,0,0,0,40 recover=105,120,105,120 phase=2832,2565,4315,4286 spell=1:hung:6886:750"},
				{"three_internet_seats_57ms_host_silent_198ms_seat3_silent_3000ms_held_seat_returns_late_seed_109", "seed=109 seats=3 internet=1 exchange=100 end=19842 rtt=57,57,57 jitter=0,0,0 recover=300,300,300 phase=0,0,0 spell=1:silent:6000:198 spell=3:silent:7342:3000"},
				{"four_internet_seats_60ms_jitter_25ms_directory_40ms_host_silent_dark_198ms_seat4_silent_dark_3000ms_back_at_120pc_held_seat_returns_late_seed_109", "seed=109 seats=4 internet=1 exchange=40 end=33342 rtt=40,52,41,50,57,60 jitter=0,0,10,25,20,0 recover=300,300,300,120 phase=1062,764,2450,1484 spell=1:silent:6871:198:dark spell=4:silent:7342:3000:dark"},
				{"three_direct_seats_200ms_jitter_40ms_held_without_its_own_trouble_seed_187", "seed=187 seats=3 internet=0 exchange=100 end=12000 rtt=200,200,200 jitter=40,10,0 recover=300,300,300 phase=0,0,0"},
				{"four_internet_seats_270ms_jitter_40ms_directory_160ms_seat3_late_400ms_more_204ms_back_at_100pc_host_stopped_824ms_loss_90pc_seat4_loss_10pc_seat4_held_without_its_own_trouble_seed_187", "seed=187 seats=4 internet=1 exchange=160 end=14878 rtt=270,200,250,255,260,215 jitter=40,0,10,0,0,0 recover=300,100,100,100 phase=1441,2938,4715,2462 spell=3:late:5233:204:400 spell=1:stopped:5500:824 burst=4:4900:717:90 burst=4:8247:631:10"},
				{"two_internet_seats_54ms_seat2_late_50ms_more_1785ms_returned_seat_held_again_seed_174", "seed=174 seats=2 internet=1 exchange=100 end=15810 rtt=54 jitter=0 recover=300,300 phase=0,0 spell=2:late:6000:1785:50"},
				{"four_internet_seats_300ms_jitter_40ms_two_return_frames_seed_131", "seed=131 seats=4 internet=1 exchange=100 end=12000 rtt=220,260,282,300,245,290 jitter=40,20,0,40,0,0 recover=300,300,300,300 phase=0,0,0,0"},
				// The complete Linux 5,000-schedule run, then the current-history failure and its directory twin.
				{"two_direct_seats_59ms_host_stopped_615ms_survivor_waits_seed_1811", "seed=1811 seats=2 internet=0 exchange=100 end=13169 rtt=59 jitter=0 recover=300,300 phase=0,0 spell=1:stopped:4247:615"},
				{"two_internet_seats_180ms_directory_240ms_seat2_silent_174ms_survivor_waits_seed_518", "seed=518 seats=2 internet=1 exchange=240 end=13403 rtt=180 jitter=0 recover=300,300 phase=0,0 spell=2:silent:5142:174"},
				{"two_direct_seats_238ms_host_silent_200ms_survivor_waits_seed_2453", "seed=2453 seats=2 internet=0 exchange=100 end=14109 rtt=238 jitter=0 recover=300,300 phase=0,0 spell=1:silent:5809:200"},
				{"three_direct_seats_270ms_jitter_20ms_seat2_stopped_200ms_host_silent_360ms_survivor_waits_seed_2469", "seed=2469 seats=3 internet=0 exchange=100 end=13324 rtt=247,270,210 jitter=0,0,20 recover=300,300,300 phase=0,0,0 spell=2:stopped:4691:200 spell=1:silent:4784:360"},
				{"four_direct_seats_115ms_jitter_40ms_host_silent_476ms_survivor_waits_seed_478", "seed=478 seats=4 internet=0 exchange=100 end=13210 rtt=94,109,97,84,115,87 jitter=20,40,10,0,40,40 recover=300,300,300,300 phase=0,0,0,0 spell=1:silent:4496:476"},
				{"three_direct_seats_160ms_seat3_slow_61pc_1061ms_back_at_100pc_survivor_waits_seed_4758", "seed=4758 seats=3 internet=0 exchange=100 end=13554 rtt=160,160,160 jitter=0,0,0 recover=300,300,100 phase=0,0,0 spell=3:slow:4493:1061:61"},
				{"two_internet_seats_212ms_directory_300ms_host_hung_408ms_tie_survivor_waits_seed_3719", "seed=3719 seats=2 internet=1 exchange=300 end=13953 rtt=212 jitter=0 recover=300,300 phase=0,0 spell=1:hung:5341:408"},
				{"three_direct_seats_114ms_loss_60pc_seat3_seat2_leaves_tie_survivor_waits_seed_4908", "seed=4908 seats=3 internet=0 exchange=100 end=17211 rtt=114,114,114 jitter=0,0,0 recover=300,300,300 phase=0,0,0 burst=3:10379:832:60 leave=2:10500"},
				{"three_direct_seats_275ms_seat3_silent_200ms_host_slow_57pc_1744ms_seat2_leaves_tie_survivor_waits_seed_3141", "seed=3141 seats=3 internet=0 exchange=100 end=16124 rtt=275,275,275 jitter=0,0,0 recover=300,300,300 phase=0,0,0 spell=3:silent:7824:200 spell=1:slow:5749:1744:57 leave=2:7063"},
				{"four_direct_seats_59ms_seat4_silent_408ms_seat3_silent_200ms_tie_survivor_waits_seed_359", "seed=359 seats=4 internet=0 exchange=100 end=12837 rtt=59,59,59,59,59,59 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=0,0,0,0 spell=4:silent:4225:408 spell=3:silent:4467:200"},
				{"four_internet_seats_218ms_host_hung_800ms_loss_60pc_seat2_tie_survivor_waits_seed_4282", "seed=4282 seats=4 internet=1 exchange=100 end=13553 rtt=218,218,218,218,218,218 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=0,0,0,0 spell=1:hung:4353:800 burst=2:4268:732:60"},
				{"three_direct_seats_240ms_jitter_40ms_seat2_slow_53pc_1575ms_host_hung_1410ms_loss_60pc_host_engine_failed_seed_4779", "seed=4779 seats=3 internet=0 exchange=100 end=19504 rtt=227,217,240 jitter=20,0,40 recover=300,300,300 phase=0,0,0 spell=2:slow:7597:1575:53 spell=1:hung:9389:1410 burst=1:11657:1632:60"},
				{"three_direct_seats_228ms_jitter_40ms_seat2_stopped_186ms_back_at_105pc_loss_90pc_seat2_engine_failed_seed_2540", "seed=2540 seats=3 internet=0 exchange=100 end=18627 rtt=228,184,204 jitter=20,10,40 recover=300,105,300 phase=0,0,0 spell=2:stopped:6721:186 burst=2:5583:2765:90"},
				{"four_direct_seats_297ms_jitter_10ms_loss_60pc_seat4_engine_failed_seed_311", "seed=311 seats=4 internet=0 exchange=100 end=13875 rtt=220,217,297,262,210,237 jitter=10,5,0,0,10,0 recover=300,300,300,300 phase=0,0,0,0 burst=4:5011:2864:60"},
				{"four_internet_seats_238ms_jitter_10ms_loss_60pc_seat2_engine_failed_seed_2125", "seed=2125 seats=4 internet=1 exchange=100 end=12765 rtt=196,216,238,182,186,162 jitter=5,0,0,0,10,10 recover=300,300,300,300 phase=0,0,0,0 burst=2:4225:2540:60"},
				{"two_internet_seats_50ms_host_silent_dark_388ms_seat2_leaves_histories_differ_seed_360", "seed=360 seats=2 internet=1 exchange=100 end=14220 rtt=50 jitter=0 recover=300,300 phase=0,0 spell=1:silent:5638:388:dark leave=2:6475"},
				{"three_direct_seats_52ms_jitter_26ms_host_silent_832ms_loss_10pc_host_histories_differ_seed_4847", "seed=4847 seats=3 internet=0 exchange=100 end=17372 rtt=52,40,41 jitter=26,5,0 recover=300,300,300 phase=0,0,0 spell=1:silent:4083:832 burst=1:9040:2332:10"},
				{"three_direct_seats_224ms_jitter_10ms_seat3_stopped_1455ms_back_at_120pc_loss_90pc_seat2_histories_differ_seed_2208", "seed=2208 seats=3 internet=0 exchange=100 end=21605 rtt=224,224,168 jitter=0,0,10 recover=300,300,120 phase=0,0,0 spell=3:stopped:4875:1455 burst=2:10808:1991:90"},
				{"four_direct_seats_59ms_jitter_20ms_host_hung_372ms_histories_differ_seed_4469", "seed=4469 seats=4 internet=0 exchange=100 end=17725 rtt=59,57,47,45,56,47 jitter=0,5,0,0,0,20 recover=300,300,300,300 phase=0,0,0,0 spell=1:hung:9167:372"},
				{"three_direct_seats_204ms_jitter_10ms_loss_60pc_seat2_loss_90pc_seat3_histories_differ_seed_2810", "seed=2810 seats=3 internet=0 exchange=100 end=15069 rtt=204,194,204 jitter=0,10,5 recover=300,300,300 phase=0,0,0 burst=2:7318:1751:60 burst=3:5450:1017:90"},
				{"four_internet_seats_252ms_jitter_40ms_held_at_the_round_start_seed_3034", "seed=3034 seats=4 internet=1 exchange=100 end=12000 rtt=205,227,215,242,252,250 jitter=40,0,10,20,5,0 recover=300,300,300,300 phase=0,0,0,0"},
				{"two_direct_seats_280ms_seat2_late_100ms_more_200ms_back_at_120pc_loss_10pc_seat2_loss_90pc_seat2_held_seat_never_returns_seed_4266", "seed=4266 seats=2 internet=0 exchange=100 end=19463 rtt=280 jitter=0 recover=300,120 phase=0,0 spell=2:late:6000:200:100 burst=2:11567:1896:10 burst=2:10474:1070:90"},
				{"two_internet_seats_55ms_host_silent_dark_456ms_seat2_leaves_held_seat_never_returns_seed_4838", "seed=4838 seats=2 internet=1 exchange=100 end=14633 rtt=55 jitter=0 recover=300,300 phase=0,0 spell=1:silent:5949:456:dark leave=2:8087"},
				{"three_direct_seats_52ms_host_stopped_188ms_back_at_120pc_loss_90pc_host_held_seat_never_returns_seed_3652", "seed=3652 seats=3 internet=0 exchange=100 end=14663 rtt=52,52,52 jitter=0,0,0 recover=120,300,300 phase=0,0,0 spell=1:stopped:5535:188 burst=1:4078:2838:90"},
				{"two_direct_seats_57ms_seat2_late_50ms_more_200ms_back_at_105pc_loss_90pc_host_held_seat_never_returns_seed_568", "seed=568 seats=2 internet=0 exchange=100 end=15479 rtt=57 jitter=0 recover=300,105 phase=0,0 spell=2:late:6000:200:50 burst=1:8339:1140:90"},
				{"two_direct_seats_91ms_seat2_late_200ms_more_200ms_back_at_105pc_loss_90pc_seat2_held_seat_never_returns_seed_4759", "seed=4759 seats=2 internet=0 exchange=100 end=17946 rtt=91 jitter=0 recover=300,105 phase=0,0 spell=2:late:5598:200:200 burst=2:10783:1163:90"},
				{"three_direct_seats_265ms_host_late_200ms_more_1335ms_back_at_105pc_held_seat_never_returns_seed_714", "seed=714 seats=3 internet=0 exchange=100 end=17838 rtt=265,265,265 jitter=0,0,0 recover=105,300,300 phase=0,0,0 spell=1:late:4503:1335:200"},
				{"two_direct_seats_198ms_host_stopped_230ms_loss_60pc_host_loss_90pc_seat2_held_seat_returns_late_seed_3209", "seed=3209 seats=2 internet=0 exchange=100 end=19465 rtt=198 jitter=0 recover=300,300 phase=0,0 spell=1:stopped:9554:230 burst=1:11504:1961:60 burst=2:7033:1433:90"},
				{"two_internet_seats_50ms_jitter_20ms_host_stopped_384ms_loss_30pc_host_held_seat_returns_late_seed_3775", "seed=3775 seats=2 internet=1 exchange=100 end=14868 rtt=50 jitter=20 recover=300,300 phase=0,0 spell=1:stopped:6292:384 burst=1:4489:2741:30"},
				{"three_direct_seats_21ms_host_hung_472ms_back_at_120pc_loss_30pc_seat2_held_seat_returns_late_seed_2325", "seed=2325 seats=3 internet=0 exchange=100 end=15806 rtt=21,21,21 jitter=0,0,0 recover=120,300,300 phase=0,0,0 spell=1:hung:4974:472 burst=2:5914:670:30"},
				{"three_direct_seats_172ms_seat2_silent_200ms_held_seat_returns_late_seed_3309", "seed=3309 seats=3 internet=0 exchange=100 end=13981 rtt=172,172,172 jitter=0,0,0 recover=300,300,300 phase=0,0,0 spell=2:silent:5681:200"},
				{"two_direct_seats_282ms_loss_90pc_host_held_seat_returns_late_seed_3760", "seed=3760 seats=2 internet=0 exchange=100 end=15195 rtt=282 jitter=0 recover=300,300 phase=0,0 burst=1:6293:2902:90"},
				{"three_direct_seats_240ms_seat3_silent_200ms_held_seat_returns_late_seed_1020", "seed=1020 seats=3 internet=0 exchange=100 end=14082 rtt=240,240,240 jitter=0,0,0 recover=300,300,300 phase=0,0,0 spell=3:silent:5782:200"},
				{"three_direct_seats_57ms_jitter_10ms_host_hung_456ms_held_without_its_own_trouble_seed_4505", "seed=4505 seats=3 internet=0 exchange=100 end=12914 rtt=51,57,54 jitter=0,10,0 recover=300,300,300 phase=0,0,0 spell=1:hung:4230:456"},
				{"three_direct_seats_58ms_host_silent_920ms_held_without_its_own_trouble_seed_1964", "seed=1964 seats=3 internet=0 exchange=100 end=16792 rtt=58,58,58 jitter=0,0,0 recover=300,300,300 phase=0,0,0 spell=1:silent:7412:920"},
				{"three_direct_seats_47ms_jitter_23ms_host_silent_376ms_held_without_its_own_trouble_seed_1176", "seed=1176 seats=3 internet=0 exchange=100 end=13136 rtt=47,47,47 jitter=0,23,0 recover=300,300,300 phase=0,0,0 spell=1:silent:4572:376"},
				{"four_internet_seats_297ms_jitter_40ms_held_without_its_own_trouble_seed_3563", "seed=3563 seats=4 internet=1 exchange=100 end=12000 rtt=255,277,295,290,297,270 jitter=40,20,10,5,20,10 recover=300,300,300,300 phase=0,0,0,0"},
				{"three_direct_seats_50ms_jitter_25ms_host_silent_448ms_returned_seat_held_again_seed_1790", "seed=1790 seats=3 internet=0 exchange=100 end=13055 rtt=50,41,45 jitter=25,5,22 recover=300,300,300 phase=0,0,0 spell=1:silent:4383:448"},
				{"three_direct_seats_60ms_jitter_30ms_host_late_400ms_more_344ms_loss_30pc_host_loss_60pc_seat2_returned_seat_held_again_seed_3372", "seed=3372 seats=3 internet=0 exchange=100 end=17535 rtt=58,60,43 jitter=5,30,5 recover=300,300,300 phase=0,0,0 spell=1:late:4589:344:400 burst=1:8811:2724:30 burst=2:6860:456:60"},
				{"three_direct_seats_300ms_host_hung_200ms_seat3_silent_7200ms_returned_seat_held_again_seed_4371", "seed=4371 seats=3 internet=0 exchange=100 end=24800 rtt=300,300,300 jitter=0,0,0 recover=300,300,300 phase=0,0,0 spell=1:hung:5083:200 spell=3:silent:6000:7200"},
				{"four_internet_seats_53ms_jitter_26ms_seat4_hung_186ms_back_at_100pc_host_late_400ms_more_436ms_returned_seat_held_again_seed_4066", "seed=4066 seats=4 internet=1 exchange=100 end=16664 rtt=52,45,42,53,41,40 jitter=20,0,10,26,10,0 recover=300,300,300,100 phase=0,0,0,0 spell=4:hung:6000:186 spell=1:late:8028:436:400"},
				{"four_internet_seats_272ms_host_stopped_752ms_back_at_100pc_seat2_slow_86pc_5574ms_back_at_100pc_two_return_frames_seed_404", "seed=404 seats=4 internet=1 exchange=100 end=20304 rtt=250,272,245,230,222,265 jitter=0,0,0,0,0,0 recover=100,100,300,300 phase=0,0,0,0 spell=1:stopped:6000:752 spell=2:slow:6730:5574:86"},
				{"three_direct_seats_290ms_jitter_10ms_seat3_slow_56pc_1296ms_back_at_105pc_host_silent_1335ms_histories_differ_seed_169", "seed=169 seats=3 internet=0 exchange=100 end=25340 rtt=270,215,290 jitter=0,0,10 recover=300,300,105 phase=0,0,0 spell=3:slow:4644:1296:56 spell=1:silent:6158:1335"},
				{"three_internet_seats_290ms_jitter_10ms_seat3_slow_56pc_1296ms_back_at_105pc_host_silent_1335ms_histories_differ_seed_169", "seed=169 seats=3 internet=1 exchange=100 end=25340 rtt=270,215,290 jitter=0,0,10 recover=300,300,105 phase=0,0,0 spell=3:slow:4644:1296:56 spell=1:silent:6158:1335"},
				{"three_seats_host_slows_while_the_other_joiner_is_held", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=3:silent:6000:10000 spell=1:slow:8500:4000:70"},
				{"seed12_four_direct_joiner_slow_74_percent_with_a_lossy_voter", "seed=12 seats=4 internet=0 exchange=100 end=14984 rtt=230,282,202,247,250,282 jitter=0,0,0,0,0,0 recover=300,100,300,300 phase=0,0,0,0 spell=2:slow:6000:984:74 burst=3:6119:2414:60"},
				{"seed12_four_internet_joiner_slow_74_percent_with_a_lossy_voter", "seed=12 seats=4 internet=1 exchange=100 end=14984 rtt=230,282,202,247,250,282 jitter=0,0,0,0,0,0 recover=300,100,300,300 phase=0,0,0,0 spell=2:slow:6000:984:74 burst=3:6119:2414:60"},
				{"seed69_four_internet_host_slow_58_percent_with_loss", "seed=69 seats=4 internet=1 exchange=100 end=16747 rtt=17,17,17,17,17,17 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=0,0,0,0 spell=1:slow:5080:3031:58 burst=2:4215:2601:60 burst=1:7939:139:60"},
				{"seed192_two_internet_host_stopped_then_the_joiner_leaves", "seed=192 seats=2 internet=1 exchange=100 end=22988 rtt=277 jitter=0 recover=105,300 phase=0,0 spell=1:stopped:6000:428 leave=2:10167"},
				// The reviewers' return shapes (hold-shapes part 1): a held seat back, caught up at 105 % and then at exactly the round's rate.
				// #5: three seats, the host held: every tail frame waits for both members' witnesses.
				{"three_seats_host_held_catches_up_at_105_percent_over_100ms", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=105,300,300 phase=1700,3400,0 spell=1:stopped:6000:800"},
				{"three_seats_host_held_catches_up_at_105_percent_over_250ms", "seed=0 seats=3 internet=1 exchange=100 rtt=250,250,250 jitter=0,0,0 recover=105,300,300 phase=1700,3400,0 spell=1:stopped:6000:800"},
				// #6, #7 and the four-seat return: one joiner held, the donor waiting for a second voter's witness.
				{"four_seats_joiner_held_catches_up_at_105_percent_over_100ms", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,105,300 phase=1700,3400,0,1700 spell=3:stopped:6000:800"},
				{"four_seats_joiner_held_catches_up_at_105_percent_over_150ms", "seed=0 seats=4 internet=1 exchange=100 rtt=150,150,150,150,150,150 jitter=0,0,0,0,0,0 recover=300,300,105,300 phase=1700,3400,0,1700 spell=3:stopped:6000:800"},
				{"four_seats_joiner_held_catches_up_at_105_percent_over_200ms", "seed=0 seats=4 internet=1 exchange=100 rtt=200,200,200,200,200,200 jitter=0,0,0,0,0,0 recover=300,300,105,300 phase=1700,3400,0,1700 spell=3:stopped:6000:800"},
				{"four_seats_joiner_held_catches_up_at_105_percent_over_250ms", "seed=0 seats=4 internet=1 exchange=100 rtt=250,250,250,250,250,250 jitter=0,0,0,0,0,0 recover=300,300,105,300 phase=1700,3400,0,1700 spell=3:stopped:6000:800"},
				// The same with its tail from a seat other than the proposer: seat 3 sits 20 ms from the returner, every other link 200 ms.
				{"four_seats_joiner_held_catches_up_from_a_near_donor_over_200ms", "seed=0 seats=4 internet=1 exchange=100 rtt=200,200,200,200,200,20 jitter=0,0,0,0,0,0 recover=300,300,300,105 phase=1700,3400,0,1700 spell=4:stopped:6000:800"},
				// #8: the tail's path at 250 ms with jitter on it.
				{"three_seats_joiner_silent_2s_returns_over_250ms_with_40ms_jitter", "seed=0 seats=3 internet=1 exchange=100 rtt=250,250,250 jitter=40,40,40 recover=300,300,300 phase=1700,3400,0 spell=2:silent:6000:2000"},
				// #13: a voter losing a third of its packets while the return is agreed.
				{"three_seats_joiner_returns_while_a_voter_loses_30_percent", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:silent:6000:2000 burst=3:7500:4000:30"},
				// #14: the proposer itself a little slow while it proposes the return.
				{"three_seats_joiner_returns_to_a_host_at_95_percent", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=1:slow:5000:15000:95 spell=2:silent:6000:2000"},
				// #16: held for a slow spell, back at 120 %: it returns once and stays returned.
				{"three_seats_joiner_slow_80_percent_6s_back_at_120_percent_stays_returned", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,120,300 phase=1700,3400,0 spell=2:slow:6000:6000:80"},
				// #18: two seats held at once return one after the other.
				{"four_seats_two_joiners_held_at_once_catch_up_at_105_percent_over_100ms", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,105,105 phase=1700,3400,0,1700 spell=3:stopped:6000:600 spell=4:stopped:6000:600"},
				// Known shapes. A seat whose inputs keep arriving, 200 ms past the bound.
				{"three_seats_joiner_inputs_200ms_late_for_5s_over_100ms", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:late:6000:5000:200"},
				// ADDENDUM 9: the host's own loop stops 2 s; the joiners hold the host's seat within the bound and it returns.
				{"two_internet_seats_host_loop_stopped_2s", "seed=0 seats=2 internet=1 exchange=100 rtt=100 jitter=0 recover=300,300 phase=1700,3400 spell=1:stopped:6000:2000"},
				{"two_direct_seats_host_loop_stopped_2s", "seed=0 seats=2 internet=0 exchange=100 rtt=100 jitter=0 recover=300,300 phase=1700,3400 spell=1:stopped:6000:2000"},
				{"four_seats_host_loop_stopped_2s", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 spell=1:stopped:6000:2000"},
				// A returned seat meets an autosave every 10 s: the host's loop, then the returned seat's own, stops 300 ms each time.
				{"three_seats_returned_joiner_meets_a_host_autosave_every_10s", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:silent:5000:2000 spell=1:stopped:10000:300 spell=1:stopped:20000:300 spell=1:stopped:30000:300"},
				{"three_seats_returned_joiner_autosaves_every_10s", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:silent:5000:2000 spell=2:stopped:10000:300 spell=2:stopped:20000:300 spell=2:stopped:30000:300"},
				// A hold while another seat still catches up privately.
				{"four_seats_hold_during_another_seats_private_catch_up", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,105,300,300 phase=1700,3400,0,1700 spell=2:stopped:6000:1500 spell=3:silent:12000:1000"},
				// The reviewers' referee shapes (hold-shapes part 2). #4: a split heals before any hold, then the epoch's winner lags.
				{"two_internet_seats_split_heals_before_a_hold_then_the_host_stalls_2s", "seed=0 seats=2 internet=1 exchange=100 rtt=100 jitter=0 recover=300,300 phase=1700,3400 spell=2:late:6000:400:150 spell=1:stopped:12000:2000"},
				// #5: four seats, a split {1,2}|{3,4} heals, then {1,3}|{2,4} splits in the same epoch.
				{"four_internet_seats_split_12_34_heals_then_13_24_splits", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 cut=3:6000:150:0 cut=5:12000:2000:0"},
				// #6: the tie comes while the seat's minute of directory requests is spent on routine check-ins.
				{"two_internet_seats_joiner_silent_2s_while_the_allowance_is_spent", "seed=0 seats=2 internet=1 exchange=100 rtt=100 jitter=0 recover=300,300 phase=1000,1000 spell=2:silent:6000:2000"},
				// #7: every tie exchange takes longer than the client's 250 ms.
				{"two_internet_seats_joiner_silent_2s_with_a_300ms_directory", "seed=0 seats=2 internet=1 exchange=300 rtt=100 jitter=0 recover=300,300 phase=1700,3400 spell=2:silent:6000:2000"},
				// #10 and hold #29: a voter present at the return frame whose votes and certificates stall while its inputs flow.
				{"three_seats_return_meets_a_voter_whose_reliable_lane_stalls", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:silent:6000:2000 spell=3:stall:8500:3000:2500"},
				// #11: link and directory dark for both halves 3 s, then a stall that inherits the client's backoff.
				{"two_internet_seats_link_and_directory_dark_3s_then_the_joiner_stalls", "seed=0 seats=2 internet=1 exchange=100 rtt=100 jitter=0 recover=300,300 phase=1700,3400 cut=1:6000:3000:3 spell=2:stopped:14000:1000"},
				// #14: a decided split asked again with other owners after a kick.
				{"four_internet_seats_decided_split_then_a_kick_then_a_stall", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 cut=3:6000:1500:0 kick=4:9000 spell=3:stopped:12000:1000"},
				// #15: the host kicks a seat while its own half has lost a 2:2 split it cannot see certified.
				{"four_internet_seats_host_kicks_while_its_half_lost_the_split", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 cut=3:6000:4000:3 kick=4:7000"},
				// #19 and hold #20: three retained seats with the host held; the two joiners split, or one lags.
				{"three_seats_host_held_then_the_joiners_split", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=1:silent:6000:10000 cut=2:10000:2000:0"},
				{"three_seats_host_held_then_a_joiner_stalls_1s", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=1:silent:6000:10000 spell=3:stopped:10000:1000"},
				{"three_seats_host_held_then_a_joiner_slow_70_percent", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=1:silent:6000:10000 spell=3:slow:9000:4000:70"},
				// #23: a split while a seat returns.
				{"four_internet_seats_split_while_a_seat_returns", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 spell=4:stopped:6000:1000 cut=3:8500:1500:0"},
				// The reviewers' hold shapes (hold-shapes part 3). #5: a seat's round trip steps up mid-match, below and past the margin.
				{"three_seats_joiner_150ms_later_for_10s", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:late:6000:10000:150"},
				{"three_seats_joiner_400ms_later_for_10s", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:late:6000:10000:400"},
				// #8: a slow machine from the round's first ticks, and slow again just after its return.
				{"three_seats_joiner_slow_50_percent_from_the_round_start", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:slow:0:10000:50"},
				{"three_seats_joiner_slow_again_just_after_its_return", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:slow:6000:4000:60 spell=2:slow:13000:3000:60"},
				// #16: the slow seat is near a voter and far from the proposer, so its input can beat the hold agreed ahead.
				{"three_seats_slow_joiner_near_a_voter_far_from_the_proposer", "seed=0 seats=3 internet=1 exchange=100 rtt=250,250,20 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:slow:6000:10000:70"},
				// #18: two seats with no directory, the host at fault.
				{"two_direct_seats_host_slow_80_percent_10s", "seed=0 seats=2 internet=0 exchange=100 rtt=100 jitter=0 recover=300,300 phase=1700,3400 spell=1:slow:6000:10000:80"},
				{"two_direct_seats_host_hung_2s", "seed=0 seats=2 internet=0 exchange=100 rtt=100 jitter=0 recover=300,300 phase=1700,3400 spell=1:hung:6000:2000"},
				// #21: four seats with no directory, the host in the pair cut off from the other.
				{"four_direct_seats_split_12_from_34_3s", "seed=0 seats=4 internet=0 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 cut=3:6000:3000:0"},
				// #34, #35: the host's loop stopped or its machine slow with three or four seats.
				{"three_seats_host_loop_stopped_2s", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=1:stopped:6000:2000"},
				{"four_seats_host_slow_80_percent_10s", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 spell=1:slow:6000:10000:80"},
				// #40: two machines slow at once in three seats.
				{"three_seats_two_joiners_slow_70_percent_at_once", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:slow:6000:10000:70 spell=3:slow:6000:10000:70"},
				{"host_slows_while_a_third_seat_is_already_held", "seed=0 seats=4 internet=1 exchange=100 rtt=100,100,100,100,100,100 jitter=0,0,0,0,0,0 recover=300,300,300,300 phase=1700,3400,0,1700 spell=3:silent:6000:10000 spell=1:slow:8500:4000:70"},
				// The return table's #13 on the proposer's side: the host loses a third of its packets while it proposes the return.
				{"three_seats_joiner_returns_while_the_host_loses_30_percent", "seed=0 seats=3 internet=1 exchange=100 rtt=100,100,100 jitter=0,0,0 recover=300,300,300 phase=1700,3400,0 spell=2:silent:6000:2000 burst=1:7500:4000:30"},
			};
			return rows;
		}
		// The random driver: N rounds from consecutive seeds, run on worker threads beside the fixed rows. Each distinct failure - its
		// property, its kind, its seats and the spells it had - is minimized and printed as a fixed row with its seed; the fixed schedule
		// rows run in the same pool. CC_TEST_HOLD_SCHEDULES sets N (default 200), CC_TEST_HOLD_SCHEDULE_SEED the first seed,
		// CC_TEST_HOLD_SCHEDULE_THREADS the workers, CC_TEST_HOLD_SCHEDULE_MINIMIZE_RUNS the rounds each minimization may spend.
		struct ScheduleDriver {
			size_t count = 0, threads = 0, budget = 0, shapes = 0, failedCount = 0, fixedCount = 0;
			uint64_t firstSeed = 0, elapsedMs = 0, budgetMs = 0;
			bool runRandom = true, reusedFixedCatalogue = false;
			std::map<std::string, std::string> priorFailures;
			SteadyClock::time_point began;
			std::vector<Schedule> schedules;
			std::vector<std::string> names, lines;
			std::vector<std::optional<Finding>> results;
			std::vector<uint64_t> startupWaits;
			std::vector<uint8_t> ran;
			std::atomic<size_t> ranCount{0};
			std::vector<std::pair<std::string, bool>> fixedResults;
			// This report is only the minimizer's index of failures just observed by the suite's
			// fixed collection with the same executable. No fixture peer or property reads it.
			bool ReadFixedFailureReport(const char* path) {
				std::ifstream input(path);
				std::string line;
				if (!std::getline(input, line) || line != "hold-fixed-groups-v1") return false;
				std::map<std::string, std::string> suffixes, reported;
				for (const auto& row: FixedScheduleRows()) {
					Schedule schedule;
					if (!ParseSchedule(row.schedule, schedule)) return false;
					suffixes.emplace(row.name, ":" + std::to_string(schedule.seats) + (schedule.internet ? ":internet" : ":direct"));
				}
				while (std::getline(input, line)) {
					const auto tab = line.find('\t');
					if (tab == std::string::npos || line.find('\t', tab + 1) != std::string::npos) return false;
					const std::string group = line.substr(0, tab), name = line.substr(tab + 1);
					const auto row = suffixes.find(name);
					if (row == suffixes.end() || std::count(group.begin(), group.end(), ':') != 3 || !group.ends_with(row->second)) return false;
					reported.try_emplace(group, name);
				}
				if (input.bad()) return false;
				priorFailures = std::move(reported);
				return true;
			}
			void Prepare(const char* selected) {
				began = SteadyClock::now();
				count = EnvironmentNumber("CC_TEST_HOLD_SCHEDULES", 200);
				firstSeed = EnvironmentNumber("CC_TEST_HOLD_SCHEDULE_SEED", 1);
				const unsigned hardware = std::max(2U, std::thread::hardware_concurrency());
				threads = std::max<size_t>(1, EnvironmentNumber("CC_TEST_HOLD_SCHEDULE_THREADS", std::max(2U, hardware - 1)));
				budget = EnvironmentNumber("CC_TEST_HOLD_SCHEDULE_MINIMIZE_RUNS", 60);
				shapes = EnvironmentNumber("CC_TEST_HOLD_SCHEDULE_SHAPES", 8);
				// Only minimization spends the time budget; every requested schedule must run.
				budgetMs = EnvironmentNumber("CC_TEST_HOLD_SCHEDULE_BUDGET_S", 200) * 1000;
				const bool driverSelected = selected && std::string_view(selected) == "random_hold_schedules";
				if (driverSelected) if (const char* report = std::getenv("CC_TEST_HOLD_FIXED_FAILURE_REPORT"))
					reusedFixedCatalogue = ReadFixedFailureReport(report);
				// A standalone driver without a valid current report runs every saved schedule itself.
				for (const auto& row: FixedScheduleRows()) {
					if (reusedFixedCatalogue) break;
					if (selected && !driverSelected && std::string_view(selected) != row.name) continue;
					Schedule schedule;
					if (!ParseSchedule(row.schedule, schedule)) { lines.push_back(std::string("FAIL ") + row.name + ": its schedule text does not parse"); fixedResults.emplace_back(row.name, false); continue; }
					if (!schedule.endMs) schedule.endMs = ScheduleEndMs(schedule);
					schedules.push_back(schedule); names.push_back(row.name);
				}
				fixedCount = schedules.size();
				runRandom = count > 0 && (!selected || driverSelected);
				if (runRandom) for (size_t index = 0; index < count; ++index) { schedules.push_back(GenerateSchedule(firstSeed + index)); names.emplace_back(); }
				results.resize(schedules.size()); startupWaits.resize(schedules.size()); ran.assign(schedules.size(), false);
			}
			// Distinct byte elements may be written by different workers without sharing a packed bit.
			void RunOne(size_t index) {
				results[index] = RunScheduleQuietly(schedules[index], &startupWaits[index]);
				ran[index] = true; if (index >= fixedCount) ++ranCount;
			}
			// Waits before every seat displayed its first second: the round's start, judged apart from the holds.
			std::string StartupWaits() const {
				size_t over = 0; uint64_t most = 0, seed = 0;
				for (size_t index = fixedCount; index < schedules.size(); ++index) {
					if (startupWaits[index] > static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs))) ++over;
					if (startupWaits[index] > most) { most = startupWaits[index]; seed = schedules[index].seed; }
				}
				return " startup_waits_past_bound=" + std::to_string(over) + " startup_longest_ms=" + std::to_string(most) + (most ? " startup_longest_seed=" + std::to_string(seed) : "");
			}
			void Finish() {
				const auto failureGroup = [&](size_t index) {
					return results[index]->property + ":" + results[index]->kind + ":" + std::to_string(schedules[index].seats) +
					       (schedules[index].internet ? ":internet" : ":direct");
				};
				for (size_t index = 0; index < fixedCount; ++index) {
					const auto& result = results[index];
					lines.push_back(std::string(result ? "FAIL " : "PASS ") + names[index] + (result ? ": seed=" + std::to_string(schedules[index].seed) + " property=" + result->property +
					                " kind=" + result->kind + ": " + result->detail : ""));
					fixedResults.emplace_back(names[index], !result);
					if (result) lines.push_back("saved_schedule_group name=" + names[index] + " group=" + failureGroup(index));
				}
				if (!reusedFixedCatalogue) lines.push_back("saved_schedule_catalogue expected=" + std::to_string(FixedScheduleRows().size()) +
				                                         " ran=" + std::to_string(std::count(ran.begin(), ran.begin() + fixedCount, uint8_t{1})));
				if (!runRandom) { elapsedMs = Elapsed(began); return; }
				// Group the failures by what failed, the seats and whether a referee decides ties, and minimize the smallest round of each
				// group; the minimized shapes are kept once each.
				std::map<std::string, std::string> reproduced = priorFailures;
				for (size_t index = 0; index < fixedCount; ++index)
					if (results[index]) reproduced.try_emplace(failureGroup(index), names[index]);
				std::map<std::string, std::vector<size_t>> groups;
				for (size_t index = fixedCount; index < schedules.size(); ++index) {
					if (!results[index]) continue;
					++failedCount;
					groups[failureGroup(index)].push_back(index);
					// Each failing round's own finding, so a run names every seed's cause and not only its group's smallest.
					lines.push_back("random_seed_finding seed=" + std::to_string(schedules[index].seed) + " group=" + failureGroup(index) + ": " + results[index]->detail.substr(0, 320) +
					                " schedule=" + FormatSchedule(schedules[index]));
				}
				std::map<std::string, std::string> minimized;
				size_t done = 0;
				for (const auto& [key, members]: groups) {
					const size_t smallest = *std::min_element(members.begin(), members.end(), [&](size_t a, size_t b) { return schedules[a].endMs < schedules[b].endMs; });
					const std::string seeds = [&] { std::string text; for (size_t member: members) text += (text.empty() ? "" : ",") + std::to_string(schedules[member].seed); return text; }();
					// A named regression just reproduced this same failure group. Keep every random failure red and spend minimization
					// on new groups, rather than repeatedly minimizing a counterexample already running in this suite.
					if (const auto fixed = reproduced.find(key); fixed != reproduced.end()) {
						lines.push_back("FAIL random_hold_schedules group=" + key + " seeds=" + seeds + ": reproduced by fixed row " + fixed->second);
						continue;
					}
					if (done++ >= shapes || Elapsed(began) >= budgetMs) {
						lines.push_back("FAIL random_hold_schedules group=" + key + " seeds=" + seeds + ": not minimized (past CC_TEST_HOLD_SCHEDULE_SHAPES or _BUDGET_S), seed " +
						                std::to_string(schedules[smallest].seed) + ": " + results[smallest]->detail);
						lines.push_back("random_hold_schedules pending {\"" + NameSchedule(schedules[smallest]) + "_seed_" + std::to_string(schedules[smallest].seed) + "\", \"" + FormatSchedule(schedules[smallest]) + "\"}");
						continue;
					}
					size_t runs = 0; Finding last;
					const Schedule shape = Minimize(schedules[smallest], *results[smallest], static_cast<unsigned>(threads), budget, runs, last, began + std::chrono::milliseconds(budgetMs));
					// The shape must fail again by itself, the same way, to be kept as a row.
					const auto again = RunScheduleQuietly(shape);
					const bool repeats = again && again->property == last.property && again->kind == last.kind;
					const std::string name = NameSchedule(shape) + "_" + last.kind + "_seed_" + std::to_string(shape.seed);
					if (minimized.contains(name)) { lines.push_back("FAIL random_hold_schedules group=" + key + " seeds=" + seeds + ": the shape of " + name); continue; }
					minimized[name] = FormatSchedule(shape);
					lines.push_back("FAIL random_hold_schedules shape=" + name + " group=" + key + " seeds=" + seeds + " property=" + last.property + " kind=" + last.kind +
					                " minimize_runs=" + std::to_string(runs) + " repeats=" + (repeats ? "1" : "0") + ": " + last.detail);
					lines.push_back("random_hold_schedules row {\"" + name + "\", \"" + FormatSchedule(shape) + "\"}");
				}
				elapsedMs = Elapsed(began);
			}
			static uint64_t Elapsed(SteadyClock::time_point since) {
				return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - since).count());
			}
		};
		bool CheckPrivateReturnShape(std::string_view shape, uint32_t rttMs, std::string& error) {
			const auto fail = [&](const std::string& detail) { error = "seed=0 " + detail; return false; };
			auto config = NetMatchConfigUtil::MakeDefault(0xF002);
			config.peerCount = 4; config.players = {{1, 0, false, "Host"}, {2, 1, false, "Returner"}, {3, 2, false, "Other"}, {4, 3, false, "Witness"}};
			NetWorldJoinHost host;
			if (!host.ConfigureMatchRejoins(config, 77, c_TestTickMs, &error) || !host.BeginRejoin(12, 1, 2, 2, "Returner", 1, &error)) return false;
			NetWorldCheckpointImage image; image.privateSessionId = config.sessionId; image.round = 77; image.tick = 400;
			image.bytes = 8; image.checkpointConfig = "config"; image.sideState = "state";
			host.PublishImage(image);
			if (shape == "benign") {
				NetSeatRoster roster; roster.matchId = config.sessionId; roster.roundNo = 77; roster.stage = NetRosterStage::Running; roster.revision = 1;
				for (uint8_t seat = 1; seat <= 4; ++seat) {
					NetRosterSeat entry; entry.seatId = seat; entry.owner = seat; entry.ticket = 100 + seat; entry.incarnation = 1;
					entry.phase = seat == 2 ? NetSeatPhase::Held : NetSeatPhase::Running; entry.holdCause = seat == 2 ? NetSeatHoldCause::LinkDrop : NetSeatHoldCause::None;
					roster.seats.push_back(entry);
				}
				for (unsigned attempt = 0; attempt < 6; ++attempt) {
					NetRosterEvent returned; returned.kind = NetRosterEventKind::Returned; returned.seat = 2; returned.ticket = 102; returned.nowMs = 1000 + attempt;
					auto admitted = ApplyRosterEvent(roster, returned);
					if (admitted.refused) return fail("a benign cancellation exhausted the return offers: " + admitted.reason);
					roster = std::move(admitted.roster);
					host.CancelJoin(12, attempt % 2 ? "the capture park moved" : "the seat was held again", false);
					const auto causes = host.TakeCancelledJoinCauses();
					if (causes != std::vector<std::pair<uint8_t, bool>>{{2, false}}) return fail("a host-side cancellation was charged to its returner");
					NetRosterEvent aborted; aborted.kind = NetRosterEventKind::TransferAborted; aborted.seat = 2; aborted.nowMs = returned.nowMs; aborted.excused = !causes.front().second;
					auto cancelled = ApplyRosterEvent(roster, aborted);
					if (cancelled.refused || cancelled.roster.Find(2)->failedReturns != 0 || cancelled.roster.Find(2)->returnAfterMs != 0)
						return fail("a benign cancellation counted toward the three-failure backoff");
					roster = std::move(cancelled.roster);
					if (!host.BeginRejoin(12, 1, 2, 3 + attempt, "Returner", returned.nowMs, &error)) return false;
				}
				return true;
			}
			if (!host.NoteTransferComplete(12, 8, &error)) return false;
			host.NoteRejoinLinkFit(12, true); host.NoteTailLinkRtt(12, rttMs);
			if (shape == "capacity-projection") {
				if (!host.NoteRejoinCapacity(12, 120, 1904762, 0)) return fail("the 105-percent work sample was refused");
				uint64_t activation = 0;
				if (!host.NoteCatchUpProgress(12, 540, 0, 1, 600, &activation, &error) ||
				    !host.NoteCatchUpProgress(12, 610, 70, 1000, 660, &activation, &error)) return false;
				return activation ? fail("a bursty closing window projected beyond the measured machine capacity") : true;
			}
			if (shape == "phase-runway" || shape == "phase-over-cap") {
				const uint64_t behind = shape == "phase-over-cap" ? 80 : rttMs == 200 ? 13 : 16;
				if (!host.NoteRejoinCapacity(12, 120, 1904762, 0)) return fail("the 105-percent work sample was refused");
				uint64_t activation = 0;
				for (uint64_t horizon = 600; horizon <= 720 && !activation; horizon += 30)
					if (!host.NoteCatchUpProgress(12, horizon - behind, 30, 500, horizon, &activation, &error)) return false;
				if (shape == "phase-over-cap") return activation ? fail("the return exceeded the unchanged neutral interval cap") : true;
				const auto* session = host.FindSession(12);
				const uint64_t transit = static_cast<uint64_t>(std::floor(rttMs / (2 * c_TestTickMs)));
				const double ratio = session->headroom.Ratio();
				const uint64_t workNeeded = static_cast<uint64_t>(std::ceil((behind - transit) * ratio / (ratio - 1)));
				if (!activation || session->activationTrailFrames < workNeeded)
					return fail("a transit-limited 105-percent return has no interval to align its producer: activation=" + std::to_string(activation) +
					    " trail=" + std::to_string(session->activationTrailFrames) + " work_needed=" + std::to_string(workNeeded));
				const uint64_t flight = static_cast<uint64_t>(std::ceil(rttMs / c_TestTickMs));
				if (2 * flight + 1 + config.slowPlayerBoundTicks + session->activationTrailFrames > NetLockstepCodec::c_MaxFutureFrameSkew)
					return fail("the producer alignment exceeds the unchanged neutral interval cap");
				return true;
			}
			if (shape == "edge") {
				uint64_t work = 0, workUs = 0;
				for (uint64_t horizon = 600; horizon <= 900; horizon += 20) {
					work += 20; workUs += 333334;
					if (!host.NoteRejoinCapacity(12, work, workUs, 0)) return fail("valid round-rate capacity was refused");
					uint64_t activation = 0;
					if (!host.NoteCatchUpProgress(12, horizon - 80, 20, 334, horizon, &activation, &error)) return false;
					if (activation != 0) return fail("an image return eighty frames behind at the round's rate was activated over " + std::to_string(rttMs) + "ms");
				}
				return true;
			}
			if (shape == "paired") {
				if (!host.BeginRejoin(13, 2, 3, 2, "Other returner", 2, &error) || !host.NoteTransferComplete(13, 8, &error)) return false;
				host.NoteRejoinLinkFit(13, true); host.NoteTailLinkRtt(13, rttMs);
			}
			uint64_t activation = 0, prior = 0, applied = 400, work = 0, workUs = 0;
			for (uint64_t horizon = 600; horizon <= 1000 && !activation; horizon += 20) {
				applied += 40; work += 40; workUs += 320000;
				(void)host.NoteRejoinCapacity(12, work, workUs, 0);
				if (!host.NoteCatchUpProgress(12, applied, 40, 334, horizon, &activation, &error)) return false;
				if (activation) {
					const auto* session = host.FindSession(12);
					const uint64_t behind = horizon > applied ? horizon - applied : 0;
					if (!session || !session->closingMeasured || session->closingRate <= 0 ||
					    activation < horizon + static_cast<uint64_t>(std::ceil(behind / session->closingRate)) + c_NetWorldActivationLeadFrames)
						return fail("the image activation precedes the returner's measured catch-up");
					prior = activation;
				}
				if (shape == "paired") {
					(void)host.NoteRejoinCapacity(13, work / 2, workUs * 2, 0);
					uint64_t otherActivation = 0;
					if (!host.NoteCatchUpProgress(13, horizon - 100, 20, 334, horizon, &otherActivation, &error)) return false;
					if (otherActivation || host.FindSession(13)->snapshotTick != image.tick) return fail("one image returner's progress activated the other or changed its shared base");
				}
			}
			if (!activation) return fail("a returner closing twice as fast as the round was never activated");
			if (shape == "closing") {
				// A delayed progress report must preserve the time needed to close, rather than reset E to a fixed lead.
				const uint64_t horizon = activation - c_NetWorldActivationLeadFrames / 2;
				work += 40; workUs += 320000; (void)host.NoteRejoinCapacity(12, work, workUs, 0);
				if (!host.NoteCatchUpProgress(12, applied + 40, 40, 334, horizon, &activation, &error)) return false;
				const auto* session = host.FindSession(12);
				const uint64_t behind = horizon > applied + 40 ? horizon - applied - 40 : 0;
				if (!session || session->activationTick < prior || (session->closingRate > 0.1 &&
				    session->activationTick < horizon + static_cast<uint64_t>(std::ceil(behind / session->closingRate)) + c_NetWorldActivationLeadFrames))
					return fail("re-announcing the activation discarded the returner's measured closing time");
			} else if (shape == "paired") {
				host.CancelJoin(13, "its base was refused", false);
				if (!host.FindSession(12) || host.FindSession(12)->activationTick != activation || host.Image().tick != image.tick)
					return fail("a refused image return poisoned the other returner's activation or base");
			}
			return true;
		}
		bool CheckReplayConvergence(std::string& error) {
			struct Release { ~Release() { ScenarioRunner::ReleaseWorldCatchUp(); } } release;
			std::vector<NetLockstepFrame> tail;
			const auto appendFrame = [](std::vector<NetLockstepFrame>& frames, uint64_t tick) {
				NetLockstepFrame frame; frame.senderPeerId = 1; frame.roundId = 77; frame.targetFrame = tick; frames.push_back(std::move(frame));
			};
			for (uint64_t tick = 401; tick <= 640; ++tick) appendFrame(tail, tick);
			if (!ScenarioRunner::InstallWorldCatchUp(400, std::move(tail), &error)) return false;
			uint64_t applied = 400, committed = 640; double elapsedMs = 0;
			const auto began = SteadyNow();
			const auto at = [&](double ms) { return began + std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double, std::milli>(ms)); };
			const auto receiveCommitted = [&] {
				std::vector<NetLockstepFrame> arrived;
				const uint64_t head = 640 + static_cast<uint64_t>(elapsedMs / c_TestTickMs);
				while (committed < head) appendFrame(arrived, ++committed);
				ScenarioRunner::AppendWorldCatchUp(std::move(arrived));
			};
			while (elapsedMs < 120000) {
				ScenarioRunner::BeginWorldCatchUpFrame(10.0, -1.0, at(elapsedMs));
				while (ScenarioRunner::TakeWorldCatchUpGrant(applied + 1, false, at(elapsedMs))) {
					NetLockstepReadyFrame ready;
					if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(applied + 1, ready, &error)) return false;
					++applied; elapsedMs += 12.0; ScenarioRunner::NoteWorldCatchUpTickCost(applied, 12000, 12000);
					// A tail can arrive between tick boundaries, but never before the playing round committed it.
					receiveCommitted();
					if (applied >= committed) return true;
				}
				elapsedMs += 10.0;
				receiveCommitted();
			}
			error = "seed=0 private replay at 12ms per tick plus a 10ms draw never closed its 240-frame trail"; return false;
		}
		bool CheckRelayReturnAndDepartures(std::string& error) {
			Case row{"three_peer_relay_host_return"};
			row.seats = 3; row.subject = 1; row.gapMs = 8000; row.blackout = true;
			row.rttMs = 250; row.otherRttMs = 100;
			if (!CheckSplitContract(row, error)) return false;
			row.blackout = false; row.hung = true;
			Fixture recovered;
			if (!recovered.Run(row, error)) return false;
			for (uint8_t id = 1; id <= 3; ++id) {
				if (!recovered.caughtAt[id - 1] || recovered.caughtAt[id - 1] > recovered.hub.faultEnd + 10000) {
					error = "relay host did not return to every observer within ten seconds"; return false;
				}
				if (id != 1 && recovered.hub.dials[{id, 1}] != 1) {
					error = "a healthy primary host route was redialed"; return false;
				}
			}
			// Poll phases and the recovery RTT follow the measured relay path:
			// an eligible early vote must not wait for a retransmission slot,
			// and the first human input must survive the larger return path.
			Case phaseRow = row;
			phaseRow.name = "relay_phased_return";
			phaseRow.rttMs = 98; phaseRow.otherRttMs = 150;
			phaseRow.planeIntervalMs = 8; phaseRow.recoveryRttMs = 306;
			phaseRow.delayStartup = true;
			Fixture phased;
			if (!phased.Run(phaseRow, error)) return false;
			for (uint8_t id = 1; id <= 3; ++id) {
				const auto& peer = *phased.peers[id - 1];
				if (!phased.returnedAt[id - 1] || phased.returnedAt[id - 1] > phased.hub.faultEnd + 10000 ||
				    phased.postReturnWait[id - 1] > 50 || peer.GetStats().peers.at(1).rejoins != 1 ||
				    (id != 1 && (phased.blackoutGap[id - 1] > 50 || phased.preFaultPaceLeadMs[id - 1] < 60))) {
					error = "phased relay return failed: peer=" + std::to_string(id) + " blackout_gap_ms=" + std::to_string(phased.blackoutGap[id - 1]) +
					    " pre_fault_pace_lead_ms=" + std::to_string(phased.preFaultPaceLeadMs[id - 1]) +
					    " return_ms=" + std::to_string(phased.returnedAt[id - 1] ? phased.returnedAt[id - 1] - phased.hub.faultEnd : 0) +
					    " post_return_wait_ms=" + std::to_string(phased.postReturnWait[id - 1]) + " returns=" + std::to_string(peer.GetStats().peers.at(1).rejoins); return false;
				}
				DiagnosticLine() << "[net-session-plane-selftest] relay-phased peer=" << static_cast<int>(id) << " blackout_gap_ms=" << phased.blackoutGap[id - 1]
				          << " pre_fault_pace_lead_ms=" << phased.preFaultPaceLeadMs[id - 1]
				          << " post_return_wait_ms=" << phased.postReturnWait[id - 1] << " return_ms=" << phased.returnedAt[id - 1] - phased.hub.faultEnd
				          << " returns=" << peer.GetStats().peers.at(1).rejoins << " result=PASS" << std::endl;
			}
			// A returned seat can go silent again while its consumers wait on the next AI frame.
			Case repeatRow = phaseRow;
			repeatRow.name = "relay_repeated_hold_return";
			Fixture repeated;
			if (!repeated.Run(repeatRow, error)) return false;
			repeated.hub.faultAt = repeated.hub.now + 4000;
			repeated.hub.faultEnd = repeated.hub.faultAt + repeatRow.gapMs;
			std::fill(repeated.caughtAt.begin(), repeated.caughtAt.end(), 0);
			std::fill(repeated.returnedAt.begin(), repeated.returnedAt.end(), 0);
			std::fill(repeated.blackoutGap.begin(), repeated.blackoutGap.end(), 0);
			std::fill(repeated.postReturnWait.begin(), repeated.postReturnWait.end(), 0);
			std::fill(repeated.seenAI.begin(), repeated.seenAI.end(), false);
			if (!repeated.RunStarted(repeatRow, error)) { error = "repeated blocked consumer: " + error; return false; }
			for (uint8_t id = 1; id <= 3; ++id) {
				const auto& peer = *repeated.peers[id - 1];
				if (!repeated.seenAI[id - 1] || peer.GetStats().peers.at(1).rejoins != 2 ||
				    repeated.returnedAt[id - 1] > repeated.hub.faultEnd + 10000 ||
				    (id != 1 && repeated.blackoutGap[id - 1] > 50)) {
					error = "the repeated hold did not preserve its consumer bound and automatic return: peer=" + std::to_string(id); return false;
				}
				DiagnosticLine() << "[net-session-plane-selftest] relay-repeated peer=" << static_cast<int>(id)
				    << " blackout_gap_ms=" << repeated.blackoutGap[id - 1] << " max_wait_ms=" << repeated.maxWait[id - 1]
				    << " returns=" << peer.GetStats().peers.at(1).rejoins << " result=PASS" << std::endl;
			}
			Case receiptRow = phaseRow;
			receiptRow.name = "relay_receipt_gap_after_return"; receiptRow.receiptGap = true; receiptRow.postReturnMs = 200000;
			Fixture receipts;
			if (!receipts.Run(receiptRow, error)) { error = "receipt gap: " + error; return false; }
			if (!receipts.hub.receiptGapFrame || !receipts.hub.receiptGapDrops || !receipts.hub.receiptGapRequestDrops) { error = "the post-return receipt gap and its missing frame requests were not exercised"; return false; }
			if (!receipts.hub.tailQueuePeak || receipts.hub.tailQueuePeak > 256 * 1024) { error = "duplicate recovery requests exceeded the reliable queue bound"; return false; }
			for (uint8_t id = 1; id <= 3; ++id) {
				const auto& peer = *receipts.peers[id - 1];
				const auto report = peer.BuildReportJson();
				if (!AcceptedThrough(peer) || *AcceptedThrough(peer) < receipts.worlds[id - 1].applied ||
				    peer.InputDelayAt(id, receipts.worlds[id - 1].applied) > 36 ||
				    peer.GetStats().peers.at(3).substitutions != 0) {
					error = "post-return input acceptance stopped, exceeded its delay bound, or held the healthy sender"; return false;
				}
				if (id == 3 && report.find("\"peer_receipt_repairs\":0,") != std::string::npos) { error = "receipt gap repair path was not exercised"; return false; }
				DiagnosticLine() << "[net-session-plane-selftest] relay-receipt peer=" << static_cast<int>(id) << " gap_frame=" << receipts.hub.receiptGapFrame
				    << " applied=" << receipts.worlds[id - 1].applied << " accepted_through=" << *AcceptedThrough(peer)
				    << " delay_ticks=" << peer.InputDelayAt(id, receipts.worlds[id - 1].applied) << " result=PASS" << std::endl;
			}
			// Both an announced round end and an explicit transport close must
			// release an excluded reader. Unannounced loss remains a partition.
			for (unsigned arm = 0; arm < 3; ++arm) {
				Fixture ended;
				if (!ended.Start(row, error)) return false;
				auto& host = *ended.peers[0];
				// Wait for the reader to consume the hold boundary. With 250 ms
				// RTT, half a second after the outage is still before that frame.
				while (ended.hub.now <= ended.hub.faultEnd + 3000 && !host.HasHeldAISeat(1)) if (!ended.Step(error)) return false;
				if (!host.HasHeldAISeat(1)) { error = "departure lever did not leave the host reading a held seat"; return false; }
				const uint64_t applied = ended.worlds[0].applied, at = ended.hub.now;
				if (arm == 0) {
					ended.peers[1]->Complete("member reached its end"); ended.peers[2]->Complete("member reached its end");
				} else {
					ended.hub.cleanRemoteClose = arm == 1;
					for (auto* wire: ended.hub.wires) if (wire->owner != 1) wire->Stop();
				}
				while (ended.hub.now <= at + 10000 && host.IsRunning()) { host.Tick(ended.hub.now); ++ended.hub.now; }
				if (host.IsFailed() || host.GetResumeFrame() != applied + 1 ||
				    (arm == 2 ? !host.IsRunning() : !host.IsStopped() || !host.GetStats().timeoutReason.starts_with("Complete:"))) {
					error = "last-donor departure was not bounded, rewrote progress, or treated a partition as an end: arm=" + std::to_string(arm); return false;
				}
				DiagnosticLine() << "[net-session-plane-selftest] relay-departure arm=" << arm << " elapsed_ms=" << ended.hub.now - at
				          << " applied=" << applied << " stopped=" << host.IsStopped() << " result=PASS" << std::endl;
			}
			return true;
		}
	}

	bool NetSessionPlaneSelfTest::CheckHeldHostControllers(std::string* error) {
		struct Wire final : LoopbackTransport {
			bool Send(NetPeerId, NetTransportLane, const std::vector<uint8_t>&, std::string* = nullptr, bool* = nullptr) override { return true; }
		};
		if (!TimerMan::IsConstructed()) TimerMan::Construct();
		if (!LuaMan::IsConstructed()) LuaMan::Construct();
		if (!SimChecksum::IsConstructed()) SimChecksum::Construct();
		if (!MovableMan::IsConstructed()) MovableMan::Construct();
		if (!ActivityMan::IsConstructed()) ActivityMan::Construct();
		if (!AudioMan::IsConstructed()) AudioMan::Construct();
		if (!SettingsMan::IsConstructed()) SettingsMan::Construct();
		if (!SceneMan::IsConstructed()) {
			install_allegro(SYSTEM_NONE, &errno, std::atexit);
			SceneMan::Construct();
		}
		Wire wire;
		NetLockstepCoordinator peer;
		peer.m_Config.sessionId = 0x60584422;
		peer.m_Config.matchConfig = NetMatchConfigUtil::MakeDefault(peer.m_Config.sessionId);
		peer.m_Config.matchConfig.peerCount = 3;
		peer.m_Config.matchConfig.players.push_back({3, 2, false, "Third"});
		peer.m_Config.matchConfig.successorOrder = {2, 3};
		peer.m_Config.matchConfig.roundId = 77;
		peer.m_Config.localPeerId = 2;
		peer.m_Config.peerCount = 3;
		peer.m_Config.startFrame = 1;
		peer.m_Config.simTickMs = 1000.0 / 60;
		peer.m_Config.migrationKey.fill(0x39);
		peer.m_State = NetLockstepState::Running;
		peer.m_Transport = &wire;
		peer.m_RemotePeerIds = {1, 3};
		peer.m_RemoteTransports = {{1, 42}};
		peer.m_RoundId = 77;
		peer.m_RoundConfigHash = NetMatchConfigUtil::HashConfig(peer.m_Config.matchConfig);
		peer.m_LastCompletedSimulationTick = 4422;
		peer.m_Stats.nextFrame = 4423;
		peer.m_MigrationPhase = NetHostMigrationPhase::Recovering;
		peer.m_MigrationSuccessor = 2;
		peer.m_MigrationGeneration = 1;
		peer.m_MigrationWireRound = 77;
		peer.m_MigrationBoundary = 6058;
		peer.m_MigrationPeers = {{3, 43}};
		peer.m_MigrationQuorumMask = 7;
		peer.m_MigrationVoterMask = 6;
		peer.m_MigrationResult = {1, 6058, 2, {2, 3}, {}, {{3, 43}}};
		for (uint8_t id : {2, 3}) {
			auto answer = peer.MigrationMessage(NetHostMigrationMessageType::Answer);
			answer.senderPeerId = id;
			peer.m_MigrationAnswers[id] = answer;
		}
		peer.m_Config.authorityPeerId = 1;
		peer.m_Config.peerFrameGroups = true;
		peer.m_MigrationPhase = NetHostMigrationPhase::None;
		peer.m_Stats.nextFrame = 728;
		peer.m_LastDeliveredFrame = 727;
		peer.m_PeerBridges[1] = {727, 727, true, {}, {}, 6};
		peer.m_AiHeldSeats[1] = peer.m_PeerLeaveFrames[1] = 727;
		peer.m_SeatTransitions[1][727] = NetLockstepCoordinator::SeatTransition::Held;
		std::unique_ptr<Actor> actor = std::make_unique<Actor>();
		if (actor->MovableObject::Create(1.0F) < 0) { *error = "held-host fixture could not create its actor"; return false; }
		actor->SetTeam(0);
		Controller& controller = *actor->GetController();
		controller.SetControlledActor(actor.get());
		NetLockstepReadyFrame handoff;
		handoff.frame = 727;
		handoff.aiHeldPeerIds = {1};
		bool valid = true;
		for (const bool futureReturn: {false, true}) {
			if (futureReturn) {
				peer.m_SeatTransitions[1][761] = NetLockstepCoordinator::SeatTransition::Back;
				peer.m_ReclaimTransactions[1] = {1, 1, 761 * 8 + 1, 1, 761, 5, 790};
				peer.m_PeerBridges[1].returnFrame = 761;
			}
			for (const bool disabled: {false, true}) {
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				controller.ResetLocalInputState(Controller::CIM_PLAYER, 0);
				controller.ApplyWireMode(Controller::CIM_PLAYER, 0);
				controller.SetDisabled(disabled);
				ScenarioRunner::SetLockstepCoordinator(&peer);
				NetActorOwnership::SeedOwner(actor->GetUniqueID(), 1, 0);
				const bool gone = peer.IsPeerGoneAtFrame(1, 727);
				ApplyLockstepLeaveHandoffs(handoff, {actor.get()}, false);
				const uint8_t producer = ScenarioRunner::GetLockstepActorOwner(actor->GetUniqueID(), 0, true, 727);
				std::cout << "[net-lockstep-selftest] held_host_handoff future_return=" << futureReturn
				          << " gone=" << gone << " initial_disabled=" << disabled << " disabled=" << controller.IsQuickDisabled()
				          << " mode=" << controller.GetInputMode() << " producer=" << static_cast<int>(producer) << std::endl;
				valid &= !gone && producer == 2 && controller.GetInputMode() == Controller::CIM_AI && controller.IsQuickDisabled() == disabled;
			}
		}
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		NetActorOwnership::ClearSeededOwners();
		// The hub's held seat is not a departed hub. An explicit departure still is, and a held client keeps its existing rule.
		peer.m_SeatTransitions[1][900] = NetLockstepCoordinator::SeatTransition::Left;
		peer.m_PeerLeaveFrames[1] = 900;
		peer.m_ReclaimTransactions.erase(1);
		peer.m_PeerLeaveFrames[3] = peer.m_AiHeldSeats[3] = 727;
		peer.m_SeatTransitions[3][727] = NetLockstepCoordinator::SeatTransition::Held;
		valid &= peer.IsPeerGoneAtFrame(1, 900) && peer.IsPeerGoneAtFrame(3, 727);
		if (!valid) *error = "a held host's controller disable depends on whether its future return has arrived";
		return valid;
	}

	bool NetSessionPlaneSelfTest::CheckOwnerHitch(unsigned arm, std::string* error) {
		std::call_once(s_FixtureObservers, [] {
			s_FrameMembers = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerFrameMembersAt(frame); };
			s_FrameAuthor = [](const NetLockstepCoordinator& peer, const NetLockstepReadyFrame& ready) -> uint8_t {
				const uint8_t local = peer.m_Config.localPeerId;
				const uint32_t members = peer.PeerFrameMembersAt(ready.frame);
				if (!peer.UsesPeerFrameGroups() || (members & (1U << (local - 1)))) return local;
				if (!peer.IsLocalPrivateReaderAt(ready.frame)) return 0;
				const auto witnesses = peer.m_PeerFrameWitnesses.find(ready.frame);
				if (witnesses == peer.m_PeerFrameWitnesses.end()) return 0;
				return WitnessedFrameAuthor(ready.frame, members, peer.CommittedFramePrefix(ready), peer.PeerFramePrefix(ready.frame), witnesses->second);
			};
			s_WaitState = [](const NetLockstepCoordinator& peer) {
				std::string starts;
				for (uint8_t remote: peer.m_RemotePeerIds) starts += " start=" + std::to_string(remote) + ":" + std::to_string(peer.m_RemoteStartsReceived.contains(remote)) +
				    "/required=" + std::to_string(peer.IsRemoteRequiredForFrame(remote, peer.m_Config.startFrame)) + "/held=" + std::to_string(peer.IsSeatUnderAI(remote, peer.m_Config.startFrame));
				return " next=" + std::to_string(peer.m_Stats.nextFrame) + " blocked=" + peer.m_AdvanceBlock +
				    " consumer=" + (peer.m_ConsumerWaitingFrame ? std::to_string(*peer.m_ConsumerWaitingFrame) : "none") +
				    " wait_since=" + std::to_string(peer.m_ConsumerWaitStartMs) + " members=" + std::to_string(peer.m_FrameGroupMembers) +
				    " local_held=" + std::to_string(peer.m_LocalSeatHeld) + " granted=" + (peer.m_GrantedSimulationTick ? std::to_string(*peer.m_GrantedSimulationTick) : "none") +
				    " hold_certificate=" + std::to_string(NetPeerFrameDetail::HasHoldDecision(peer.m_PeerBridgeCertificates, peer.m_Stats.nextFrame)) + starts;
			};
			s_HeldSeats = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerHeldSeatsAt(frame); };
			s_HoldBoundaries = [](const NetLockstepCoordinator& peer) -> const HoldBoundaries& { return peer.m_PeerFrameHolds; };
			s_OwnHoldPending = [](const NetLockstepCoordinator& peer, uint64_t frame) {
				for (const auto& [key, kept]: peer.m_PendingPeerDecisions)
					if (NetPeerFrameDetail::IsHoldDecision(key.second) && key.first >= frame && std::find(kept.first.members.begin(), kept.first.members.end(), peer.GetConfig().localPeerId) != kept.first.members.end()) return true;
				return false;
			};
			s_ReturnRoute = [](const NetLockstepCoordinator& peer, uint8_t seat) { return peer.PeerFrameRoute(seat); };
			s_ReturnTransport = [](const NetLockstepCoordinator& peer, uint8_t seat) {
				// FLAGGED fixture departure: a surviving mesh proposer can own a listener/dial route outside the old host star.
				return peer.PeerFrameRoute(seat).second;
			};
		});
		Case row{"owner_hitch"};
		row.seats = arm == 4 ? 2 : 3; row.subject = arm == 3 ? 1 : 2;
		row.gapMs = arm == 0 ? 2000 : arm == 1 ? 6000 : arm == 2 ? 10000 : 3000;
		row.frozen = arm != 1; row.realFrozen = arm == 2 || arm == 4; row.captureFrames = true;
		// Two seats over the internet: the joiner's inputs run ahead of its stopped loop, so only its unanswered receipts show the stop.
		if (arm == 4) { row.internet = row.directoryBoth = row.strictBound = true; row.rttMs = row.otherRttMs = 60; }
		Fixture fixture;
		NetMatchService service;
		uint64_t keepalives = 0;
		const auto disarm = [&] {
			service.DisarmSessionLiveness();
			if (service.m_LivenessThread.joinable()) { service.m_LivenessThread.request_stop(); service.m_LivenessThread.join(); }
			keepalives = service.m_Liveness.sentWhileBusy;
			// These two are borrowed for the sender's lifetime; the fixture owns them.
			(void)service.m_Coordinator.release(); (void)service.m_MigratedTransport.release();
		};
		struct Cleanup { std::function<void()> run; ~Cleanup() { run(); } } cleanup{disarm};
		fixture.onFreeze = [&](NetLockstepCoordinator& peer, INetTransport& wire) {
			service.m_Coordinator.reset(&peer); service.m_MigratedTransport.reset(&wire);
			service.ArmSessionLivenessLocked();
		};
		fixture.onThaw = disarm;
		std::string why;
		if (!fixture.Run(row, why)) { if (error) *error = why; return false; }
		// One keepalive per 100 ms of stopped loop: a Windows sender wakes on the system timer tick, so its cadence is coarser than Linux's.
		if ((arm == 2 || arm == 4) && keepalives < row.gapMs / 100) {
			if (error) *error = "the stopped main loop did not retain its independent authenticated sender: " + std::to_string(keepalives) + " in " + std::to_string(row.gapMs) + " ms";
			return false;
		}
		const size_t first = row.subject == 1 ? 1 : 0, second = row.seats > 2 ? 2 : 1 - first;
		if (second == first) { if (error) *error = "the controller comparison named one peer twice"; return false; }
		uint64_t heldAt = 0, returnedAt = 0;
		for (const auto& [tick, ready]: fixture.committed[first]) for (const auto* commands: {&ready.localCommands, &ready.remoteCommands}) for (const auto& command: *commands) {
			if (const auto* hold = std::get_if<NetGameSeatHold>(&command.payload); hold && hold->peerId == row.subject) heldAt = tick;
			if (const auto* back = std::get_if<NetGameSeatReclaim>(&command.payload); back && back->peerId == row.subject) returnedAt = tick;
		}
		if (!returnedAt || !fixture.committed[second].contains(returnedAt) || (row.gapMs >= 3000 && !heldAt)) {
			if (error) *error = "the committed bridge and automatic return boundaries were missing"; return false;
		}
		if (heldAt && (!fixture.committed[second].contains(heldAt) || !NetLockstepSelfTest::CheckSeatControllerState(false,
		    *fixture.peers[first], fixture.committed[first].at(heldAt), *fixture.peers[second], fixture.committed[second].at(heldAt), row.subject, error))) return false;
		if (!NetLockstepSelfTest::CheckSeatControllerState(true, *fixture.peers[first], fixture.committed[first].at(returnedAt),
		    *fixture.peers[second], fixture.committed[second].at(returnedAt), row.subject, error)) return false;
		DiagnosticLine() << "[net-lockstep-selftest] PASS combat_bridge case=" << arm << " stall_ms=" << row.gapMs << " F=" << heldAt << " G=" << returnedAt
		          << " keepalives=" << keepalives << " host_changes=0 images=" << fixture.imageReturns[row.subject - 1] << " survivor_undone=0" << std::endl;
		return true;
	}

	bool NetSessionPlaneSelfTest::CheckHostAdministration(unsigned arm, std::string* error) {
		std::call_once(s_FixtureObservers, [] {
			s_FrameMembers = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerFrameMembersAt(frame); };
			s_FrameAuthor = [](const NetLockstepCoordinator& peer, const NetLockstepReadyFrame& ready) -> uint8_t {
				const uint8_t local = peer.m_Config.localPeerId;
				const uint32_t members = peer.PeerFrameMembersAt(ready.frame);
				if (!peer.UsesPeerFrameGroups() || (members & (1U << (local - 1)))) return local;
				if (!peer.IsLocalPrivateReaderAt(ready.frame)) return 0;
				const auto witnesses = peer.m_PeerFrameWitnesses.find(ready.frame);
				if (witnesses == peer.m_PeerFrameWitnesses.end()) return 0;
				return WitnessedFrameAuthor(ready.frame, members, peer.CommittedFramePrefix(ready), peer.PeerFramePrefix(ready.frame), witnesses->second);
			};
			s_WaitState = [](const NetLockstepCoordinator& peer) {
				std::string starts;
				for (uint8_t remote: peer.m_RemotePeerIds) starts += " start=" + std::to_string(remote) + ":" + std::to_string(peer.m_RemoteStartsReceived.contains(remote)) +
				    "/required=" + std::to_string(peer.IsRemoteRequiredForFrame(remote, peer.m_Config.startFrame)) + "/held=" + std::to_string(peer.IsSeatUnderAI(remote, peer.m_Config.startFrame));
				return " next=" + std::to_string(peer.m_Stats.nextFrame) + " blocked=" + peer.m_AdvanceBlock +
				    " consumer=" + (peer.m_ConsumerWaitingFrame ? std::to_string(*peer.m_ConsumerWaitingFrame) : "none") +
				    " wait_since=" + std::to_string(peer.m_ConsumerWaitStartMs) + " members=" + std::to_string(peer.m_FrameGroupMembers) +
				    " local_held=" + std::to_string(peer.m_LocalSeatHeld) + " granted=" + (peer.m_GrantedSimulationTick ? std::to_string(*peer.m_GrantedSimulationTick) : "none") +
				    " hold_certificate=" + std::to_string(NetPeerFrameDetail::HasHoldDecision(peer.m_PeerBridgeCertificates, peer.m_Stats.nextFrame)) + starts;
			};
			s_HeldSeats = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerHeldSeatsAt(frame); };
			s_HoldBoundaries = [](const NetLockstepCoordinator& peer) -> const HoldBoundaries& { return peer.m_PeerFrameHolds; };
			s_OwnHoldPending = [](const NetLockstepCoordinator& peer, uint64_t frame) {
				for (const auto& [key, kept]: peer.m_PendingPeerDecisions)
					if (NetPeerFrameDetail::IsHoldDecision(key.second) && key.first >= frame && std::find(kept.first.members.begin(), kept.first.members.end(), peer.GetConfig().localPeerId) != kept.first.members.end()) return true;
				return false;
			};
			s_ReturnRoute = [](const NetLockstepCoordinator& peer, uint8_t seat) { return peer.PeerFrameRoute(seat); };
			s_ReturnTransport = [](const NetLockstepCoordinator& peer, uint8_t seat) {
				// FLAGGED fixture departure: a surviving mesh proposer can own a listener/dial route outside the old host star.
				return peer.PeerFrameRoute(seat).second;
			};
		});
		if (arm == 10) { std::string why; const bool passed = CheckPlacementCombatReturn(why); if (!passed && error) *error = why; return passed; }
		if (arm >= 5 && arm <= 9) {
			// Revised direct contract: both groups play and a hostless group (including one peer) migrates, then only the healed loser image-returns.
			Case split{"direct_migrated_split_heals"}; split.subject = arm == 5 ? 3 : arm == 9 ? 2 : 1;
			split.seats = arm == 6 ? 2 : 4; split.gapMs = 23000; split.tie = arm == 5;
			split.splitContract = split.directMigration = true; split.oneWaySubject = arm >= 8;
			std::string why;
			const bool passed = CheckSplitContract(split, why);
			if (!passed && error) *error = why;
			return passed;
		}
		Case row{"certified_admin_continuity"}; row.subject = 1; row.gapMs = 20000; row.hung = true;
		// The host returns after the votes, before their future activation frame.
		if (arm == 4) row.gapMs = 15100;
		row.seats = arm == 1 || arm == 3 ? 2 : 4; row.captureFrames = true;
		row.adminDirectory = row.internet = arm == 1; row.adminDirectoryDown = arm == 2;
		Fixture fixture; std::string why;
		if (!fixture.Run(row, why)) { if (error) *error = why; return false; }
		// Old arm 3 prohibited a lone direct joiner from migrating. A hung host is not a split: its survivor must migrate normally.

		for (size_t index = 0; index < fixture.peers.size(); ++index) {
			const auto& peer = *fixture.peers[index];
			const auto result = peer.GetMigrationResult();
			const uint64_t activation = AdminActivation(result);
			if (peer.GetHostPeerId() != 2 || result.generation != 1 || result.hostPeerId != 2 || !activation ||
			    (index != 0 && peer.MigrationUsesDirectory() != row.adminDirectory) ||
		    (index != 0 && arm != 4 && fixture.firstAdminChangeMs[index] >= fixture.hub.faultEnd) ||
		    (index != 0 && arm == 4 && fixture.firstAdminChangeMs[index] < fixture.hub.faultEnd)) {
				if (error) *error = "certified admin change did not finish on the survivors before the old host returned"; return false;
			}
			if (!fixture.committed[index].contains(activation)) { if (error) *error = "admin activation was not an ordinary displayed frame"; return false; }
			const auto& ready = fixture.committed[index].at(activation);
			size_t commands = 0;
			for (const auto* batch: {&ready.localCommands, &ready.remoteCommands}) for (const auto& command: *batch)
				if (const auto* authority = std::get_if<NetGameHostAuthority>(&command.payload)) {
					++commands;
					if (authority->formerPeerId != 1 || authority->peerId != 2 || authority->voters != ((1U << row.seats) - 2) ||
					    authority->members != authority->voters || authority->applyFrame != ready.frame) {
						if (error) *error = "admin activation did not carry every surviving member's certificate"; return false;
					}
				}
			if (commands != 1 || ready.authorityPeerId != 2 || ready.updateAuthorityPeerId != 2) {
				if (error) *error = "the activity and committed tick did not read the same administrator exactly once"; return false;
			}
		}
		if (row.adminDirectory && (fixture.hub.firstAdminRequestMs + 2 < fixture.hub.faultAt + 15000 ||
		    fixture.firstAdminChangeMs[1] < fixture.hub.faultAt + 16500)) {
			if (error) *error = "the directory verdict was bypassed before its forced release: request_ms=" + std::to_string(fixture.hub.firstAdminRequestMs) +
			    " activation_ms=" + std::to_string(fixture.firstAdminChangeMs[1]) + " fault_ms=" + std::to_string(fixture.hub.faultAt); return false;
		}
		DiagnosticLine() << "[net-lockstep-selftest] PASS certified_admin_continuity arm=" << arm
		          << " silence_ms=15000 directory_wait_ms=" << (row.adminDirectory ? 1500 : 0)
		          << " every_survivor=1 frames_moved=0 automatic_return=1" << std::endl;
		return true;
	}

	bool NetSessionPlaneSelfTest::CheckSceneLifecycle(const std::string& name, std::string* error) {
		std::call_once(s_FixtureObservers, [] {
			s_FrameMembers = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerFrameMembersAt(frame); };
			s_FrameAuthor = [](const NetLockstepCoordinator& peer, const NetLockstepReadyFrame& ready) -> uint8_t {
				const uint8_t local = peer.m_Config.localPeerId;
				const uint32_t members = peer.PeerFrameMembersAt(ready.frame);
				if (!peer.UsesPeerFrameGroups() || (members & (1U << (local - 1)))) return local;
				if (!peer.IsLocalPrivateReaderAt(ready.frame)) return 0;
				const auto witnesses = peer.m_PeerFrameWitnesses.find(ready.frame);
				if (witnesses == peer.m_PeerFrameWitnesses.end()) return 0;
				return WitnessedFrameAuthor(ready.frame, members, peer.CommittedFramePrefix(ready), peer.PeerFramePrefix(ready.frame), witnesses->second);
			};
			s_WaitState = [](const NetLockstepCoordinator& peer) {
				std::string starts;
				for (uint8_t remote: peer.m_RemotePeerIds) starts += " start=" + std::to_string(remote) + ":" + std::to_string(peer.m_RemoteStartsReceived.contains(remote)) +
				    "/required=" + std::to_string(peer.IsRemoteRequiredForFrame(remote, peer.m_Config.startFrame)) + "/held=" + std::to_string(peer.IsSeatUnderAI(remote, peer.m_Config.startFrame));
				return " next=" + std::to_string(peer.m_Stats.nextFrame) + " blocked=" + peer.m_AdvanceBlock +
				    " consumer=" + (peer.m_ConsumerWaitingFrame ? std::to_string(*peer.m_ConsumerWaitingFrame) : "none") +
				    " wait_since=" + std::to_string(peer.m_ConsumerWaitStartMs) + " members=" + std::to_string(peer.m_FrameGroupMembers) +
				    " local_held=" + std::to_string(peer.m_LocalSeatHeld) + " granted=" + (peer.m_GrantedSimulationTick ? std::to_string(*peer.m_GrantedSimulationTick) : "none") +
				    " hold_certificate=" + std::to_string(NetPeerFrameDetail::HasHoldDecision(peer.m_PeerBridgeCertificates, peer.m_Stats.nextFrame)) + starts;
			};
			s_HeldSeats = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerHeldSeatsAt(frame); };
			s_HoldBoundaries = [](const NetLockstepCoordinator& peer) -> const HoldBoundaries& { return peer.m_PeerFrameHolds; };
			s_OwnHoldPending = [](const NetLockstepCoordinator& peer, uint64_t frame) {
				for (const auto& [key, kept]: peer.m_PendingPeerDecisions)
					if (NetPeerFrameDetail::IsHoldDecision(key.second) && key.first >= frame && std::find(kept.first.members.begin(), kept.first.members.end(), peer.GetConfig().localPeerId) != kept.first.members.end()) return true;
				return false;
			};
			s_ReturnRoute = [](const NetLockstepCoordinator& peer, uint8_t seat) { return peer.PeerFrameRoute(seat); };
			s_ReturnTransport = [](const NetLockstepCoordinator& peer, uint8_t seat) {
				// FLAGGED fixture departure: a surviving mesh proposer can own a listener/dial route outside the old host star.
				return peer.PeerFrameRoute(seat).second;
			};
		});
		Hub hub;
		const uint8_t count = name == "rematch-chat" || name == "rematch-redial" || name == "kick-received-two" || name == "heavy-input" || name == "leave-rejoin-two" || name == "drop-after-input" || name == "drop-before-host-input" || name == "silent-before-host-input" || name == "referee-input-runway" ? 2 : 3;
		hub.row = Case{"scene_lifecycle"}; hub.row.seats = count; hub.row.rttMs = hub.row.otherRttMs = 100;
		auto ip = std::make_unique<Wire>(hub, 1);
		auto relay = std::make_unique<Wire>(hub, 1);
		NetMuxTransport hostWire(std::move(ip), std::move(relay));
		GnsP2PConfig relayConfig; relayConfig.connectionMode = 2;
		hostWire.SetHostP2P(3, relayConfig);
		Wire clientWire(hub, 2), otherWire(hub, 3), returningWire(hub, 2);
		auto ownedHostSession = std::make_unique<NetSession>();
		auto& hostSession = *ownedHostSession;
		NetSession clientSession, otherSession, returningSession;
		NetSessionConfig sessionConfig;
		sessionConfig.port = 47919; sessionConfig.sessionId = 0x5343454E45ULL; sessionConfig.maxPeers = count - 1;
		sessionConfig.displayName = "Captain"; sessionConfig.timeoutMs = 30000; sessionConfig.heartbeatIntervalMs = 100;
		auto& identity = sessionConfig.localIdentity;
		identity.gameVersion = "7.0.0-test"; identity.networkProtocolVersion = NetProtocol::c_Version;
		identity.controllerFrameVersion = ControllerFrame::c_Version; identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
		identity.buildId = "scene-session-rows"; identity.platform = "test";
		auto joinConfig = sessionConfig; joinConfig.displayName = "Joiner"; ++joinConfig.localNonce;
		if (!hostSession.StartHost(hostWire, sessionConfig, error) || !clientSession.StartClient(clientWire, "fixture", joinConfig, error)) return false;
		if (count == 3) { auto config = joinConfig; config.displayName = "Other joiner"; config.localNonce += 2;
			if (!otherSession.StartClient(otherWire, "fixture", config, error)) return false;
		}
		for (; hub.now < 2000 && (hostSession.GetReadyPeerCount() != count - 1 || !clientSession.IsReady() || (count == 3 && !otherSession.IsReady())); ++hub.now) {
			hostSession.Tick(hub.now); clientSession.Tick(hub.now);
			if (count == 3) otherSession.Tick(hub.now);
		}
		if (!clientSession.IsReady() || hostSession.GetReadyPeerCount() != count - 1 || (count == 3 && !otherSession.IsReady())) { *error = "the scene row did not seat every player"; return false; }
		const NetPeerId originalRoute = hostSession.GetReadyPeers().front().transportPeerId;
		auto match = NetMatchConfigUtil::MakeDefault(sessionConfig.sessionId);
		match.peerCount = count; match.roundId = 77; match.players = {{1, 0, false, "Captain"}, {2, 1, false, "Joiner"}};
		if (count == 3) match.players.push_back({3, 1, false, "Other joiner"});
		if (name == "capsule-retry" || name == "paired-image-service") {
			match.successorOrder = {2, 3};
			match.migrationPeers = {{1, 47901, {"127.0.0.1"}}, {2, 47902, {"127.0.0.1"}}, {3, 47903, {"127.0.0.1"}}};
		}
		const uint16_t delay = count == 3 ? 24 : 12;
		match.inputDelayFrames = delay; match.peerInputDelayFrames.assign(count, delay);
		hub.directory.configHash = NetMatchConfigUtil::HashConfig(match);
		std::vector<std::unique_ptr<NetLockstepCoordinator>> rounds(count);
		std::vector<NetLockstepConfig> roundConfigs(count);
		std::vector<std::shared_ptr<NetPeerSessionLinks>> links(count);
		std::vector<std::unique_ptr<NetPeerSessionWire>> sessionWires(count);
		std::array<NetSession*, 3> sessions{&hostSession, &clientSession, &otherSession};
		std::array<INetTransport*, 3> nativeWires{&hostWire, &clientWire, &otherWire};
		std::vector<uint64_t> produced(count, 1), applied(count), displayedAt(count);
		std::vector<double> credits(count);
		std::vector<std::map<uint64_t, uint64_t>> refereeRequests(count);
		for (size_t index = 0; index < count; ++index) {
			NetLockstepConfig config; config.sessionId = match.sessionId; config.matchConfig = match;
			config.localPeerId = static_cast<uint8_t>(index + 1); config.peerCount = count; config.roundId = index == 0 ? 77 : 0;
			config.startFrame = 1; config.inputDelayFrames = delay; config.peerInputDelayFrames = {{1, delay}, {2, delay}};
			config.peerIncarnations = {{1, 1}, {2, 1}}; config.timeoutMs = 30000;
			if (count == 3) { config.peerInputDelayFrames[3] = delay; config.peerIncarnations[3] = 1; }
			config.peerFrameGroups = config.substituteSlowPeers = true; config.simTickMs = c_TestTickMs;
			config.slowPlayerBoundTicks = 3; config.frameLane = NetTransportLane::InputUnreliable; config.relayToOtherPeers = index == 0;
			// An internet match's referee that has not decided a tie answers Waiting.
			if (name == "leave-rejoin-two") config.frameTieReferee = [](const NetFrameTieRequest&) { NetFrameTieReply reply; reply.state = NetFrameTieReply::State::Waiting; return reply; };
			// The directory, through each seat's own client: the first half to check in keeps a split, answered after the exchange.
			if (name == "drop-after-input" || name == "drop-before-host-input" || name == "silent-before-host-input" || name == "referee-input-runway") {
				if (hub.directory.retained.empty()) for (uint16_t seat = 1; seat <= count; ++seat) hub.directory.retained.push_back(seat);
				const uint8_t seat = static_cast<uint8_t>(index + 1);
				hub.directoryClients[seat].StartRound(hub.now, (seat * 1700ULL) % 5100);
				config.frameTieReferee = [&, index, seat](const NetFrameTieRequest& request) {
					refereeRequests[index].try_emplace(request.frame, hub.now);
					return hub.directoryClients[seat].Query(request, hub.now);
				};
			}
			config.migrationKey.fill(0x39); config.peerSessionLinks = links[index] = std::make_shared<NetPeerSessionLinks>();
			links[index]->sessionClock = [&] { return hub.now; };
			if (index == 0) for (const auto& peer: hostSession.GetReadyPeers()) config.remoteTransportPeerIds[peer.assignedPeerId + 1] = peer.transportPeerId;
			else config.remoteTransportPeerIds[1] = sessions[index]->GetRemoteTransportPeerId();
			rounds[index] = std::make_unique<NetLockstepCoordinator>();
			if (!rounds[index]->Start(*nativeWires[index], config, error)) return false;
			roundConfigs[index] = config;
			rounds[index]->DeferStopsToTickBoundary();
			rounds[index]->SetSessionEventSink([&, index](const NetTransportEvent& event) { sessions[index]->InjectEvent(event, hub.now); });
		}
		// A host carrying many AI actors commits a few kilobytes of input every tick that do not compress.
		const bool heavy = name == "heavy-input";
		const auto HeavyInput = [](uint64_t frame) {
			std::vector<ControllerFrame> frames(40);
			uint64_t state = frame * 0x9E3779B97F4A7C15ULL;
			const auto next = [&] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
			for (size_t index = 0; index < frames.size(); ++index) {
				auto& actor = frames[index];
				actor.actorUniqueID = static_cast<int64_t>(index + 1);
				if constexpr (ControlState::CONTROLSTATECOUNT >= 64) actor.stateMask = next();
				else actor.stateMask = next() & ((uint64_t{1} << ControlState::CONTROLSTATECOUNT) - 1);
				actor.analogMoveX = static_cast<int16_t>(next()); actor.analogMoveY = static_cast<int16_t>(next());
				actor.analogAimX = static_cast<int16_t>(next()); actor.analogAimY = static_cast<int16_t>(next());
				actor.aimAngle = static_cast<float>(next() % 6283) / 1000.0F;
				actor.viewPointX = static_cast<float>(next() % 100000) / 10.0F; actor.viewPointY = static_cast<float>(next() % 100000) / 10.0F;
			}
			return frames;
		};
		bool hostWithholds = false;
		const auto step = [&](bool joinerPlaying) {
			hub.directory.refusing = hub.tieAnswersHeld;
			for (size_t index = 0; index < count; ++index) {
				if (!joinerPlaying && index == 1) continue;
				hub.PollDirectory(static_cast<uint8_t>(index + 1));
				auto& round = *rounds[index]; round.Tick(hub.now);
				sessions[index]->Tick(hub.now, false);
				if (!round.IsRunning()) continue;
				credits[index] = std::min(6.0, credits[index] + 1 / c_TestTickMs);
				while (credits[index] >= 1) {
					const uint64_t next = applied[index] + 1;
					if (produced[index] == next) {
						if (hostWithholds && index == 0) break;
						if (!round.QueueLocalInput(next, heavy && index == 0 ? HeavyInput(next) : std::vector<ControllerFrame>{}, {}, error)) return false;
						++produced[index];
					}
					NetLockstepReadyFrame ready;
					if (next < round.GetStats().effectiveStartFrame) ready.frame = next;
					else if (!round.PopReadyFrame(ready)) { round.NoteFrameWait(next, hub.now); break; }
					if (ready.frame != next) { *error = "the scene consumer skipped a frame"; return false; }
					applied[index] = ready.frame; displayedAt[index] = hub.now;
					round.FinishFrameWait(hub.now); round.FinishSimulationTick(ready.frame); credits[index] -= 1;
				}
			}
			++hub.now; return true;
		};
		for (; hub.now < 5000 && *std::min_element(applied.begin(), applied.end()) < 150;) if (!step(true)) return false;
		if (*std::min_element(applied.begin(), applied.end()) < 150) { *error = "the scene row did not reach live play"; return false; }
		for (const auto& round: rounds) if (!round->m_MigrationHistory.contains(round->GetStats().nextFrame - 1)) {
			*error = "the scene row did not retain its last complete input frame"; return false;
		}
		if (count == 3) for (const auto& round: rounds) for (const auto& [peer, stats]: round->GetStats().peers) if (stats.holds != 0) {
			*error = "the returning-seat row held a player before the Leave gesture"; return false;
		}
		for (size_t index = 0; index < count; ++index) {
			sessionWires[index] = std::make_unique<NetPeerSessionWire>(links[index]);
			links[index]->sessionAttached = true; sessions[index]->RebindSessionWire(*sessionWires[index]);
		}
		if (name == "rematch-redial") {
			NetSeatAuthRegistry registry;
			if (!registry.BeginHostedSession()) { *error = "the rematch registry could not start"; return false; }
			NetH4Identity authIdentity; authIdentity.gameVersion = identity.gameVersion; authIdentity.buildId = identity.buildId;
			authIdentity.controllerFrameVersion = ControllerFrame::c_Version; authIdentity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			NetReconnectHost admission; admission.Configure(&registry, match.sessionId, authIdentity);
			admission.SetSeatTable(NetH4BuildSeatTable(match), match.mode); admission.SetHostAddress("fixture");
			admission.SetMatchConfigHash(NetMatchConfigUtil::HashConfig(match));
			uint64_t unixNow = 1700000000000ULL;
			const auto clock = [](void* context) { return *static_cast<uint64_t*>(context); };
			admission.SetUnixClock(clock, &unixNow);
			NetReconnectTicketStore store; store.SetPath("Userdata/hold-row-rematch-redial.json");
			NetReconnectClient original; original.Configure(&store, authIdentity, "Joiner"); original.SetUnixClock(clock, &unixNow);
			original.SetHostContext("fixture", NetMatchConfigUtil::HashConfig(match));
			if (!original.BeginAdmission(hub.now, error)) return false;
			for (unsigned pump = 0; pump < 20; ++pump, ++hub.now) {
				for (const auto& packet: original.TakeOutbound()) admission.HandleMessage(originalRoute, packet.payload, hub.now);
				admission.Tick(hub.now);
				for (const auto& packet: admission.TakeOutbound()) if (packet.connection == originalRoute) original.HandleMessage(packet.payload, hub.now);
				original.Tick(hub.now);
			}
			if (!original.IsAdmitted() || original.GetAssignedPeerId() + 1 != 2) { *error = "the first round did not issue the player's seat ticket"; return false; }
			const auto ticket = original.GetRecord(); const auto incarnation = original.GetIncarnation();
			admission.SetLiveMatch(true); hostSession.SetReconnectHost(&admission);
			struct DetachAdmission { NetSession& host; NetSession& client; ~DetachAdmission() { host.SetReconnectHost(nullptr); client.SetReconnectClient(nullptr); } } detach{hostSession, returningSession};
			rounds[0]->Complete("round ended");
			for (const uint64_t until = hub.now + 300; hub.now < until; ++hub.now) for (auto& round: rounds) round->Tick(hub.now);
			for (auto& round: rounds) round.reset();
			admission.SetMatchEnded();
			NetMatchConfig rematch;
			if (!NetMatchConfigUtil::DeriveRematchConfig(match, rematch, error)) return false;
			admission.SetSeatTable(NetH4BuildSeatTable(rematch), rematch.mode); admission.SetMatchConfigHash(NetMatchConfigUtil::HashConfig(rematch));
			clientSession.Close("re-dialling the rematch lobby");
			for (const uint64_t until = hub.now + 1200; hub.now < until; ++hub.now) { hostSession.Tick(hub.now); clientSession.Tick(hub.now); }
			NetReconnectClient returning; returning.Configure(&store, authIdentity, "Joiner"); returning.SetUnixClock(clock, &unixNow);
			returning.SetHostContext("fixture", NetMatchConfigUtil::HashConfig(rematch)); returning.SetRequireStoredTicket(true);
			returningSession.SetReconnectClient(&returning);
			auto redial = joinConfig; redial.localNonce += 4;
			if (!returningSession.StartClient(returningWire, "fixture", redial, error)) return false;
			for (const uint64_t until = hub.now + 3000; hub.now < until && (!returningSession.IsReady() || hostSession.GetReadyPeerCount() != 1); ++hub.now) {
				hostSession.Tick(hub.now); returningSession.Tick(hub.now);
			}
			if (!returningSession.IsReady() || !returning.UsedStoredTicket() || returning.GetRecord().stableSeat != ticket.stableSeat ||
			    returning.GetRecord().epoch != ticket.epoch || returning.GetIncarnation() <= incarnation || returningSession.GetLocalPeerId() != 1) {
				*error = "the stored seat ticket did not re-dial the rematch lobby: " + returningSession.BuildRejectText(); return false;
			}
			std::array<NetLobbySession, 2> lobbies;
			for (size_t index = 0; index < 2; ++index) {
				NetLobbySessionConfig config; config.host = index == 0; config.localPeerId = static_cast<uint8_t>(index + 1);
				config.matchConfig = rematch; config.startFrame = 1; config.autoReady = config.autoStart = false;
				config.session = index == 0 ? &hostSession : &returningSession; config.sessionNowMs = [&] { return hub.now; };
				config.remoteTransportPeerIds[index == 0 ? 2 : 1] = index == 0 ? hostSession.GetReadyPeers().front().transportPeerId : returningSession.GetRemoteTransportPeerId();
				if (!lobbies[index].Start(index == 0 ? static_cast<INetTransport&>(*sessionWires[0]) : static_cast<INetTransport&>(returningWire), config, error)) return false;
			}
			for (const uint64_t until = hub.now + 2000; hub.now < until; ++hub.now) for (auto& lobby: lobbies) lobby.Tick(hub.now);
			if (!lobbies[0].IsConfigAcked(2) || lobbies[1].GetMatchConfigHash() != NetMatchConfigUtil::HashConfig(rematch)) {
				*error = "the re-dialled seat did not acknowledge the next round's config"; return false;
			}
			return true;
		}
		if (name == "capsule-retry" || name == "paired-image-service") {
			for (const uint64_t until = hub.now + 3000; hub.now < until;) if (!step(false)) return false;
			if (!rounds[0]->HasHeldAISeat(2)) { *error = "the returning successor was never held"; return false; }
			// This is the service's real lobby and sealing path on the already admitted connection.
			// The registry issues the successor credential; no certificate or peer decision is fabricated.
			if (!ActivityMan::IsConstructed()) ActivityMan::Construct();
			std::unique_ptr<Activity> savedActivity = std::make_unique<GameActivity>();
			g_ActivityMan.SwapCheckpointActivity(savedActivity);
			struct RestoreActivity { std::unique_ptr<Activity>& saved; ~RestoreActivity() { g_ActivityMan.SwapCheckpointActivity(saved); } } restoreActivity{savedActivity};
			struct RestorePath { std::string& slot; std::string old; ~RestorePath() { slot = std::move(old); } } restorePath{NetMatchService::s_TicketStorePath, NetMatchService::s_TicketStorePath};
			const std::string ticketStem = "Userdata/hold-row-" + name + "-successor";
			NetMatchService::s_TicketStorePath = ticketStem + "-host.json";
			NetMatchService service; service.m_IsHost = true; service.m_LocalPeerId = 1; service.m_MatchConfig = match;
			service.m_MigratedTransport = std::move(sessionWires[0]); service.m_Session = std::move(ownedHostSession);
			service.m_Coordinator = std::move(rounds[0]); service.m_Runner = std::make_unique<NetMatchRunner>();
			service.m_MigrationKey.fill(0x39);
			if (!service.m_SeatAuth.BeginHostedSession()) { *error = "the successor registry could not draw its epoch"; return false; }
			NetH4Identity identity; identity.gameVersion = "7.0.0-test"; identity.buildId = "scene-session-rows";
			identity.controllerFrameVersion = ControllerFrame::c_Version; identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			service.m_ReconnectHost.Configure(&service.m_SeatAuth, match.sessionId, identity);
			service.m_ReconnectHost.SetSeatTable(NetH4BuildSeatTable(match), match.mode);
			service.m_ReconnectHost.SetHostAddress("127.0.0.1"); service.m_ReconnectHost.SetMatchConfigHash(NetMatchConfigUtil::HashConfig(match));
			NetReconnectTicketStore clientStore, otherStore;
			clientStore.SetPath(ticketStem + "-client.json"); otherStore.SetPath(ticketStem + "-other.json");
			NetReconnectClient successor, otherSuccessor;
			successor.Configure(&clientStore, identity, "Joiner"); otherSuccessor.Configure(&otherStore, identity, "Other joiner");
			for (auto* admitted: {&successor, &otherSuccessor}) {
				const uint8_t peer = admitted == &successor ? 2 : 3;
				const auto ready = hostSession.GetReadyPeers();
				const auto binding = std::find_if(ready.begin(), ready.end(), [peer](const auto& entry) { return entry.assignedPeerId + 1 == peer; });
				if (binding == ready.end()) { *error = "the admitted successor has no session route"; return false; }
				const NetPeerId connection = binding->transportPeerId;
				admitted->SetHostContext("127.0.0.1", NetMatchConfigUtil::HashConfig(match));
				if (!admitted->BeginAdmission(hub.now, error)) return false;
				for (unsigned pump = 0; pump < 20; ++pump, ++hub.now) {
					for (const auto& packet: admitted->TakeOutbound()) service.m_ReconnectHost.HandleMessage(connection, packet.payload, hub.now);
					service.m_ReconnectHost.Tick(hub.now);
					for (const auto& packet: service.m_ReconnectHost.TakeOutbound()) if (packet.connection == connection) admitted->HandleMessage(packet.payload, hub.now);
					admitted->Tick(hub.now);
				}
				if (!admitted->IsAdmitted() || admitted->GetAssignedPeerId() + 1 != peer) { *error = "the successor did not receive its actual admitted credential"; return false; }
			}
			NetLobbySession clientLobby;
			NetLobbySessionConfig hostConfig; hostConfig.host = true; hostConfig.localPeerId = 1; hostConfig.matchConfig = match;
			hostConfig.session = service.m_Session.get(); hostConfig.startFrame = 1; hostConfig.autoReady = hostConfig.autoStart = false;
			hostConfig.enableMigration = true; hostConfig.sessionNowMs = [&] { return hub.now; };
			hostConfig.sealMigration = [&](uint8_t peer, const NetHash32& hash, std::vector<uint8_t>& bytes) { return service.SealMigrationCapsuleLocked(peer, hash, bytes); };
			for (const auto& peer: hostSession.GetReadyPeers()) hostConfig.remoteTransportPeerIds[peer.assignedPeerId + 1] = peer.transportPeerId;
			auto clientConfig = hostConfig; clientConfig.host = false; clientConfig.localPeerId = 2; clientConfig.session = &clientSession;
			clientConfig.remoteTransportPeerIds = {{1, clientSession.GetRemoteTransportPeerId()}};
			bool opened = false;
			clientConfig.openMigration = [&](const NetLobbyMigration& capsule) {
				std::vector<uint8_t> context(capsule.configHash.begin(), capsule.configHash.end()), plaintext; context.push_back(capsule.peerId);
				opened = successor.OpenSuccessorCapsule(context, capsule.sealedState, plaintext) && !plaintext.empty(); return opened;
			};
			service.m_HostThisNetworkOnly = false; service.m_DirectoryRow.resumeSessionId = "fixture-unregistered-row"; service.m_DirectoryRegistered = false;
			if (!service.m_Runner->GetLobbySession().Start(*service.ActiveWireLocked(), hostConfig, error) || !clientLobby.Start(*sessionWires[1], clientConfig, error)) return false;
			if (!service.m_WorldJoin.ConfigureMatchRejoins(match, 77, c_TestTickMs, error) ||
			    !service.m_WorldJoin.BeginRejoin(originalRoute, 1, 2, 2, "Joiner", hub.now, error)) return false;
			const auto pumpRejoin = [&](bool tickHostLobby) {
				if (name == "paired-image-service") {
					rounds[0] = std::move(service.m_Coordinator);
					const bool advanced = step(true);
					service.m_Coordinator = std::move(rounds[0]);
					if (!advanced) return false;
					--hub.now; // The caller advances the clock, as it does for the capsule-only pump.
				} else {
					service.m_Coordinator->Tick(hub.now);
					for (size_t index = 1; index < count; ++index) rounds[index]->Tick(hub.now);
				}
				clientLobby.Tick(hub.now); clientSession.Tick(hub.now, false); otherSession.Tick(hub.now, false);
				if (tickHostLobby) service.m_Runner->GetLobbySession().Tick(hub.now);
				hostSession.Tick(hub.now, false); service.DrivePrivateMatchRejoins(hub.now);
				return true;
			};
			for (const uint64_t until = hub.now + 1200; hub.now < until; ++hub.now) {
				if (!pumpRejoin(true)) return false;
			}
			const auto* pending = service.m_WorldJoin.FindSession(originalRoute);
			if (!pending || !pending->matchConfigSent || !service.m_SuccessorCapsulesOwed.contains(originalRoute) || opened) {
				*error = "a failed seal was forgotten after the config was sent"; return false;
			}
			service.m_DirectoryRegistered = true;
			// The lobby's normal config resend is not ticked here: the live service must retry the capsule itself.
			for (const uint64_t until = hub.now + 1200; hub.now < until && !opened; ++hub.now) {
				if (!pumpRejoin(false)) return false;
			}
			if (!opened || service.m_SuccessorCapsulesOwed.contains(originalRoute)) { *error = "the live rejoin service did not retry and deliver its successor capsule"; return false; }
			if (name == "paired-image-service") {
				const auto ready = hostSession.GetReadyPeers();
				const auto other = std::find_if(ready.begin(), ready.end(), [](const auto& peer) { return peer.assignedPeerId + 1 == 3; });
				if (other == ready.end()) { *error = "the second image returner lost its admitted route"; return false; }
				const NetPeerId otherRoute = other->transportPeerId;
				if (!service.m_WorldJoin.BeginRejoin(otherRoute, 2, 3, 2, "Other joiner", hub.now, error)) return false;
				NetWorldCheckpointImage image; image.privateSessionId = match.sessionId; image.round = 77; image.tick = applied[0] - 80;
				image.bytes = 8; image.checkpointConfig = "config"; image.sideState = "state"; image.digest = "fixture-image";
				service.m_WorldJoin.PublishImage(image);
				// The fixture's published base has the capture time the real image producer records with it.
				service.m_PrivateImageTakenMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyNow().time_since_epoch()).count());
				if (!service.m_WorldJoin.NoteTransferComplete(otherRoute, image.bytes, error)) return false;
				service.m_WorldJoin.NoteRejoinLinkFit(otherRoute, true); service.m_WorldJoin.NoteTailLinkRtt(otherRoute, 100);
				LoopbackTransport privateWire; NetLockstepCoordinator privateReader;
				auto privateConfig = roundConfigs[2]; privateConfig.roundId = 77; privateConfig.startFrame = image.tick + 1;
				privateConfig.peerFrameGroups = false; privateConfig.peerSessionLinks.reset();
				if (!privateReader.StartReplay(privateWire, privateConfig, error)) return false;
				const auto replayCommitted = [&] {
					for (uint64_t frame = privateReader.GetStats().nextFrame; frame < service.m_Coordinator->GetStats().nextFrame; ++frame) {
						const auto kept = service.m_Coordinator->m_MigrationHistory.find(frame);
						NetLockstepReadyFrame ready;
						if (kept == service.m_Coordinator->m_MigrationHistory.end() || !service.m_Coordinator->DecodeMigrationFrame(kept->second, frame, ready)) return false;
						const auto recorded = PackWorldJoinReadyFrame(ready);
						if (!privateReader.QueueReplayFrame(frame, recorded.frames, recorded.commands, error, recorded.observations, recorded.valueObservations,
						                                   recorded.senderPeerId, recorded.replayUpdateAuthorityPeerId)) return false;
						privateReader.Tick(hub.now);
						if (!privateReader.PopReadyFrame(ready)) return false;
						privateReader.FinishSimulationTick(ready.frame);
					}
					return true;
				};
				if (!replayCommitted()) { *error = "the other image reader could not consume the real committed tail"; return false; }
				uint64_t activation = 0;
				if (!service.m_WorldJoin.NoteCatchUpProgress(otherRoute, privateReader.GetStats().nextFrame - 1, 60, 1000, service.m_Coordinator->GetStats().nextFrame, &activation, error) || !activation) {
					*error = "the second image return did not name its activation"; return false;
				}
				// Seat 3 still plays in the agreed round, so this activation must be refused. The first returner waits for its archive
				// until that refusal has happened: a host-wide error here would prevent its later transfer.
				std::string expectedRefusal;
				if (service.m_Coordinator->SchedulePeerReclaim(3, otherRoute, 2, activation, &expectedRefusal) || expectedRefusal.empty()) {
					*error = "the round admitted an image return of a seat that was still playing"; return false;
				}
				const auto refused = [&] {
					const auto held = service.m_PrivateTransferHeldReasons.find(otherRoute);
					return held != service.m_PrivateTransferHeldReasons.end() && held->second == expectedRefusal;
				};
				for (const uint64_t until = hub.now + 2400; hub.now < until && !refused();) {
					rounds[0] = std::move(service.m_Coordinator);
					const bool advanced = step(false);
					service.m_Coordinator = std::move(rounds[0]);
					if (!advanced) return false;
					clientLobby.Tick(hub.now); service.m_Runner->GetLobbySession().Tick(hub.now);
					if (!replayCommitted() || !service.m_WorldJoin.NoteCatchUpProgress(otherRoute, privateReader.GetStats().nextFrame - 1, 1, 17,
					                                                              service.m_Coordinator->GetStats().nextFrame, nullptr, error)) return false;
					service.DrivePrivateMatchRejoins(hub.now);
				}
				if (!refused() || service.m_Coordinator->HasAgreedSeatReclaim(3)) {
					*error = "the already-playing seat's image activation was not refused"; return false;
				}
				if (!service.m_PrivateJoinError.empty()) { *error = "one refused activation left a host-wide image error: " + service.m_PrivateJoinError; return false; }
				service.m_WorldJoinImageArchive = std::make_shared<const std::vector<uint8_t>>(image.bytes, 0x39);
				service.m_WorldJoinImageDigest = image.digest;
				bool transferred = false;
				for (const uint64_t until = hub.now + 2000; hub.now < until && !transferred; ++hub.now) {
					if (!pumpRejoin(true)) return false;
					if (const auto* first = service.m_WorldJoin.FindSession(originalRoute)) transferred = first->transferStarted;
				}
				if (!transferred) { *error = "the first image did not leave after the other returner's activation was refused"; return false; }
			}
			return true;
		}
		if (name == "referee-input-runway") {
			for (const uint64_t until = hub.now + 10000; hub.now < until;) if (!step(true)) return false;
			const size_t healthyQueries = refereeRequests[0].size() + refereeRequests[1].size();
			DiagnosticLine() << "[net-session-scene] referee-input-runway healthy_queries=" << healthyQueries
			          << " host_frame=" << applied[0] << " joiner_frame=" << applied[1] << std::endl;
			if (healthyQueries != 0) { *error = "healthy buffered input spent the directory's recovery request allowance"; return false; }
			clientWire.Stop();
			auto& host = *rounds[0];
			const uint64_t begin = hub.now, before = applied[0];
			uint64_t longestWait = 0;
			for (; hub.now < begin + 3000;) {
				if (!step(false)) return false;
				if (host.m_ConsumerWaitingFrame && hub.now >= host.m_ConsumerWaitStartMs)
					longestWait = std::max(longestWait, hub.now - host.m_ConsumerWaitStartMs);
			}
			const bool held = host.m_PeerBridges.contains(2) && host.m_PeerBridges.at(2).committedHold;
			DiagnosticLine() << "[net-session-scene] referee-input-runway drop_queries=" << refereeRequests[0].size()
			          << " before=" << before << " after=" << applied[0] << " longest_wait_ms=" << longestWait << " held=" << held << std::endl;
			if (longestWait > 50 || hub.now - displayedAt[0] > 50 || applied[0] < before + 110 || !held) {
				*error = "authority ruling: a dropped peer's local hold exceeded the 50 ms survivor bound"; return false;
			}
			return true;
		}
		if (name == "kick-pending") {
			auto& host = *rounds[0]; const uint64_t boundary = host.GetStats().nextFrame;
			// The removed player has receipted our next input, while its own next input is still pending.
			host.m_PeerInputReceipts[1][2] = boundary + 1;
			host.m_RemoteFrames[boundary].erase(2); host.m_PeerSourceInputs[boundary].erase(2);
			host.EvictRemovedPeer(2, "removed from the session", hub.now);
			hostSession.DisconnectReadyPeer(originalRoute, NetRejectReason::ParticipantRemoved, c_NetRemovedLinkText);
			const uint64_t begin = hub.now, before = applied[0], repairs = host.m_PeerReceiptRepairs;
			const uint64_t otherBefore = applied[2], otherRepairs = rounds[2]->m_PeerReceiptRepairs;
			uint64_t firstTwoAt = 0, greatestGap = 0;
			uint64_t otherFirstTwoAt = 0, otherGap = 0;
			for (; hub.now < begin + 2000;) {
				const uint64_t previous = displayedAt[0];
				const uint64_t otherPrevious = displayedAt[2];
				if (!step(false)) return false;
				rounds[1]->Tick(hub.now); clientSession.Tick(hub.now, false);
				if (displayedAt[0] != previous) greatestGap = std::max(greatestGap, displayedAt[0] - previous);
				if (!firstTwoAt && applied[0] >= before + 2) firstTwoAt = hub.now - begin;
				if (displayedAt[2] != otherPrevious) otherGap = std::max(otherGap, displayedAt[2] - otherPrevious);
				if (!otherFirstTwoAt && applied[2] >= otherBefore + 2) otherFirstTwoAt = hub.now - begin;
			}
			const bool named = clientSession.HasReject() && clientSession.GetRejectReason() == NetRejectReason::ParticipantRemoved;
			const bool released = host.SeatReleases().contains(2);
			const uint64_t finalWait = hub.now - displayedAt[0];
			DiagnosticLine() << "[net-session-scene] kick-pending before=" << before << " after=" << applied[0] << " two_updates_ms=" << firstTwoAt
			          << " greatest_gap_ms=" << greatestGap << " final_wait_ms=" << finalWait << " removed_repairs=" << host.m_PeerReceiptRepairs - repairs << " released=" << released << " named=" << named << std::endl;
			DiagnosticLine() << "[net-session-scene] kick-survivor before=" << otherBefore << " after=" << applied[2]
			          << " two_updates_ms=" << otherFirstTwoAt << " greatest_gap_ms=" << otherGap << " final_wait_ms=" << hub.now - displayedAt[2]
			          << " released=" << rounds[2]->SeatReleases().contains(2) << " removed_repairs=" << rounds[2]->m_PeerReceiptRepairs - otherRepairs << std::endl;
			if (!firstTwoAt || firstTwoAt > 50 || greatestGap > 50 || finalWait > 50 || applied[0] < before + 110 ||
			    !released || !named || host.m_PeerReceiptRepairs != repairs) {
				*error = "a kick kept the host waiting on the removed seat or failed to tell that player why"; return false;
			}
			if (!otherFirstTwoAt || otherFirstTwoAt > 50 || otherGap > 50 || hub.now - displayedAt[2] > 50 ||
			    applied[2] < otherBefore + 110 || !rounds[2]->SeatReleases().contains(2) || rounds[2]->m_PeerReceiptRepairs != otherRepairs) {
				*error = "a kick stopped the other survivor or kept repairing the removed seat"; return false;
			}
			return true;
		}
		if (name == "heavy-input") {
			hub.countInputs = true;
			const uint64_t begin = hub.now, before = applied[0];
			for (; hub.now < begin + 3000;) if (!step(true)) return false;
			hub.countInputs = false;
			unsigned worst = 0, repeated = 0, ticks = 0;
			for (const auto& [frame, times]: hub.reliableInputTicks) {
				if (frame <= before + 30 || frame + 30 > applied[0]) continue;
				++ticks; worst = std::max(worst, times); if (times > 1) ++repeated;
			}
			const uint64_t played = applied[0] - before;
			DiagnosticLine() << "[net-session-scene] heavy-input played=" << played << " reliable_ticks=" << hub.reliableInputTicks.size() << " judged=" << ticks
			          << " worst_reliable_sends=" << worst << " ticks_sent_twice=" << repeated
			          << " reliable_bytes_per_tick=" << (played ? hub.reliableInputBytes / played : 0) << " unreliable_bytes_per_tick=" << (played ? hub.unreliableInputBytes / played : 0) << std::endl;
			if (played < 170 || worst > 1) { *error = "the host sent a committed input tick more than once on the reliable lane with no packet lost"; return false; }
			return true;
		}
		if (name == "drop-after-input") {
			auto& host = *rounds[0];
			// No tie stands decided, as in a match whose earlier ties the directory refused.
			host.m_PeerFrameTieWinners.clear();
			// The joiner's last receipts of our input are lost while its own inputs keep arriving; then its link closes.
			hub.dropReceiptsFrom = 2;
			for (const uint64_t until = hub.now + 300; hub.now < until;) if (!step(true)) return false;
			clientWire.Stop();
			hub.dropReceiptsFrom = 0;
			const uint64_t begin = hub.now, before = applied[0];
			uint64_t longestWait = 0;
			for (; hub.now < begin + 3000;) {
				if (!step(false)) return false;
				if (host.m_ConsumerWaitingFrame && hub.now >= host.m_ConsumerWaitStartMs) longestWait = std::max(longestWait, hub.now - host.m_ConsumerWaitStartMs);
			}
			const bool held = host.m_PeerBridges.contains(2) && host.m_PeerBridges.at(2).committedHold;
			DiagnosticLine() << "[net-session-scene] drop-after-input before=" << before << " after=" << applied[0] << " longest_wait_ms=" << longestWait
			          << " final_wait_ms=" << hub.now - displayedAt[0] << " held=" << held << " next=" << host.GetStats().nextFrame << std::endl;
			// Authority ruling: this is a dropped peer, not a split. The survivor holds it inside 50 ms without directory permission.
			if (longestWait > 50 || hub.now - displayedAt[0] > 50 || applied[0] < before + 110 || !held) {
				*error = "authority ruling: a joiner dropped after its last input made the host wait past its 50 ms local hold bound"; return false;
			}
			return true;
		}
		if (name == "silent-before-host-input") {
			auto& host = *rounds[0];
			host.m_PeerFrameTieWinners.clear();
			hub.tieAnswersHeld = true;
			// As a relay that stops forwarding: the host's inputs stop reaching the joiner first, so the joiner's inputs, an input delay
			// ahead, are in hand while its receipt of the host's own input never comes; then the joiner falls silent. No close reaches the
			// host, so the joiner keeps a route the whole time.
			hub.dropInputsFrom = 1;
			const auto ownWait = [&] {
				const uint64_t next = host.GetStats().nextFrame;
				return !host.LocalInputAccepted(next) && host.m_RemoteFrames.contains(next) && host.m_RemoteFrames.at(next).contains(2);
			};
			for (const uint64_t until = hub.now + 2000; hub.now < until && !ownWait();) if (!step(true)) return false;
			const bool ahead = ownWait();
			const uint64_t stalled = host.GetStats().nextFrame;
			hub.dropInputsFrom = 0; hub.tieAnswersHeld = false;
			const uint64_t begin = hub.now, before = applied[0];
			uint64_t longestWait = 0;
			for (; hub.now < begin + 3000;) {
				if (!step(false)) return false;
				if (host.m_ConsumerWaitingFrame && hub.now >= host.m_ConsumerWaitStartMs) longestWait = std::max(longestWait, hub.now - host.m_ConsumerWaitStartMs);
			}
			const bool held = host.m_PeerBridges.contains(2) && host.m_PeerBridges.at(2).committedHold;
			const bool routed = host.HasPeerFrameRoute(2);
			DiagnosticLine() << "[net-session-scene] silent-before-host-input stalled=" << stalled << " joiner_input_ahead=" << ahead << " routed=" << routed << " before=" << before
			          << " after=" << applied[0] << " longest_wait_ms=" << longestWait << " final_wait_ms=" << hub.now - displayedAt[0] << " held=" << held
			          << " next=" << host.GetStats().nextFrame << std::endl;
			if (!ahead) { *error = "the host never waited on its own input with the joiner's input in hand"; return false; }
			if (longestWait > 50 || hub.now - displayedAt[0] > 50 || applied[0] < before + 110 || !held) {
				*error = "authority ruling: the silent joiner's missing receipt gated the host past its 50 ms local hold bound: waited " + std::to_string(longestWait) +
				         " ms, applied " + std::to_string(applied[0] - before) + " frames in 3 s, held=" + std::to_string(held) + ", routed=" + std::to_string(routed);
				return false;
			}
			return true;
		}
		if (name == "drop-before-host-input") {
			auto& host = *rounds[0];
			host.m_PeerFrameTieWinners.clear();
			// As in a match whose ties the directory refused, no tie is decided until the joiner's link is gone.
			hub.tieAnswersHeld = true;
			// The host's inputs stop reaching the joiner first: the joiner waits for the host's next input while its own inputs, an input
			// delay ahead, keep arriving; then the link closes with the host holding the joiner's inputs but no receipt of its own.
			hub.dropInputsFrom = 1;
			const auto ownWait = [&] {
				const uint64_t next = host.GetStats().nextFrame;
				return !host.LocalInputAccepted(next) && host.m_RemoteFrames.contains(next) && host.m_RemoteFrames.at(next).contains(2);
			};
			for (const uint64_t until = hub.now + 2000; hub.now < until && !ownWait();) if (!step(true)) return false;
			const bool ahead = ownWait(), undecided = host.m_PeerFrameTieWinners.empty();
			const uint64_t stalled = host.GetStats().nextFrame;
			clientWire.Stop();
			hub.dropInputsFrom = 0; hub.tieAnswersHeld = false;
			const uint64_t begin = hub.now, before = applied[0];
			uint64_t longestWait = 0;
			for (; hub.now < begin + 3000;) {
				if (!step(false)) return false;
				if (host.m_ConsumerWaitingFrame && hub.now >= host.m_ConsumerWaitStartMs) longestWait = std::max(longestWait, hub.now - host.m_ConsumerWaitStartMs);
			}
			const bool held = host.m_PeerBridges.contains(2) && host.m_PeerBridges.at(2).committedHold;
			DiagnosticLine() << "[net-session-scene] drop-before-host-input stalled=" << stalled << " joiner_input_ahead=" << ahead << " undecided=" << undecided << " before=" << before << " after=" << applied[0]
			          << " longest_wait_ms=" << longestWait << " final_wait_ms=" << hub.now - displayedAt[0] << " held=" << held << " next=" << host.GetStats().nextFrame << std::endl;
			if (!ahead || !undecided) { *error = "the host never waited on its own input with the joiner's input in hand and no tie decided"; return false; }
			if (longestWait > 50 || hub.now - displayedAt[0] > 50 || applied[0] < before + 110 || !held) {
				*error = "authority ruling: the dropped joiner's missing receipt gated the host beyond the 50 ms local hold bound"; return false;
			}
			return true;
		}
		if (name == "kick-received" || name == "kick-received-two") {
			auto& host = *rounds[0];
			// The host's own next input is late, so the removed player's input for that frame has arrived before the kick.
			const auto arrived = [&] { const auto next = host.m_RemoteFrames.find(host.GetStats().nextFrame); return next != host.m_RemoteFrames.end() && next->second.contains(2); };
			hostWithholds = true;
			for (const uint64_t until = hub.now + 500; hub.now < until && !arrived();) if (!step(true)) return false;
			hostWithholds = false;
			if (!arrived()) { *error = "the kicked player's input for the host's next frame never arrived ahead of it"; return false; }
			const uint64_t next = host.GetStats().nextFrame;
			host.EvictRemovedPeer(2, "removed from the session", hub.now);
			hostSession.DisconnectReadyPeer(originalRoute, NetRejectReason::ParticipantRemoved, c_NetRemovedLinkText);
			const uint64_t boundary = host.m_PeerLeaveFrames.contains(2) ? host.m_PeerLeaveFrames.at(2) : 0;
			const uint64_t begin = hub.now;
			std::vector<uint64_t> before(applied), gap(count), firstTwo(count);
			for (; hub.now < begin + 2000;) {
				const auto previous = displayedAt;
				if (!step(false)) return false;
				rounds[1]->Tick(hub.now); clientSession.Tick(hub.now, false);
				for (size_t index = 0; index < count; ++index) {
					if (index == 1) continue;
					// The host's own late input before the kick is the row's setup, not a wait the kick caused.
					if (displayedAt[index] != previous[index]) gap[index] = std::max(gap[index], displayedAt[index] - std::max(previous[index], begin));
					if (!firstTwo[index] && applied[index] >= before[index] + 2) firstTwo[index] = hub.now - begin;
				}
			}
			bool stopped = false;
			for (size_t index = 0; index < count; ++index) {
				if (index == 1) continue;
				const uint64_t finalWait = hub.now - displayedAt[index];
				DiagnosticLine() << "[net-session-scene] " << name << " peer=" << index + 1 << " boundary=" << boundary << " before=" << before[index] << " after=" << applied[index]
				          << " two_updates_ms=" << firstTwo[index] << " greatest_gap_ms=" << gap[index] << " final_wait_ms=" << finalWait
				          << " next=" << rounds[index]->GetStats().nextFrame << " accepted=" << rounds[index]->PeerInputAccepted(rounds[index]->GetStats().nextFrame, 2) << std::endl;
				stopped |= !firstTwo[index] || firstTwo[index] > 50 || gap[index] > 50 || finalWait > 50 || applied[index] < before[index] + 110 || applied[index] <= boundary;
			}
			if (boundary <= next || stopped) { *error = "a kick froze a remaining player on the removed seat's input before its boundary"; return false; }
			return true;
		}
		if (name == "rematch-chat") {
			hub.row.laterSessionMessageFirst = true;
			rounds[0]->Complete("round ended");
			for (const uint64_t until = hub.now + 300; hub.now < until; ++hub.now) for (auto& round: rounds) round->Tick(hub.now);
			for (auto& round: rounds) round.reset();
			std::array<NetLobbySession, 2> lobbies;
			NetMatchConfig rematch;
			if (!NetMatchConfigUtil::DeriveRematchConfig(match, rematch, error)) return false;
			for (size_t index = 0; index < 2; ++index) {
				NetLobbySessionConfig config; config.host = index == 0; config.localPeerId = static_cast<uint8_t>(index + 1);
				config.matchConfig = rematch; config.startFrame = 1; config.autoReady = config.autoStart = false;
				config.startCountdownMs = 30000; config.session = sessions[index]; config.sessionNowMs = [&] { return hub.now; };
				config.remoteTransportPeerIds[index == 0 ? 2 : 1] = index == 0 ? originalRoute : clientSession.GetRemoteTransportPeerId();
				config.displayName = index == 0 ? "Captain" : "Joiner";
				if (!lobbies[index].Start(*sessionWires[index], config, error)) return false;
			}
			if (!clientSession.SendChat(c_NetChatScopeAll, "joiner watches the rematch countdown")) { *error = "the rematch chat was refused locally"; return false; }
			const uint64_t begin = hub.now; unsigned hostLines = 0;
			for (; hub.now < begin + 2000; ++hub.now) {
				for (auto& lobby: lobbies) lobby.Tick(hub.now - begin);
				for (const auto& line: hostSession.TakeChatEntries()) if (line.text == "joiner watches the rematch countdown") ++hostLines;
			}
			lobbies[0].SetLocalReady(true); lobbies[0].RequestStart();
			for (const uint64_t until = hub.now + 1000; hub.now < until; ++hub.now) for (auto& lobby: lobbies) lobby.Tick(hub.now - begin);
			const bool countdown = lobbies[1].IsStartCountdownRunning();
			lobbies[0].CancelStart();
			for (const uint64_t until = hub.now + 1000; hub.now < until; ++hub.now) for (auto& lobby: lobbies) lobby.Tick(hub.now - begin);
			const bool canceled = !lobbies[1].IsStartCountdownRunning();
			DiagnosticLine() << "[net-session-scene] rematch-chat received=" << hostLines << " host_messages=" << lobbies[0].GetStats().messagesReceived
			          << " joiner_messages=" << lobbies[1].GetStats().messagesReceived << " countdown=" << countdown << " canceled=" << canceled << std::endl;
			for (size_t index = 0; index < 2; ++index) DiagnosticLine() << "[net-session-scene] lobby-state peer=" << index + 1
			          << " state=" << static_cast<int>(lobbies[index].GetState()) << " reason=" << lobbies[index].GetFailureReason()
			          << " session=" << static_cast<int>(sessions[index]->GetState()) << " reader=" << (links[index]->frameReader != nullptr)
			          << " pending_bytes=" << links[index]->pendingBytes << " incoming_bytes=" << links[index]->incomingBytes
			          << " routes=" << links[index]->sessionRoutes.size() << " received=" << links[index]->received.size() << std::endl;
			if (hostLines != 1 || !countdown || !canceled || hub.delayedChatSends == 0) { *error = "the rematch lost the joiner's chat or its Start and Cancel exchange"; return false; }
			const uint32_t sentBeforeQuiet = clientWire.sends;
			for (const uint64_t until = hub.now + 1200; hub.now < until; ++hub.now) lobbies[1].Tick(hub.now - begin);
			const uint32_t quietSends = clientWire.sends - sentBeforeQuiet;
			if (quietSends > 64) { *error = "a silent lobby reader exceeded its bounded session window"; return false; }
			if (!clientSession.SendChat(c_NetChatScopeAll, "back at the lobby")) { *error = "the resumed lobby chat was refused locally"; return false; }
			unsigned resumedLines = 0;
			for (const uint64_t until = hub.now + 2000; hub.now < until; ++hub.now) {
				for (auto& lobby: lobbies) lobby.Tick(hub.now - begin);
				for (const auto& line: hostSession.TakeChatEntries()) if (line.text == "back at the lobby") ++resumedLines;
			}
			DiagnosticLine() << "[net-session-scene] lobby-order delayed_chat_sends=" << hub.delayedChatSends << " quiet_sends=" << quietSends
			          << " resumed_chat=" << resumedLines << std::endl;
			if (resumedLines != 1) { *error = "the lobby did not resume its chat after its reader returned"; return false; }
			// A queued chat keeps working while the next round starts on a new key.
			if (!clientSession.SendChat(c_NetChatScopeAll, "ready for the next round")) { *error = "the next-round chat was refused locally"; return false; }
			const auto startRound = [&](size_t index) {
				auto config = roundConfigs[index]; config.matchConfig = rematch;
				config.roundId = index == 0 ? rematch.roundId : 0; config.migrationKey.fill(0x40);
				rounds[index] = std::make_unique<NetLockstepCoordinator>();
				if (!rounds[index]->Start(*sessionWires[index], config, error)) return false;
				rounds[index]->DeferStopsToTickBoundary();
				rounds[index]->SetSessionEventSink([&, index](const NetTransportEvent& event) { sessions[index]->InjectEvent(event, hub.now); });
				return true;
			};
			if (!startRound(0)) return false;
			unsigned nextLines = 0;
			for (const uint64_t until = hub.now + 300; hub.now < until; ++hub.now) {
				rounds[0]->Tick(hub.now); hostSession.Tick(hub.now, false); lobbies[1].Tick(hub.now - begin);
				for (const auto& line: hostSession.TakeChatEntries()) if (line.text == "ready for the next round") ++nextLines;
			}
			if (!startRound(1)) return false;
			produced.assign(count, 1); applied.assign(count, 0); displayedAt.assign(count, 0); credits.assign(count, 0);
			for (const uint64_t until = hub.now + 2000; hub.now < until && std::min(applied[0], applied[1]) < 60;) if (!step(true)) return false;
			for (const auto& line: hostSession.TakeChatEntries()) if (line.text == "ready for the next round") ++nextLines;
			DiagnosticLine() << "[net-session-scene] next-round chat=" << nextLines << " host_frame=" << applied[0] << " joiner_frame=" << applied[1] << std::endl;
			if (nextLines != 1 || std::min(applied[0], applied[1]) < 60 || !rounds[0]->IsRunning() || !rounds[1]->IsRunning()) {
				*error = "a queued session message or the next round stopped when its key changed"; return false;
			}
			return true;
		}
		if (name == "leave-rejoin" || name == "leave-rejoin-two") {
			const bool two = count == 2;
			uint64_t hostGap = 0;
			const auto hostStep = [&](bool joinerPlaying) { const uint64_t shown = displayedAt[0]; if (!step(joinerPlaying)) return false; if (displayedAt[0] != shown) hostGap = std::max(hostGap, displayedAt[0] - shown); return true; };
			unsigned directorySteps = 0; bool waitingDial = true, answered = false;
			hostWire.SetPump([&] {
				++directorySteps;
				if (waitingDial && hub.now >= 6500) {
					waitingDial = false; auto config = joinConfig; ++config.localNonce;
					answered = returningSession.StartClient(returningWire, "fixture", config, error);
				}
			});
			const uint64_t departure = hub.now, before = applied[0];
			rounds[1]->Leave("Match left");
			if (!two) rounds[2]->Leave("Match left");
			// A leaving player plays on to the frame it declared before it goes.
			const auto declared = [&] { const auto ended = rounds[0]->m_PeerRoundEnds.find(2); return ended != rounds[0]->m_PeerRoundEnds.end() && rounds[0]->GetStats().nextFrame > ended->second.frame; };
			if (two) for (const uint64_t until = hub.now + 1000; hub.now < until && !declared();) if (!hostStep(true)) return false;
			for (; hub.now < departure + 250;) if (!hostStep(false)) return false;
			clientWire.Stop();
			if (!two) otherWire.Stop();
			uint64_t answerAt = 0;
			for (; hub.now < 8500;) {
				if (!hostStep(false)) return false;
				if (answered) returningSession.Tick(hub.now);
				if (!answerAt && returningSession.IsReady()) answerAt = hub.now;
				if (answerAt && hostSession.GetReadyPeers().size() == 1) break;
			}
			const auto ready = hostSession.GetReadyPeers();
			const bool live = rounds[0]->IsRunning() && applied[0] > before + 60;
			const bool held = rounds[0]->HasHeldAISeat(2);
			DiagnosticLine() << "[net-session-scene] leave-rejoin live=" << live << " held=" << held << " directory_steps=" << directorySteps
			          << " answered_ms=" << (answerAt ? answerAt - 6500 : 0) << " ready=" << ready.size() << " returning_seat=" << static_cast<int>(returningSession.GetLocalPeerId() + 1) << std::endl;
			DiagnosticLine() << "[net-session-scene] departure-state next=" << rounds[0]->GetStats().nextFrame << " applied=" << applied[0]
			          << " declarations=" << rounds[0]->m_PeerRoundEnds.size() << " authority=" << rounds[0]->PeerGroupHasAuthority(1)
			          << " bridges=" << rounds[0]->m_PeerBridges.size() << " previous_retained=" << rounds[0]->m_MigrationHistory.contains(rounds[0]->GetStats().nextFrame - 1)
			          << " waiting=" << rounds[0]->WaitsForPlacement() << " holds=" << rounds[0]->GetStats().peers.at(2).holds << "/" << (two ? 0 : rounds[0]->GetStats().peers.at(3).holds) << std::endl;
			if (!live || !held || !answerAt || answerAt > 8500 || ready.size() != 1 || returningSession.GetLocalPeerId() != 1) {
				*error = "the last player's leave stopped the host or its returning dial was not answered within two seconds"; return false;
			}
			const uint64_t activation = rounds[0]->GetStats().nextFrame + 24;
			if (!rounds[0]->SchedulePeerReclaim(2, ready.front().transportPeerId, 2, activation, error)) return false;
			auto config = roundConfigs[1]; config.startFrame = activation; config.roundId = match.roundId;
			config.joinsRunningRound = config.continuesMatch = true;
			config.initialSeatHolds = rounds[0]->HeldTransactions(); config.initialPeerLeaves = rounds[0]->m_PeerLeaveFrames;
			config.originalRoundConfigHash = rounds[0]->m_RoundConfigHash;
			config.peerSessionLinks = std::make_shared<NetPeerSessionLinks>();
			config.peerSessionLinks->sessionClock = [&] { return hub.now; };
			config.remoteTransportPeerIds[1] = returningSession.GetRemoteTransportPeerId();
			// The new reader starts from the host's snapshot, whose activation has already been signed.
			const auto certificate = rounds[0]->m_PeerBridgeCertificates.find({activation, 1});
			NetHostMigrationMessage agreed; uint32_t voters = 0;
			if (certificate == rounds[0]->m_PeerBridgeCertificates.end() ||
			    !rounds[0]->DecodePeerBridgeCertificate(certificate->second, agreed, voters) || !rounds[0]->PeerGroupHasAuthority(voters) ||
			    agreed.preparedFrame != 1 || agreed.frame != activation || agreed.members != std::vector<uint8_t>{2} ||
			    agreed.configHash != config.originalRoundConfigHash || !rounds[0]->m_ReclaimTransactions.contains(2)) {
				*error = "the return snapshot did not carry its committed activation"; return false;
			}
			config.initialSeatReclaims[2] = rounds[0]->m_ReclaimTransactions.at(2);
			// The returner's private catch-up of the round's committed history reaches the agreed frame as the round does: no return
			// starts ahead of the round's head.
			for (const uint64_t until = hub.now + 2000; hub.now < until && applied[0] + 1 < activation;) if (!hostStep(false)) return false;
			config.peerIncarnations[2] = config.initialSeatReclaims.at(2).seatIncarnation;
			if (config.initialSeatReclaims.at(2).activationFrame != activation || config.peerIncarnations.at(2) != 2) {
				*error = "the return snapshot changed its activation or player incarnation"; return false;
			}
			rounds[1] = std::make_unique<NetLockstepCoordinator>();
			if (!rounds[1]->Start(returningWire, config, error)) return false;
			// As the runner does, the live start takes the replayed history it caught up through.
			rounds[1]->AdoptReplayedSeatTransitions(*rounds[0], activation - 1);
			rounds[1]->DeferStopsToTickBoundary();
			sessions[1] = &returningSession;
			rounds[1]->SetSessionEventSink([&](const NetTransportEvent& event) { returningSession.InjectEvent(event, hub.now); });
			applied[1] = activation - 1; produced[1] = activation; credits[1] = 0;
			(void)rounds[0]->SendReturnerTheRoundFrom(2, activation);
			uint64_t reclaimedAt = 0;
			for (const uint64_t until = hub.now + 3000; hub.now < until;) {
				if (!hostStep(true)) return false;
				if (!reclaimedAt && applied[0] >= activation && !rounds[0]->HasHeldAISeat(2)) reclaimedAt = hub.now;
			}
			DiagnosticLine() << "[net-session-scene] returned-host-gap greatest_gap_ms=" << hostGap << std::endl;
			if (two && hostGap > 50) { *error = "the host waited on a returning seat that had not started playing"; return false; }
			const uint64_t accepted = rounds[0]->m_PeerAcceptedThrough[2];
			DiagnosticLine() << "[net-session-scene] returned-input host_frame=" << applied[0] << " joiner_frame=" << applied[1]
			          << " accepted_through=" << accepted << " holds=" << rounds[0]->GetStats().peers.at(2).holds
			          << " reclaimed_ms=" << (reclaimedAt ? reclaimedAt - answerAt : 0) << std::endl;
			if (!reclaimedAt || reclaimedAt > answerAt + 2500) {
				*error = "the returning seat missed its original two-and-a-half-second reclaim bound"; return false;
			}
			if (!rounds[0]->IsRunning() || !rounds[1]->IsRunning() || rounds[0]->HasHeldAISeat(2) || rounds[1]->HasHeldAISeat(2) ||
			    std::min(applied[0], applied[1]) < activation + 100 || accepted < activation + 100 || rounds[0]->GetStats().peers.at(2).holds != 1) {
				*error = "the answered returning seat did not resume its accepted input and live play"; return false;
			}
			rounds[1]->Leave("Match left");
			for (const uint64_t until = hub.now + 250; hub.now < until;) if (!step(false)) return false;
			if (!rounds[0]->IsRunning() || !rounds[0]->CompleteAtAgreedEnd("Match ended by host")) { *error = "the host could not choose End Match after an ordinary leave"; return false; }
			const uint64_t endAt = hub.now;
			for (; hub.now < endAt + 200 && rounds[0]->IsRunning();) if (!step(false)) return false;
			DiagnosticLine() << "[net-session-scene] host-end completed=" << (rounds[0]->GetState() == NetLockstepState::Stopped)
			          << " after_ms=" << hub.now - endAt << std::endl;
			if (rounds[0]->GetState() != NetLockstepState::Stopped || rounds[0]->GetStats().timeoutReason.rfind("Complete:", 0) != 0) {
				*error = "the host's End Match waited on a seat that had already left"; return false;
			}
			return true;
		}
		*error = "unknown scene lifecycle row"; return false;
	}

	int NetSessionPlaneSelfTest::Run() {
		std::call_once(s_FixtureObservers, [] {
			s_FrameMembers = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerFrameMembersAt(frame); };
			s_FrameAuthor = [](const NetLockstepCoordinator& peer, const NetLockstepReadyFrame& ready) -> uint8_t {
				const uint8_t local = peer.m_Config.localPeerId;
				const uint32_t members = peer.PeerFrameMembersAt(ready.frame);
				if (!peer.UsesPeerFrameGroups() || (members & (1U << (local - 1)))) return local;
				if (!peer.IsLocalPrivateReaderAt(ready.frame)) return 0;
				const auto witnesses = peer.m_PeerFrameWitnesses.find(ready.frame);
				if (witnesses == peer.m_PeerFrameWitnesses.end()) return 0;
				return WitnessedFrameAuthor(ready.frame, members, peer.CommittedFramePrefix(ready), peer.PeerFramePrefix(ready.frame), witnesses->second);
			};
			s_WaitState = [](const NetLockstepCoordinator& peer) {
				std::string starts;
				for (uint8_t remote: peer.m_RemotePeerIds) starts += " start=" + std::to_string(remote) + ":" + std::to_string(peer.m_RemoteStartsReceived.contains(remote)) +
				    "/required=" + std::to_string(peer.IsRemoteRequiredForFrame(remote, peer.m_Config.startFrame)) + "/held=" + std::to_string(peer.IsSeatUnderAI(remote, peer.m_Config.startFrame));
				return " next=" + std::to_string(peer.m_Stats.nextFrame) + " blocked=" + peer.m_AdvanceBlock +
				    " consumer=" + (peer.m_ConsumerWaitingFrame ? std::to_string(*peer.m_ConsumerWaitingFrame) : "none") +
				    " wait_since=" + std::to_string(peer.m_ConsumerWaitStartMs) + " members=" + std::to_string(peer.m_FrameGroupMembers) +
				    " local_held=" + std::to_string(peer.m_LocalSeatHeld) + " granted=" + (peer.m_GrantedSimulationTick ? std::to_string(*peer.m_GrantedSimulationTick) : "none") +
				    " hold_certificate=" + std::to_string(NetPeerFrameDetail::HasHoldDecision(peer.m_PeerBridgeCertificates, peer.m_Stats.nextFrame)) + starts;
			};
			s_HeldSeats = [](const NetLockstepCoordinator& peer, uint64_t frame) { return peer.PeerHeldSeatsAt(frame); };
			s_HoldBoundaries = [](const NetLockstepCoordinator& peer) -> const HoldBoundaries& { return peer.m_PeerFrameHolds; };
			s_OwnHoldPending = [](const NetLockstepCoordinator& peer, uint64_t frame) {
				for (const auto& [key, kept]: peer.m_PendingPeerDecisions)
					if (NetPeerFrameDetail::IsHoldDecision(key.second) && key.first >= frame && std::find(kept.first.members.begin(), kept.first.members.end(), peer.GetConfig().localPeerId) != kept.first.members.end()) return true;
				return false;
			};
			s_ReturnRoute = [](const NetLockstepCoordinator& peer, uint8_t seat) { return peer.PeerFrameRoute(seat); };
			s_ReturnTransport = [](const NetLockstepCoordinator& peer, uint8_t seat) {
				// FLAGGED fixture departure: a surviving mesh proposer can own a listener/dial route outside the old host star.
				return peer.PeerFrameRoute(seat).second;
			};
		});
		if (!TimerMan::IsConstructed()) TimerMan::Construct();
		if (const char* name = std::getenv("CC_TEST_SESSION_SCENE_CASE")) {
			std::string error;
			const bool passed = CheckSceneLifecycle(name, &error);
			DiagnosticLine() << "[net-session-plane-selftest] " << (passed ? "PASS " : "FAIL ") << name << (error.empty() ? "" : ": " + error) << std::endl;
			if (passed) DiagnosticLine() << "[net-session-plane-selftest] PASS" << std::endl;
			return passed ? 0 : 1;
		}
		std::vector<Case> rows;
		const auto gap = [&](const char* name, uint8_t subject, uint64_t ms) { Case row{name}; row.subject = subject; row.gapMs = ms; return row; };
		rows.push_back(gap("peer_returns_2s", 2, 2000)); rows.push_back(gap("peer_returns_8s", 2, 8000)); rows.push_back(gap("peer_returns_40s", 2, 40000));
		auto frozen = gap("peer_frozen_10s_keepalives", 2, 10000); frozen.frozen = true; rows.push_back(frozen);
		auto tailReturn = frozen; tailReturn.name = "peer_frozen_10s_return_by_tail_only"; tailReturn.returnByTailOnly = true; rows.push_back(tailReturn);
		auto tailReturnTwo = tailReturn; tailReturnTwo.name = "two_internet_joiner_frozen_10s_return_by_tail_only"; tailReturnTwo.seats = 2; tailReturnTwo.internet = tailReturnTwo.directoryBoth = true; rows.push_back(tailReturnTwo);
		rows.push_back(gap("host_absent_3s", 1, 3000)); rows.push_back(gap("host_absent_20s", 1, 20000));
		auto slowMachine = gap("joiner_machine_at_90_percent_20s", 2, 20000); slowMachine.slowPercent = 90; rows.push_back(slowMachine);
		auto slowerMachine = gap("joiner_machine_at_80_percent_20s", 2, 20000); slowerMachine.slowPercent = 80; rows.push_back(slowerMachine);
		auto slowHearing = gap("held_seat_and_a_member_slow_to_hear_hold_nobody_else", 2, 10000); slowHearing.lateReceiptsFrom = 3; slowHearing.lateReceiptsMs = 2000; rows.push_back(slowHearing);
		const auto slowThree = [&](const char* name, uint32_t percent) {
			Case row = gap(name, 2, 20000); row.seats = 3; row.slowPercent = percent; row.rttMs = row.otherRttMs = 100; row.heldOnce = true; return row;
		};
		rows.push_back(slowThree("slow_joiner_at_80_percent_three_seats_over_100ms_20s", 80));
		rows.push_back(slowThree("slow_joiner_at_50_percent_three_seats_over_100ms_20s", 50));
		const auto slowTwo = [&](const char* name, uint32_t percent) {
			Case row = gap(name, 2, 20000); row.seats = 2; row.internet = row.directoryBoth = true; row.slowPercent = percent;
			row.rttMs = row.otherRttMs = 100; row.heldOnce = row.noRefusals = row.strictBound = true; return row;
		};
		rows.push_back(slowTwo("two_internet_slow_joiner_at_80_percent_20s", 80));
		rows.push_back(slowTwo("two_internet_slow_joiner_at_50_percent_20s", 50));
		// The field's slow player: held for a slow spell, then able to catch up only a little faster than the round - it returns once, caught up.
		auto slowSpell = slowTwo("two_internet_joiner_held_for_a_slow_spell_catches_up_at_115_percent", 80); slowSpell.catchUpPercent = 115;
		slowSpell.postReturnMs = 35000; rows.push_back(slowSpell);
		// A seat that catches up only a little faster than the round and then keeps its rate returns and stays: what it still trails by
		// once caught up is the round trip of its tail and report, at any internet round trip.
		const auto lagged = [&](const char* name, uint32_t rtt, uint32_t catchUp, uint64_t gapMs, uint64_t withinMs) {
			Case row = gap(name, 2, gapMs); row.seats = 3; row.rttMs = row.otherRttMs = rtt; row.catchUpPercent = catchUp; row.heldOnce = true;
			row.returnWithinMs = withinMs; row.postReturnMs = withinMs + 16000; return row;
		};
		rows.push_back(lagged("joiner_catching_up_at_105_percent_returns_over_100ms", 100, 105, 800, 20000));
		rows.push_back(lagged("joiner_catching_up_at_105_percent_returns_over_200ms", 200, 105, 800, 20000));
		rows.push_back(lagged("joiner_catching_up_at_102_percent_returns_over_100ms", 100, 102, 600, 40000));
		// The four-player match's shape: a donor sends the tail only once a second voter has witnessed it, and the caught-up seat returns.
		auto fourSeats = lagged("joiner_catching_up_at_105_percent_returns_four_seats_over_100ms", 100, 105, 800, 20000); fourSeats.seats = 4; rows.push_back(fourSeats);
		// A tail sent by a seat other than the one that proposes the return: it waits for the host's witness, then crosses its own link.
		auto otherDonor = lagged("joiner_catching_up_at_105_percent_returns_from_another_donor", 100, 105, 800, 20000);
		otherDonor.thirdSubjectRttMs = 20; otherDonor.otherRttMs = 200; otherDonor.donorSeat = 3; rows.push_back(otherDonor);
		// The same with only a fiftieth of a second of slack: the catch-up leaves too little to cover the tail's path.
		auto fourSlow = lagged("joiner_catching_up_at_102_percent_returns_four_seats_over_100ms", 100, 102, 600, 40000); fourSlow.seats = 4; rows.push_back(fourSlow);
		auto otherSlow = lagged("joiner_catching_up_at_102_percent_returns_from_another_donor", 100, 102, 600, 40000);
		otherSlow.thirdSubjectRttMs = 20; otherSlow.otherRttMs = 200; otherSlow.donorSeat = 3; rows.push_back(otherSlow);
		for (const auto& [name, seats, subject, rtt, donor]: std::vector<std::tuple<const char*, uint8_t, uint8_t, uint32_t, uint8_t>>{
		    {"three_seats_held_host_catches_up_then_runs_at_round_rate_over_100ms", 3, 1, 100, 0},
		    {"three_seats_held_host_catches_up_then_runs_at_round_rate_over_250ms", 3, 1, 250, 0},
		    {"four_seats_held_joiner_catches_up_then_runs_at_round_rate_over_100ms", 4, 2, 100, 0},
		    {"four_seats_held_joiner_catches_up_then_runs_at_round_rate_over_200ms", 4, 2, 200, 0},
		    {"four_seats_held_joiner_catches_up_then_runs_at_round_rate_over_250ms", 4, 2, 250, 0},
		    {"four_seats_held_joiner_at_round_rate_uses_a_different_donor_over_200ms", 4, 2, 200, 3}}) {
			auto row = lagged(name, rtt, 105, 800, 21000); row.seats = seats; row.subject = subject; row.roundRateAfterMs = 18000;
			if (donor) { row.thirdSubjectRttMs = 20; row.donorSeat = donor; }
			rows.push_back(row);
		}
		// A seat whose inputs keep arriving within the bound, though most are lost: each message carries the window back past the waited
		// frame (the input delay and the bound), so no seat that plays waits past the bound on it and nothing need hold it.
		auto thinned = gap("joiner_keeps_one_input_in_three_over_100ms", 2, 10000); thinned.seats = 3; thinned.rttMs = thinned.otherRttMs = 100;
		thinned.keepInputEvery = 3; rows.push_back(thinned);
		// A seat held for waiting past the bound while its link stays up learns its hold: every peer plays the same history to the end.
		auto lostInputs = gap("joiner_inputs_lost_1s_held_and_back_over_100ms", 2, 1000); lostInputs.seats = 3; lostInputs.inputsLost = true;
		lostInputs.rttMs = lostInputs.otherRttMs = 100; rows.push_back(lostInputs);
		// The W+L shape: a joiner a little slower than the host over the internet, its referee answering as late as the directory does.
		// A 240 ms HTTP exchange cannot extend the survivor's 50 ms bound, even with the minute's routine allowance spent.
		auto lateReferee = gap("two_internet_late_joiner_takes_every_decision_with_a_240ms_referee", 2, 20000); lateReferee.seats = 2;
		lateReferee.internet = lateReferee.directoryBoth = lateReferee.noRefusals = true; lateReferee.slowPercent = 95;
		lateReferee.rttMs = lateReferee.otherRttMs = 100; lateReferee.directoryExchangeMs = 240; rows.push_back(lateReferee);
		// A secure exchange past the timeout floor succeeds after the client's measured timeout adapts.
		auto pastTimeout = lateReferee; pastTimeout.name = "two_internet_late_joiner_with_a_referee_past_the_tie_timeout"; pastTimeout.directoryExchangeMs = 300;
		rows.push_back(pastTimeout);
		// Three retained seats keep their local bounds while two links fail; direct arbitration follows the agreed host where no referee is configured.
		auto threeTie = gap("three_retained_seats_tie_goes_to_the_hosts_half", 3, 10000); threeTie.seats = 3; threeTie.internet = threeTie.directoryBoth = true;
		threeTie.secondSubject = 2; threeTie.secondAfterMs = 4000; rows.push_back(threeTie);
		// A voter silent from the return's first vote: the members that play neither wait on its vote nor past the bound.
		auto silentVoter = gap("held_seat_return_meets_a_silent_voter", 2, 10000); silentVoter.seats = 4; silentVoter.silentVoter = 4; silentVoter.silentVoterMs = 2000;
		silentVoter.postReturnMs = 6000; rows.push_back(silentVoter);
		auto farReturn = gap("one_return_for_a_held_seat_over_150ms_round_trips", 2, 10000); farReturn.rttMs = farReturn.otherRttMs = 150; rows.push_back(farReturn);
		auto inTurn = gap("three_seats_second_joiner_drops_while_the_first_is_held", 2, 10000); inTurn.seats = 3; inTurn.secondSubject = 3; inTurn.secondAfterMs = 4000; rows.push_back(inTurn);
		auto silentHostPair = gap("four_direct_seats_host_and_joiner_silent_keeps_survivors_bound", 1, 3000);
		silentHostPair.secondSubject = 2; silentHostPair.rttMs = silentHostPair.otherRttMs = 100; rows.push_back(silentHostPair);
		auto heldAgain = gap("two_internet_joiner_held_again_while_catching_up", 2, 3000); heldAgain.seats = 2; heldAgain.internet = heldAgain.directoryBoth = true;
		heldAgain.refaultPercent = 70; heldAgain.refaultAfterMs = 200; heldAgain.refaultMs = 5000; rows.push_back(heldAgain);
		auto heldUnblocked = heldAgain; heldUnblocked.name = "two_internet_joiner_held_again_while_never_blocked"; heldUnblocked.refaultPercent = 92; heldUnblocked.refaultMs = 8000; rows.push_back(heldUnblocked);
		auto slowJoinerTwo = gap("two_internet_joiner_machine_at_80_percent_20s", 2, 20000); slowJoinerTwo.seats = 2; slowJoinerTwo.internet = true; slowJoinerTwo.slowPercent = 80; rows.push_back(slowJoinerTwo);
		auto slowHost = gap("two_internet_host_machine_at_90_percent_20s", 1, 20000); slowHost.seats = 2; slowHost.internet = true; slowHost.slowPercent = 90; rows.push_back(slowHost);
		// The slow host's directory is out of reach but its peer link stays up: the survivor installs the hold locally without waiting for either referee.
		auto slowHostDark = slowHost; slowHostDark.name = "two_internet_slow_host_whose_directory_is_out_of_reach_20s"; slowHostDark.subjectDirectoryDark = true; rows.push_back(slowHostDark);
		auto majority = gap("partition_3_1", 4, 3000); majority.partition = true; rows.push_back(majority);
		auto tie = gap("partition_2_2_host_tie", 3, 3000); tie.tie = tie.internet = tie.directoryBoth = tie.splitContract = true; rows.push_back(tie);
		auto tieThenDrop = tie; tieThenDrop.name = "refereed_2_2_split_then_a_second_drop_in_the_same_hold"; tieThenDrop.gapMs = 10000;
		tieThenDrop.secondSubject = 2; tieThenDrop.secondAfterMs = 4000; rows.push_back(tieThenDrop);
		auto two = gap("two_direct_host_3s", 1, 3000); two.seats = 2; two.splitContract = true; rows.push_back(two);
		auto relay = gap("relay_link_blackout_return", 2, 8000); relay.blackout = true; rows.push_back(relay);
		for (uint8_t subject: {2, 1}) for (uint64_t ms: {150, 400, 1500, 3000}) {
			const char* name = subject == 2 ? (ms == 150 ? "joiner_spike_150ms" : ms == 400 ? "joiner_spike_400ms" : ms == 1500 ? "joiner_hiccup_1500ms" : "joiner_hiccup_3s")
			                              : (ms == 150 ? "host_spike_150ms" : ms == 400 ? "host_spike_400ms" : ms == 1500 ? "host_hiccup_1500ms" : "host_hiccup_3s");
			rows.push_back(gap(name, subject, ms));
		}
		Case loss{"loss_20percent_60s"}; loss.rttMs = 100; loss.lossPercent = 20; loss.steadyMs = 60000; rows.push_back(loss);
		Case steady{"steady_rtt_250ms_120s"}; steady.rttMs = 250; steady.steadyMs = 120000; rows.push_back(steady);
		Case hostPath{"two_host_relay_100ms_120s"}; hostPath.subject = 1; hostPath.seats = 2; hostPath.rttMs = 100; hostPath.steadyMs = 120000; hostPath.measurePace = true; rows.push_back(hostPath);
		hostPath.name = "four_host_relay_100ms_joiners_20ms_120s"; hostPath.seats = 4; hostPath.otherRttMs = 20; rows.push_back(hostPath);
		auto internetHost = gap("two_internet_host_3s_directory_reachable", 1, 3000); internetHost.seats = 2; internetHost.internet = true; rows.push_back(internetHost);
		auto directHost = gap("two_host_3s_no_directory_configured", 1, 3000); directHost.seats = 2; directHost.splitContract = true; rows.push_back(directHost);
		auto joiner = gap("two_joiner_3s_internet_and_direct", 2, 3000); joiner.seats = 2; joiner.internet = true; rows.push_back(joiner);
		auto both = gap("two_internet_host_and_directory_3s", 1, 3000); both.seats = 2; both.internet = both.directoryDown = true; rows.push_back(both);
		// The relay kit's lever: the host's peer link goes dark while both still reach the directory, so the host keeps the tie and the joiner returns.
		auto hostBlackout = gap("two_internet_host_blackout_directory_reachable_by_both", 1, 8000);
		hostBlackout.seats = 2; hostBlackout.internet = hostBlackout.directoryBoth = hostBlackout.blackout = true;
		hostBlackout.rttMs = hostBlackout.otherRttMs = 24; hostBlackout.joinerInputsLostFirstMs = 50; rows.push_back(hostBlackout);
		// With no input in flight both halves reach the local bound, hold each other and play; only the later arbitration loser image-returns.
		auto bothStall = gap("two_internet_host_blackout_both_halves_stall", 1, 8000);
		bothStall.seats = 2; bothStall.internet = bothStall.directoryBoth = bothStall.blackout = bothStall.splitContract = true;
		bothStall.rttMs = bothStall.otherRttMs = 24; rows.push_back(bothStall);
		// The host's peer and directory links go dark: both halves keep the local bound; the host learns arbitration when its directory recovers.
		auto hostDark = gap("two_internet_host_blackout_directory_dark", 1, 8000);
		hostDark.seats = 2; hostDark.internet = hostDark.blackout = hostDark.splitContract = true;
		hostDark.rttMs = hostDark.otherRttMs = 24; rows.push_back(hostDark);
		// A joiner's whole connection gone longer than the host keeps history returns through a private image.
		auto pastHistory = gap("two_internet_joiner_dark_past_history", 2, 75000);
		pastHistory.seats = 2; pastHistory.internet = pastHistory.blackout = pastHistory.splitContract = pastHistory.imageReturn = true;
		pastHistory.rttMs = pastHistory.otherRttMs = 24; rows.push_back(pastHistory);
		// The same image return at a real round trip (hold-shapes part 1, #19).
		auto pastHistoryFar = pastHistory; pastHistoryFar.name = "two_internet_joiner_dark_past_history_over_100ms"; pastHistoryFar.rttMs = pastHistoryFar.otherRttMs = 100; rows.push_back(pastHistoryFar);
		// A voter silent from the return's first vote, and a second tie inside one hold, at a real round trip.
		auto silentVoterFar = silentVoter; silentVoterFar.name = "held_seat_return_meets_a_silent_voter_over_100ms"; silentVoterFar.rttMs = silentVoterFar.otherRttMs = 100; rows.push_back(silentVoterFar);
		auto tieThenDropFar = tieThenDrop; tieThenDropFar.name = "refereed_2_2_split_then_a_second_drop_in_the_same_hold_over_100ms"; tieThenDropFar.rttMs = tieThenDropFar.otherRttMs = 100; rows.push_back(tieThenDropFar);
		for (const auto& [name, mode, stage]: std::vector<std::tuple<const char*, uint8_t, uint8_t>>{
		    {"dedicated_match_stopped_joiner_keeps_the_survivor_bound", 2, 0},
		    {"persistent_world_stopped_joiner_keeps_the_survivor_bound", 3, 0},
		    {"agreed_start_gate_stopped_joiner_keeps_the_survivor_bound", 0, 1},
		    {"agreed_brain_placement_stopped_joiner_keeps_the_survivor_bound", 0, 2}}) {
			auto row = gap(name, 2, 2000); row.seats = 3; row.frozen = true;
			row.mode = mode; row.rosterStage = stage; row.rttMs = row.otherRttMs = 100; rows.push_back(row);
		}
		// One schedule from its text, run alone with every engine line printed: the way to read a failing shape the driver printed.
		if (const char* text = std::getenv("CC_TEST_HOLD_SCHEDULE")) {
			Schedule schedule;
			// "seed:<n>" runs the driver's own round for that seed.
			if (std::string_view(text).starts_with("seed:")) schedule = GenerateSchedule(std::strtoull(text + 5, nullptr, 10));
			else if (!ParseSchedule(text, schedule)) { DiagnosticLine() << "[net-session-plane-selftest] FAIL CC_TEST_HOLD_SCHEDULE does not parse"; return 1; }
			if (!schedule.endMs) schedule.endMs = ScheduleEndMs(schedule);
			Fixture fixture; const auto finding = RunSchedule(schedule, fixture);
			DiagnosticLine() << "[net-session-plane-selftest] schedule " << FormatSchedule(schedule);
			DiagnosticLine() << "[net-session-plane-selftest] " << (finding ? "FAIL " : "PASS ") << NameSchedule(schedule) << " seed=" << schedule.seed
			                 << (finding ? " property=" + finding->property + " kind=" + finding->kind + ": " + finding->detail : "") << fixture.RefereeEvidence();
			if (!finding) DiagnosticLine() << "[net-session-plane-selftest] PASS";
			return finding ? 1 : 0;
		}
		for (const auto& [name, seats, exchange, dark]: std::vector<std::tuple<const char*, uint8_t, uint32_t, bool>>{
		    {"split_two_seats_both_hold_before_a_300ms_reply_loser_images_back", 2, 300, false},
		    {"split_four_seats_both_hold_before_a_1000ms_reply_winner_history_untouched", 4, 1000, false},
		    {"split_four_seats_directory_dark_both_keep_bound_then_loser_images_back", 4, 100, true}}) {
			Case row{name}; row.seats = seats; row.subject = seats == 2 ? 2 : 3; row.gapMs = 5000;
			row.internet = row.directoryBoth = row.splitContract = true; row.tie = seats == 4;
			row.directoryExchangeMs = exchange; row.directoryDown = dark; rows.push_back(row);
		}
		for (const auto& [name, internet, gapMs]: std::vector<std::tuple<const char*, bool, uint64_t>>{
		    {"hung_host_3s_direct_survivor_holds_without_a_split", false, 3000},
		    {"hung_host_3s_and_directory_dark_survivor_holds_locally", true, 3000},
		    {"hung_host_18s_direct_survivor_holds_then_migrates_roster", false, 18000}}) {
			Case row{name}; row.seats = 2; row.subject = 1; row.gapMs = gapMs; row.hung = true;
			row.internet = internet; row.directoryDown = internet; rows.push_back(row);
		}
		int passed = 0, failed = 0;
		const char* selected = std::getenv("CC_TEST_SESSION_PLANE_CASE");
		if (selected && std::string_view(selected) == "fixed_session_fixtures") selected = nullptr;
		constexpr const char* heldHostRow = "a_held_host_preserves_controller_disable_before_its_return_arrives";
		// This row uses the actor handoff's process-wide coordinator; run it before the independent wire fixtures.
		if (!selected || std::string_view(selected) == heldHostRow) {
			std::string error;
			const bool ok = CheckHeldHostControllers(&error);
			DiagnosticLine() << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << heldHostRow << (error.empty() ? "" : ": " + error);
			if (ok) ++passed; else ++failed;
			if (selected) {
				if (ok) DiagnosticLine() << "[net-session-plane-selftest] PASS";
				return ok ? 0 : 1;
			}
		}
		DiagnosticLine() << "[net-session-plane-selftest] topology=single-box proof=false forced_rows=" << (selected ? 1 : rows.size() + 45 + FixedScheduleRows().size()) << std::endl;
		// Every row runs on a pool of worker threads: a named row keeps its engine lines and is printed as soon as it ends, the longest
		// rows started first; the random driver's rounds follow them in the same pool.
		QuietLines quiet(std::cout);
		struct RowRun { std::string name; std::function<bool(std::string&)> check; std::string error, lines; bool ok = false; uint64_t cost = 0; };
		std::vector<RowRun> named;
		if (!selected || std::string_view(selected) == "pause_policy_is_never_offered_or_agreed")
			named.push_back({"pause_policy_is_never_offered_or_agreed", CheckPauseNeverAgreed});
		if (!selected || std::string_view(selected) == "authority_fixture_checks_each_half_and_never_waives_the_bound")
			named.push_back({"authority_fixture_checks_each_half_and_never_waives_the_bound", CheckAuthorityOracle});
		// FLAGGED fixture departure: an exact input lost on one wire can create two continuations without a total transport cut.
		if (!selected || std::string_view(selected) == "history_fixture_requires_causal_input_wire_evidence")
			named.push_back({"history_fixture_requires_causal_input_wire_evidence", CheckInputHistoryOracle});
		if (!selected || std::string_view(selected) == "history_fixture_keeps_a_proven_split_after_its_reader_returns")
			named.push_back({"history_fixture_keeps_a_proven_split_after_its_reader_returns", CheckReturnedReaderHistoryOracle});
		if (!selected || std::string_view(selected) == "history_fixture_accepts_only_exact_receipt_wire_evidence")
			named.push_back({"history_fixture_accepts_only_exact_receipt_wire_evidence", CheckReceiptHistoryOracle});
		if (!selected || std::string_view(selected) == "private_history_author_requires_authenticated_commit_evidence") {
			named.push_back({"private_history_author_requires_authenticated_commit_evidence", [](std::string& error) {
				for (unsigned variant = 0; variant < 11; ++variant) {
					NetLockstepCoordinator peer;
					peer.m_Config.sessionId = 0xA0710;
					peer.m_Config.matchConfig = NetMatchConfigUtil::MakeDefault(peer.m_Config.sessionId);
					peer.m_Config.matchConfig.roundId = 77;
					peer.m_Config.localPeerId = 2; peer.m_Config.peerCount = 3;
					peer.m_Config.peerFrameGroups = variant != 7;
					peer.m_Config.migrationKey.fill(1);
					peer.m_FrameGroupChanges[1] = variant == 6 ? 6 : 4;
					if (variant != 6 && variant != 10) {
						NetLockstepCoordinator::PeerBridge own;
						own.fromFrame = own.aiFrame = 1; own.committedHold = true; own.members = 4;
						peer.m_PeerBridges[2] = own; peer.m_PeerFrameHolds[2][1] = true;
					}
					NetLockstepReadyFrame ready; ready.frame = 708; ready.localPeerId = 2;
					ready.authorityPeerId = ready.updateAuthorityPeerId = 3;
					ControllerFrame input; input.actorUniqueID = 1003; input.analogMoveX = 300;
					ready.remoteFrameCounts[3] = 1; ready.remoteFrames = {input};
					NetHash32 prepared{}; prepared.fill(9); peer.m_PeerPreparedPrefixes[ready.frame] = prepared;
					const auto digest = peer.CommittedFramePrefix(ready);
					auto witness = peer.PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
					witness.senderPeerId = witness.successorPeerId = 3;
					witness.frame = witness.appliedFrame = ready.frame; witness.preparedFrame = UINT64_MAX;
					witness.voterMask = 4;
					witness.bytes.assign(digest.begin(), digest.end()); witness.bytes.insert(witness.bytes.end(), prepared.begin(), prepared.end());
					if (variant == 2) witness.voterMask = 5;
					if (variant == 3) witness.bytes[0] ^= 1;
					if (variant == 4) witness.bytes[32] ^= 1;
					if (variant == 5) witness.senderPeerId = witness.successorPeerId = 1;
					if (variant == 8) witness.bytes.pop_back();
					if (variant == 9) ++witness.frame;
					if (variant != 1) peer.m_PeerFrameWitnesses[ready.frame][witness.senderPeerId] = witness;
					const uint8_t expected = variant == 0 ? 3 : variant == 6 || variant == 7 ? 2 : 0;
					const uint8_t observed = s_FrameAuthor(peer, ready);
					if (observed != expected) {
						error = "private frame provenance variant=" + std::to_string(variant) + " author=" + std::to_string(observed) + " expected=" + std::to_string(expected); return false;
					}
				}
				return true;
			}});
		}
		if (!selected || std::string_view(selected) == "transport_rtt_observes_the_injected_physical_jitter")
			named.push_back({"transport_rtt_observes_the_injected_physical_jitter", CheckTransportRttOracle});
		if (!selected || std::string_view(selected) == "host_hold_ai_authority_is_the_certified_frames")
			named.push_back({"host_hold_ai_authority_is_the_certified_frames", CheckHostHoldAuthority});
		if (!selected || std::string_view(selected) == "input_delay_uses_link_jitter_without_machine_receipt_cost") {
			named.push_back({"input_delay_uses_link_jitter_without_machine_receipt_cost", [](std::string& error) {
				struct Wire final : LoopbackTransport {
					uint32_t rtt = 0;
					uint32_t GetPeerPingMs(NetPeerId) const override { return rtt; }
					bool IsPeerPingMeasured(NetPeerId) const override { return true; }
					bool Send(NetPeerId, NetTransportLane, const std::vector<uint8_t>&, std::string* = nullptr, bool* = nullptr) override { return true; }
				};
				for (const uint32_t rtt: {30U, 200U}) for (const uint32_t receiptMs: {rtt + 30, rtt + 250}) {
					Wire wire; wire.rtt = rtt;
					NetLockstepCoordinator peer;
					peer.m_Config.sessionId = 0xB1D31A7;
					peer.m_Config.matchConfig = NetMatchConfigUtil::MakeDefault(peer.m_Config.sessionId);
					peer.m_Config.localPeerId = 2;
					peer.m_Config.authorityPeerId = 1;
					peer.m_Config.peerCount = 2;
					peer.m_Config.startFrame = 1;
					peer.m_Config.simTickMs = c_TestTickMs;
					peer.m_Config.migrationKey.fill(0x39);
					peer.m_Config.peerFrameGroups = peer.m_Config.adaptiveInputDelay = true;
					peer.m_Config.inputDelayFrames = peer.m_Config.matchConfig.inputDelayFrames = 1;
					peer.m_Config.peerInputDelayFrames = {{1, 1}, {2, 1}};
					peer.m_Config.matchConfig.peerInputDelayFrames = {1, 1};
					peer.m_Config.matchConfig.roundId = peer.m_RoundId = 77;
					peer.m_RoundConfigHash = NetMatchConfigUtil::HashConfig(peer.m_Config.matchConfig);
					peer.m_State = NetLockstepState::Running;
					peer.m_Transport = &wire;
					peer.m_RemotePeerIds = {1};
					peer.m_RemoteTransports = {{1, 42}};
					peer.m_LastCompletedSimulationTick = 4422;
					peer.m_Stats.nextFrame = 4423;
					peer.m_FrameGroupMembers = 3;
					peer.m_MigrationHistory[4422] = {1, 2, 3};
					peer.m_PeerReceiptDelaySamples[1].Observe(1000, receiptMs);
					peer.TickPeerInputDelay(1000);
					uint64_t proposed = 0;
					for (const auto& [key, votes]: peer.m_PeerBridgeVotes)
						if (key.second == 10 && votes.contains(2)) proposed = votes.at(2).completeFrom;
					DiagnosticLine() << "[session-plane-link-delay] rtt_ms=" << rtt << " receipt_ms=" << receiptMs << " proposed_ticks=" << proposed;
					if (proposed != (rtt == 30 ? 3U : 13U)) {
						error = "a machine's receipt cost changed a stable measured link's delay: rtt=" + std::to_string(rtt) +
						        " receipt=" + std::to_string(receiptMs) + " delay=" + std::to_string(proposed); return false;
					}
				}
				NetInputDelayEstimator jitter;
				for (uint64_t at = 1000; at < 6000; at += 100) jitter.Observe(at, at < 5500 || at == 5900 ? 30 : 45);
				if (jitter.SenderRequiredFrames(c_TestTickMs, 3) != 4) { error = "recent p95 link jitter did not add its input margin"; return false; }
				jitter.Observe(11000, 30);
				if (jitter.SenderRequiredFrames(c_TestTickMs, 3) != 3) { error = "expired link jitter still inflated the input delay"; return false; }
				for (uint64_t at = 11100; at < 13000; at += 100) jitter.Observe(at, 30);
				jitter.Observe(13000, 200);
				jitter.Observe(13100, 200);
				if (jitter.SenderRequiredFrames(c_TestTickMs, 3) != 13) { error = "a sustained RTT rise was also charged as jitter"; return false; }
				return true;
			}});
		}
		if (!selected || std::string_view(selected) == "directory_client_round_trip_allowances_and_refusals")
			named.push_back({"directory_client_round_trip_allowances_and_refusals", CheckDirectoryClient});
		if (!selected || std::string_view(selected) == "a_kept_hold_waits_for_its_own_directory_without_changing_history") {
			named.push_back({"a_kept_hold_waits_for_its_own_directory_without_changing_history", [](std::string& error) {
				// Old: a hold waits five seconds for this seat's own referee. New: both halves play on their local holds;
				// the delayed losing half alone stops when its reply arrives and adopts the ruling history through an image.
				Case row{"kept_hold_own_directory"}; row.seats = 4; row.subject = 3; row.gapMs = 8000;
				row.tie = row.internet = row.subjectDirectoryDark = row.secondHalfDirectoryDark = true;
				return CheckSplitContract(row, error);
			}});
		}
		if (!selected || std::string_view(selected) == "a_healed_tie_keeps_one_history_across_a_host_change") {
			named.push_back({"a_healed_tie_keeps_one_history_across_a_host_change", [](std::string& error) {
				Case row{"old_tie_host_change"}; row.seats = 4; row.subject = 1; row.gapMs = 20000; row.hung = true;
				row.internet = row.adminDirectory = row.tieBeforeAdministration = true; row.rttMs = row.otherRttMs = 100;
				Fixture fixture; const bool ok = fixture.Run(row, error);
				bool oldTie = false;
				for (const auto& [key, tie]: fixture.hub.directory.ties) oldTie |= std::get<0>(key) == 0 && !tie.winner.empty();
				const bool hostChanged = fixture.hub.adminDecision.state == NetHostChangeReply::State::Decided && fixture.hub.adminDecision.generation == 1;
				DiagnosticLine() << "[session-plane-old-tie] seed=0 old_tie=" << oldTie << " host_changes=" << hostChanged << std::endl;
				if (!ok) return false;
				if (!oldTie || !hostChanged) { error = "the healed split and the later host change did not both reach the real directory paths"; return false; }
				return true;
			}});
		}
		if (!selected || std::string_view(selected) == "repeated_host_changes_bound_the_tail_or_answer_missing") {
			RowRun run; run.name = "repeated_host_changes_bound_the_tail_or_answer_missing"; run.cost = 4000000;
			run.check = [](std::string& error) {
				const auto fail = [&](const std::string& detail) { error = "seed=0 " + detail; return false; };
				Case row{"tail_after_33_administrations"}; row.subject = 1; row.seats = 4; row.gapMs = 20000; row.hung = true;
				row.internet = row.adminDirectory = row.repeatedAdministration = row.imageReturn = true; row.rttMs = row.otherRttMs = 100;
				row.stepMs = 5;
				Fixture fixture; std::string detail;
				if (!fixture.Start(row, detail)) return fail(detail);
				std::string pastBound;
				size_t maxProofBound = 0;
				bool capacityAnswered = false;
				for (uint64_t generation = 1; generation <= 33; ++generation) {
					const uint8_t former = fixture.peers[0]->GetHostPeerId();
					fixture.hub.row.subject = former; fixture.hub.faultAt = fixture.hub.now + 1000; fixture.hub.faultEnd = fixture.hub.faultAt + 20000;
					fixture.hub.tailMissingSent = fixture.hub.tailMissingFrame = 0; fixture.hub.tailMissingDonor = 0;
					std::fill(fixture.maxWait.begin(), fixture.maxWait.end(), 0);
					bool returned = false;
					for (const uint64_t until = fixture.hub.faultEnd + 14000; fixture.hub.now < until;) {
						if (!fixture.Step(detail) || !fixture.CheckBound(detail)) return fail("admin generation " + std::to_string(generation) + ": " + detail);
						if (fixture.hub.now < fixture.hub.faultEnd + 1000) continue;
						returned = true;
						for (uint8_t seat = 1; seat <= 4; ++seat) {
							if (seat == former) continue;
							const auto& peer = *fixture.peers[seat - 1];
							returned &= peer.GetConfig().migrationGeneration == generation && peer.GetHostPeerId() == fixture.hub.directory.host;
						}
						const auto& reader = *fixture.peers[former - 1];
						if (returned && reader.IsStopped() && reader.GetStats().timeoutReason.starts_with("PeerHeld:") && fixture.hub.tailMissingDonor) {
							const auto& donor = *fixture.peers[fixture.hub.tailMissingDonor - 1];
							const uint64_t frame = fixture.hub.tailMissingFrame;
							std::map<uint8_t, NetLockstepCoordinator::PeerDecisionKey> latest;
							for (const auto& [key, decision]: donor.m_PeerBridgeCertificates) if (key.first <= frame) latest[key.second] = key;
							std::set<NetLockstepCoordinator::PeerDecisionKey> proofs;
							for (const auto& [kind, key]: latest) proofs.insert(key);
							for (const auto& [key, decision]: donor.m_PeerBridgeCertificates) if (key.second == 3) proofs.insert(key);
							for (const auto& [seat, bridge]: donor.m_PeerBridges) if (bridge.fromFrame <= frame)
								for (auto proof = donor.m_PeerBridgeCertificates.lower_bound({bridge.fromFrame, 0x10}); proof != donor.m_PeerBridgeCertificates.end() && proof->first.first == bridge.fromFrame; ++proof)
									if (NetPeerFrameDetail::IsHoldDecision(proof->first.second) && (proof->first.second & (1U << (seat - 1))) != 0) proofs.insert(proof->first);
							size_t count = 0;
							for (const auto& key: proofs) if (donor.m_PeerBridgeCertificates.contains(key) &&
							    (key.second == 3 || !donor.m_PeerAppliedThrough.contains(former) || key.first > donor.m_PeerAppliedThrough.at(former))) ++count;
							capacityAnswered = donor.m_MigrationHistory.contains(frame) && count > 32 && fixture.hub.tailMissingSent;
							std::cerr << "[session-plane-tail-capacity] seed=0 generation=" << generation << " certificates=" << count << " image_handoff=" << capacityAnswered << std::endl;
							if (!capacityAnswered) return fail("the reader needed an image before its retained tail exceeded 32 certificates");
						} else if (returned) {
							returned = reader.GetConfig().migrationGeneration == generation && reader.GetHostPeerId() == fixture.hub.directory.host;
							for (const auto& peer: fixture.peers) returned &= !peer->HasHeldAISeat(former) && !Bridges(*peer).contains(former);
						}
						if (returned) break;
					}
					if (!returned) return fail("admin generation " + std::to_string(generation) + " neither returned its reader nor handed an unservable certified tail to an image");
					for (const auto& peer: fixture.peers) {
						size_t administrative = 0; std::set<uint8_t> otherKinds;
						for (const auto& [key, decision]: peer->m_PeerBridgeCertificates) {
							if (key.second == 3) ++administrative; else otherKinds.insert(key.second);
						}
						maxProofBound = std::max(maxProofBound, administrative + otherKinds.size() + peer->m_PeerBridges.size());
					}
					std::cerr << "[session-plane-tail-capacity] seed=0 generation=" << generation << " proof_bound=" << maxProofBound << " image_handoff=" << capacityAnswered << std::endl;
					// FLAGGED fixture departure: a former host may still be a private reader during the next administration.
					// Retain raw waits, and use the same continuously enforced playing role and unchanged 50 ms bound here.
					for (uint8_t seat = 1; seat <= 4; ++seat) if (seat != former && fixture.playingMaxWait[seat - 1] > 50 && pastBound.empty())
						pastBound = "survivor " + std::to_string(seat) + " waited " + std::to_string(fixture.playingMaxWait[seat - 1]) + "ms at generation " + std::to_string(generation);
					if (capacityAnswered) return pastBound.empty() || fail(pastBound);
				}
				if (fixture.hub.adminHistory.size() != 33 || maxProofBound > 32)
					return fail("the retained tail's proof set grew past its certificate capacity without an image handoff");
				return pastBound.empty() || fail(pastBound);
			};
			named.push_back(std::move(run));
		}
		for (const auto& [name, seats, rtt]: std::vector<std::tuple<const char*, uint8_t, uint32_t>>{
		    {"image_return_three_seats_over_100ms_keeps_equal_histories_and_bound", 3, 100},
		    {"image_return_three_seats_over_250ms_keeps_equal_histories_and_bound", 3, 250},
		    {"image_return_four_seats_over_100ms_keeps_equal_histories_and_bound", 4, 100},
		    {"image_return_four_seats_over_200ms_keeps_equal_histories_and_bound", 4, 200}}) {
			if (selected && std::string_view(selected) != name) continue;
			named.push_back({name, [seats, rtt](std::string& error) { return CheckImageActivation(seats, rtt, error); }});
		}
		for (const auto& [name, shape]: std::vector<std::pair<const char*, const char*>>{
		    {"directory_decision_lands_after_the_final_query", "final"},
		    {"directory_refuses_stale_host_hash_generation_and_removed_owners", "refusals"},
		    {"directory_general_429_blocks_both_request_allowances", "throttle"},
		    {"directory_two_split_questions_wait_for_their_own_replies", "two-questions"},
		    {"directory_waiting_query_does_not_starve_a_later_check_in", "waiting-question"},
		    {"directory_healed_link_queries_before_the_old_exchange_times_out", "healed-query"},
		    {"directory_full_record_503_retries_without_frame_authority", "storage"}}) {
			if (selected && std::string_view(selected) != name) continue;
			named.push_back({name, [shape = std::string(shape)](std::string& error) { return CheckDirectoryDelayedReply(shape, error); }});
		}
		for (const auto& [name, shape, rtt]: std::vector<std::tuple<const char*, const char*, uint32_t>>{
		    {"image_return_at_round_rate_80_frames_behind_over_100ms", "edge", 100},
		    {"image_return_at_round_rate_80_frames_behind_over_200ms", "edge", 200},
		    {"image_return_at_round_rate_80_frames_behind_over_250ms", "edge", 250},
		    {"image_activation_reannouncement_preserves_closing_time", "closing", 250},
		    {"image_return_105_percent_aligns_inside_200ms_neutral_runway", "phase-runway", 200},
		    {"image_return_105_percent_aligns_inside_250ms_neutral_runway", "phase-runway", 250},
		    {"image_return_105_percent_keeps_the_neutral_interval_cap", "phase-over-cap", 250},
		    {"image_return_projection_never_exceeds_measured_capacity", "capacity-projection", 250},
		    {"two_image_returners_share_a_base_and_keep_separate_outcomes", "paired", 100},
		    {"benign_image_cancellations_do_not_exhaust_three_returns", "benign", 100}}) {
			if (selected && std::string_view(selected) != name) continue;
			named.push_back({name, [shape = std::string(shape), rtt](std::string& error) { return CheckPrivateReturnShape(shape, rtt, error); }});
		}
		for (const auto& row: rows) {
			if (selected && std::string_view(selected) != row.name) continue;
			RowRun run; run.name = row.name;
			run.cost = static_cast<uint64_t>(row.seats) * ((row.steadyMs ? row.steadyMs : row.gapMs + row.gapMs / 2) + 12000 + row.postReturnMs) *
			           (row.name == std::string("relay_link_blackout_return") ? 5 : row.name == std::string("partition_3_1") || row.name == std::string("host_absent_3s") ? 3 : 1);
			run.check = [row](std::string& error) {
				Fixture fixture; bool ok = row.splitContract ? CheckSplitContract(row, error) : fixture.Run(row, error);
				if (ok && row.name == std::string("relay_link_blackout_return")) ok = CheckRelayReturnAndDepartures(error);
				if (row.name == std::string("partition_3_1") || row.name == std::string("host_absent_3s"))
				    for (unsigned order = row.subject == 1 ? 3 : 1; order <= 3; ++order) {
					auto asymmetric = row; asymmetric.asymmetricBridge = order; asymmetric.rttMs = asymmetric.otherRttMs = 4;
					Fixture arm; std::string detail; const bool converged = arm.Run(asymmetric, detail);
					DiagnosticLine() << "[net-session-plane-selftest] bridge-input-race subject=" << static_cast<int>(row.subject) << " order=" << (order == 1 ? "input-first" : order == 2 ? "receipt-first" : "lost-forward")
					          << " two_pending_votes=" << arm.hub.asymmetricReleased << " dropped=" << arm.hub.asymmetricDropped << " resent=" << arm.hub.asymmetricResent
					          << " result=" << (converged ? "PASS" : "FAIL") << (detail.empty() ? "" : ": " + detail) << std::endl;
					if (!converged) { ok = false; if (error.empty()) error = detail; }
				}
				if (ok && row.name == std::string("two_joiner_3s_internet_and_direct")) { auto direct = row; direct.internet = false; Fixture arm; ok = arm.Run(direct, error); }
				return ok;
			};
			named.push_back(std::move(run));
		}
		ScheduleDriver driver; driver.Prepare(selected);
		std::vector<size_t> order(named.size());
		for (size_t index = 0; index < order.size(); ++index) order[index] = index;
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return named[a].cost > named[b].cost; });
		const size_t total = named.size() + driver.schedules.size();
		std::atomic<size_t> next{0};
		std::mutex doneMutex; std::condition_variable doneSignal;
		std::deque<size_t> finished;
		std::vector<std::thread> pool;
		for (size_t index = 0; index < driver.threads; ++index) pool.emplace_back([&] {
			t_PoolWorker = true;
			for (size_t item; (item = next++) < total;) {
				if (item < named.size()) {
					auto& run = named[order[item]];
					t_RowLines = &run.lines;
					try { run.ok = run.check(run.error); }
					catch (const std::exception& exception) { run.ok = false; run.error = "row raised an exception: " + std::string(exception.what()); }
					t_RowLines = nullptr;
					{ std::lock_guard lock(doneMutex); finished.push_back(order[item]); }
					doneSignal.notify_all();
				} else { t_QuietLines = !selected; driver.RunOne(item - named.size()); t_QuietLines = false; }
			}
		});
		for (size_t printed = 0; printed < named.size(); ++printed) {
			size_t index = 0;
			{ std::unique_lock lock(doneMutex); doneSignal.wait(lock, [&] { return !finished.empty(); }); index = finished.front(); finished.pop_front(); }
			auto& run = named[index];
			if (!run.lines.empty()) DiagnosticLine() << run.lines;
			DiagnosticLine() << "[net-session-plane-selftest] " << (run.ok ? "PASS " : "FAIL ") << run.name << (run.error.empty() ? "" : ": " + (run.ok || run.error.starts_with("seed=") ? run.error : "seed=0 " + run.error)) << std::endl;
			if (run.ok) ++passed; else ++failed;
		}
		for (auto& thread: pool) thread.join();
		driver.Finish();
		for (const auto& line: driver.lines) DiagnosticLine() << "[net-session-plane-selftest] " << line;
		for (const auto& [name, ok]: driver.fixedResults) { if (ok) ++passed; else ++failed; }
		if (driver.runRandom) {
			DiagnosticLine() << "[net-session-plane-selftest] random_hold_schedules schedules=" << driver.count << " ran=" << driver.ranCount.load() << " seeds=" << driver.firstSeed << ".." << driver.firstSeed + driver.count - 1
			                 << " failed=" << driver.failedCount << " threads=" << driver.threads << " elapsed_s=" << driver.elapsedMs / 1000
			                 << " saved_catalogue_reused=" << driver.reusedFixedCatalogue << driver.StartupWaits();
			const bool complete = driver.ranCount.load() == driver.count;
			DiagnosticLine() << "[net-session-plane-selftest] " << (driver.failedCount || !complete ? "FAIL" : "PASS") << " random_hold_schedules"
			                 << (!complete || driver.failedCount ? ": seeds=" + std::to_string(driver.firstSeed) + ".." + std::to_string(driver.firstSeed + driver.count - 1) + " " : "")
			                 << (!complete ? "did not run every requested schedule" : driver.failedCount ? std::to_string(driver.failedCount) + " of " + std::to_string(driver.count) + " schedules broke a property" : "");
			if (driver.failedCount || !complete) ++failed; else ++passed;
		}
		for (const auto& [name, scene]: std::vector<std::pair<const char*, const char*>>{
		    {"declared_leave_keeps_the_round_and_returns_on_its_seat", "leave-rejoin"},
		    {"two_seats_declared_leave_returns_through_a_private_reclaim", "leave-rejoin-two"},
		    {"two_seats_closed_joiner_after_input_does_not_hold_up_the_host", "drop-after-input"},
		    {"two_seats_closed_joiner_before_host_input_does_not_hold_up_the_host", "drop-before-host-input"},
		    {"two_seats_silent_joiner_before_host_input_does_not_hold_up_the_host", "silent-before-host-input"},
		    {"healthy_input_runway_spends_no_tie_allowance_before_a_real_cut", "referee-input-runway"},
		    {"a_rematch_starts_without_the_ended_rounds_catch_up", "rematch-chat"},
		    {"image_successor_capsule_sealing_refusal_is_retried", "capsule-retry"},
		    {"a_refused_image_activation_keeps_the_other_returners_service_clear", "paired-image-service"},
		    {"stored_ticket_redials_the_rematch_lobby_on_its_original_seat", "rematch-redial"}}) {
			if (selected && std::string_view(selected) != name) continue;
			std::string error; const bool ok = CheckSceneLifecycle(scene, &error);
			DiagnosticLine() << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << name << (error.empty() ? "" : ": seed=0 " + error);
			if (ok) ++passed; else ++failed;
		}
		// Service instances own process-wide replay hooks on destruction, so these checks follow the row pool.
		if (!selected || std::string_view(selected) == "in_place_private_reader_skips_the_committed_capture") {
			std::string error;
			const bool ok = [&]() {
				Case row{"private_reader_capture"}; row.subject = 2; row.seats = 3; row.gapMs = 6000;
				row.frozen = row.internet = row.captureFrames = true; row.rttMs = row.otherRttMs = 100; row.checkpointEveryFrames = 60;
				Fixture fixture;
				if (!fixture.Start(row, error)) return false;
				while (fixture.hub.now < 18000) {
					if (!fixture.Step(error)) return false;
					const uint64_t tick = fixture.worlds[1].applied;
					if (!tick || !fixture.peers[1]->IsLocalPrivateReaderAt(tick)) continue;
					const auto found = fixture.committed[1].find(tick);
					if (found == fixture.committed[1].end()) continue;
					std::vector<std::pair<uint8_t, NetGameCheckpoint>> notes;
					for (const auto* commands: {&found->second.localCommands, &found->second.remoteCommands}) for (const auto& command: *commands)
						if (const auto* note = std::get_if<NetGameCheckpoint>(&command.payload); note && note->kind == NetGameCheckpoint::Capture)
							notes.emplace_back(command.senderPeerId, *note);
					if (notes.empty()) continue;
					if (!MovableMan::IsConstructed()) MovableMan::Construct();
					if (!ActivityMan::IsConstructed()) ActivityMan::Construct();
					std::unique_ptr<Activity> savedActivity = std::make_unique<GameActivity>();
					savedActivity->SetActivityState(Activity::Running); g_ActivityMan.SwapCheckpointActivity(savedActivity);
					struct RestoreActivity { std::unique_ptr<Activity>& saved; ~RestoreActivity() { g_ActivityMan.SwapCheckpointActivity(saved); } } restoreActivity{savedActivity};
					NetMatchService service; service.m_IsHost = false; service.m_LocalPeerId = 2;
					service.m_MatchConfig = fixture.peers[1]->GetConfig().matchConfig;
					service.m_Coordinator = std::move(fixture.peers[1]); service.m_AutosaveMatchId = "fixture-private-reader";
					ScenarioRunner::SetLockstepCoordinator(service.m_Coordinator.get());
					if (!g_ActivityMan.ActivityRunning() || ScenarioRunner::WorldCatchUpActive() || !ScenarioRunner::IsLockstepControllerSyncActive()) {
						error = "the private-reader capture row did not reach the live tick-boundary path"; return false;
					}
					for (const auto& [sender, note]: notes) ScenarioRunner::NoteAppliedCheckpoint(sender, note);
					const auto before = service.m_Coordinator->m_LocalCaptureTick;
					const auto reports = service.m_Coordinator->m_CaptureParkReportsMs;
					service.AutosaveAtTickBoundary(tick, false);
					if (service.m_Coordinator->m_LocalCaptureTick != before || service.m_Coordinator->m_CaptureParkReportsMs != reports ||
					    !service.m_ScheduledCaptures.empty()) {
						error = "an in-place private reader took or parked for the capture it replayed"; return false;
					}
					return true;
				}
				error = "the real private reader did not replay a committed capture"; return false;
			}();
			DiagnosticLine() << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << "in_place_private_reader_skips_the_committed_capture" << (error.empty() ? "" : ": seed=0 " + error);
			if (ok) ++passed; else ++failed;
		}
		for (unsigned arm = 0; arm < 5; ++arm) {
			const char* name = arm == 0 ? "image_base_transient_refusal_answers_after_four_seconds" :
			                   arm == 1 ? "image_base_out_of_memory_answers_immediately" :
			                   arm == 2 ? "image_base_host_refusals_allow_fresh_attempts" :
			                   arm == 3 ? "held_host_still_serves_another_seats_image" : "slow_held_host_names_another_seats_base_through_the_playing_seats";
			if (selected && std::string_view(selected) != name) continue;
			std::string error;
			const bool ok = [&] {
				NetMatchService service; service.m_IsHost = true;
				auto match = NetMatchConfigUtil::MakeDefault(0xF004);
				match.peerCount = 4; match.players = {{1, 0, false, "Host"}, {2, 1, false, "Returner"}, {3, 2, false, "Witness"}, {4, 3, false, "Other"}};
				if (!service.m_WorldJoin.ConfigureMatchRejoins(match, 77, c_TestTickMs, &error)) return false;
				if (arm >= 3) {
					Case row{"held_host_image"}; row.subject = 1; row.seats = 4; row.gapMs = 15000; row.internet = row.captureFrames = true;
					row.rttMs = row.otherRttMs = arm == 3 ? 100 : 2;
					Schedule schedule; schedule.seats = 4; schedule.internet = true;
					for (uint8_t a = 1; a <= 4; ++a) for (uint8_t b = 1; b <= 4; ++b) if (a != b) schedule.rttMs[a][b] = row.rttMs;
					// The image seat leaves first; the host's machine then stops or slows while that other seat is already held.
					schedule.spells = {{2, Spell::Kind::Silent, 6000, 12000}, {1, arm == 3 ? Spell::Kind::Stopped : Spell::Kind::Slow, 9000, 12000, 50}};
					Fixture fixture;
					fixture.hub.schedule = &schedule;
					if (!fixture.Start(row, error)) return false;
					const auto stepRound = [&] {
						if (!fixture.Step(error)) return false;
						// Keepalives flow only to the seats whose own links are up, and reach a seat whose link comes back a heartbeat and a trip
						// later: until then its view is its own fault's.
						const auto hearsKeepalives = [&](uint8_t seat) {
							if (fixture.OwnTrouble(seat)) return false;
							for (const auto& spell: schedule.spells)
								if (spell.seat == seat && fixture.hub.now >= spell.EndMs() && fixture.hub.now < spell.EndMs() + c_NetPeerHeartbeatMs + 2 * row.rttMs) return false;
							return true;
						};
						if (arm == 3) for (const auto& peer: fixture.peers) if (hearsKeepalives(peer->GetConfig().localPeerId) && HoldVisible(*peer, 1, fixture.hub.now)) {
							error = "flowing keepalives showed a held host seat: observer=" + std::to_string(peer->GetConfig().localPeerId) + " slow=" +
							        std::to_string(peer->IsHeldAsSlowMachine(1)) + " at " + std::to_string(fixture.hub.now) + " ms"; return false;
						}
						return true;
					};
					while (fixture.hub.now < 24000 && (fixture.hub.now < 10000 ||
					       !fixture.peers[0]->IsOwnHostSeatHeld() || !fixture.peers[0]->HasHeldAISeat(2)))
						if (!stepRound()) return false;
					if (!fixture.peers[0]->IsOwnHostSeatHeld() || !fixture.peers[0]->HasHeldAISeat(2)) {
						error = "the image returner and host were not both held: host=" + std::to_string(fixture.peers[0]->IsOwnHostSeatHeld()) +
						        " image_seat=" + std::to_string(fixture.peers[0]->HasHeldAISeat(2)); return false;
					}
					if (!service.m_WorldJoin.ConfigureMatchRejoins(fixture.peers[0]->GetConfig().matchConfig, fixture.peers[0]->GetRoundId(), c_TestTickMs, &error)) return false;
					service.m_LocalPeerId = 1;
					// The fixture retains the coordinator; the service borrows it only while following its real capture schedule.
					service.m_Coordinator.reset(fixture.peers[0].get());
					struct ReleaseCoordinator { NetMatchService& service; ~ReleaseCoordinator() { (void)service.m_Coordinator.release(); } } releaseCoordinator{service};
					if (!service.m_WorldJoin.BeginRejoin(12, 1, 2, 2, "Returner", fixture.hub.now, &error)) return false;
					service.m_WorldCapturePending = service.m_PrivateBaseRequested = true;
					NetMatchService::AutosaveTickInput input; input.tick = fixture.worlds[0].applied; input.localPeer = 1;
					input.writers = service.CheckpointWriters(input.tick); input.lead = 60; input.ownSeatHeld = true;
					const auto output = service.StepAutosaveSchedule(input);
					if (!output.requestPrivateBase || !fixture.peers[0]->RequestPrivateBaseCapture()) {
						error = "the held host did not ask the playing seats for the base another seat needs"; return false;
					}
					uint64_t observed = input.tick, captureFrame = 0, captureTick = 0;
					bool captured = false;
					while (fixture.hub.now < fixture.hub.faultEnd + 12000) {
						if (!stepRound()) return false;
						for (auto at = fixture.committed[0].upper_bound(observed); at != fixture.committed[0].end(); ++at) {
							const auto& ready = at->second;
							input = {}; input.tick = ready.frame; input.localPeer = 1; input.lead = 60;
							input.writers = service.CheckpointWriters(input.tick);
							input.ownSeatHeld = fixture.peers[0]->IsSeatUnderAI(1, ready.frame);
							for (const auto* commands: {&ready.localCommands, &ready.remoteCommands}) for (const auto& command: *commands)
								if (const auto* note = std::get_if<NetGameCheckpoint>(&command.payload)) {
									input.applied.push_back({command.senderPeerId, note->kind, note->tick});
									if (command.senderPeerId == 1 && note->kind == NetGameCheckpoint::Capture) {
										if (!input.ownSeatHeld) { error = "the requested base was named only after the host returned"; return false; }
										if (captureTick && captureTick != note->tick) { error = "one held-host request named two base ticks"; return false; }
										captureFrame = ready.frame; captureTick = note->tick;
									}
								}
							const auto step = service.StepAutosaveSchedule(input);
							if (step.requestPrivateBase && !fixture.peers[0]->RequestPrivateBaseCapture()) { error = "the held host lost its base request"; return false; }
							if (step.capture && input.tick == captureTick) captured = true;
							observed = ready.frame;
						}
						if (!captureTick || !captured) continue;
						bool allCommitted = true;
						for (size_t index = 0; index < fixture.committed.size(); ++index) {
							if (index == 1) continue; // The image returner receives this state in its base, not on its old connection.
							const auto& history = fixture.committed[index];
							const auto at = history.find(captureFrame);
							if (at == history.end() || !fixture.worlds[index].hashes.contains(captureTick)) { allCommitted = false; break; }
							bool named = false;
							for (const auto* commands: {&at->second.localCommands, &at->second.remoteCommands}) for (const auto& command: *commands)
								if (const auto* note = std::get_if<NetGameCheckpoint>(&command.payload); note && command.senderPeerId == 1 && note->kind == NetGameCheckpoint::Capture && note->tick == captureTick) named = true;
							if (!named) { error = "the playing seats committed different held-host base schedules"; return false; }
							if (fixture.worlds[index].hashes.at(captureTick) != fixture.worlds[0].hashes.at(captureTick)) {
								error = "the held host and playing seats have different image-capture histories"; return false;
							}
						}
						if (allCommitted && service.m_OpenCaptureApplied && service.IsJoinCaptureTick(captureTick)) {
							if (!fixture.wronglyHeld.empty()) { error = fixture.wronglyHeld; return false; }
							for (uint8_t seat: {3, 4}) if (fixture.maxWait[seat - 1] > static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs))) {
								error = "playing seat " + std::to_string(seat) + " waited " + std::to_string(fixture.maxWait[seat - 1]) + "ms during the held-host base request"; return false;
							}
							if (const auto twoReturns = fixture.TwoReturnFrames(); !twoReturns.empty()) { error = twoReturns; return false; }
							return true;
						}
					}
					error = "the playing seats did not commit and apply the held host's requested base"; return false;
				}
				for (unsigned attempt = 0; attempt < (arm == 2 ? 6U : 1U); ++attempt) {
					const uint64_t opened = 1000 + attempt * 5000;
					if (!service.m_WorldJoin.BeginRejoin(12, 1, 2, 2 + attempt, "Returner", opened, &error)) return false;
					service.m_WorldCapturePending = service.m_PrivateBaseRequested = true;
					service.NotePrivateBaseRefusalLocked({arm == 1 ? "out of memory" : "capture writer refused the base", arm == 1});
					service.AnswerStalledReturnersLocked(opened + (arm == 1 ? 1 : 4000));
					if (arm != 1 && !service.m_WorldJoin.FindSession(12)) { error = "a transient base refusal cancelled a return before its stage window"; return false; }
					service.AnswerStalledReturnersLocked(opened + (arm == 1 ? 2 : 4001));
					if (service.m_WorldJoin.FindSession(12) || service.m_WorldJoin.TakeCancelledJoinCauses() != std::vector<std::pair<uint8_t, bool>>{{2, false}} ||
					    service.m_WorldCapturePending || service.m_PrivateBaseRequested || !service.m_PrivateBaseRefusal.empty()) {
						error = "a host-side base refusal left a pending return or charged the returner"; return false;
					}
				}
				return true;
			}();
			DiagnosticLine() << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << name << (error.empty() ? "" : ": seed=0 " + error);
			if (ok) ++passed; else ++failed;
		}
		if (!selected || std::string_view(selected) == "image_replay_12ms_tick_10ms_draw_closes_its_trail") {
			std::string error; const bool ok = CheckReplayConvergence(error);
			DiagnosticLine() << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << "image_replay_12ms_tick_10ms_draw_closes_its_trail" << (error.empty() ? "" : ": " + error);
			if (ok) ++passed; else ++failed;
		}
		for (unsigned arm = 0; arm < 3; ++arm) {
			const char* name = arm == 0 ? "successor_unvalidated_boundary_refused" : arm == 1 ? "leave_during_handover_exits" : "malformed_traffic_does_not_renew_authority";
			if (selected && std::string_view(selected) != name) continue;
			std::string error; const bool ok = NetLockstepSelfTest::CheckSessionRecoveryGuard(arm, &error);
			DiagnosticLine() << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << name << (error.empty() ? "" : ": " + error) << std::endl;
			if (ok) ++passed; else ++failed;
		}
		DiagnosticLine() << "[net-session-plane-selftest] " << passed << " of " << passed + failed << " passed" << std::endl;
		DiagnosticLine() << "[net-session-plane-selftest] " << (failed ? "FAIL" : "PASS") << std::endl;
		return failed || passed == 0 ? 1 : 0;
	}
}
