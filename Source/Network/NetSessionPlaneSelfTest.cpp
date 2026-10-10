#include "NetSessionPlaneSelfTest.h"

#include "NetLockstep.h"
#include "NetLockstepSelfTest.h"
#include "NetIdentity.h"
#include "TimerMan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace RTE {
	namespace {
		constexpr double c_TestTickMs = 1000.0 / 60;
		constexpr uint64_t c_TestContinuityMs = 2000, c_TestReleaseMs = 250, c_TestReturnAllowanceMs = 2000;
		constexpr uint32_t c_TestBoundTicks = 3, c_TestCatchUpRate = 3;
		struct Case {
			const char* name;
			uint8_t subject = 2, seats = 4;
			uint64_t gapMs = 0;
			bool frozen = false, partition = false, tie = false, internet = false, directoryDown = false, directoryBoth = false, blackout = false;
			uint32_t rttMs = 2, otherRttMs = 2, lossPercent = 0;
			uint64_t steadyMs = 0;
			bool measurePace = false;
		};
		struct Wire;
		struct Hub {
			uint64_t now = 0, faultAt = 6000, faultEnd = 6000;
			Case row;
			std::map<uint16_t, Wire*> listeners;
			std::set<Wire*> wires;
			bool DuringFault() const { return now >= faultAt && now < faultEnd; }
			bool Split(uint8_t a, uint8_t b) const {
				if (!DuringFault() || row.frozen || row.blackout || !row.gapMs) return false;
				if (row.tie) return (a <= 2) != (b <= 2);
				return a == row.subject || b == row.subject;
			}
			uint64_t Delay(uint8_t a, uint8_t b) const { return ((a == row.subject || b == row.subject ? row.rttMs : row.otherRttMs) + 1) / 2; }
			bool DirectoryReachable(uint8_t peer) const {
				if (!DuringFault()) return true;
				if (row.directoryDown) return false;
				return row.directoryBoth || peer != row.subject;
			}
			struct Tie { uint64_t at = 0; std::vector<std::vector<uint16_t>> confirmed; std::vector<uint16_t> winner; };
			std::map<uint64_t, Tie> ties;
		};
		struct Wire final : INetTransport {
			Hub& hub;
			uint8_t owner;
			uint16_t port = 0;
			uint32_t next = 1, sends = 0;
			std::map<NetPeerId, std::pair<Wire*, NetPeerId>> connections;
			struct Event { uint64_t at; NetTransportEvent value; };
			std::vector<Event> events;
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
				const NetPeerId local = next++, remote = other->next++;
				connections[local] = {other, remote}; other->connections[remote] = {this, local};
				events.push_back({hub.now, {NetTransportEventType::PeerConnected, local, NetTransportLane::ControlReliable, {}, {}}});
				other->events.push_back({hub.now, {NetTransportEventType::PeerConnected, remote, NetTransportLane::ControlReliable, {}, {}}}); return true;
			}
			bool Send(NetPeerId id, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override {
				if (congested) *congested = false;
				const auto found = connections.find(id);
				if (found == connections.end()) { if (error) *error = "the fixture connection is absent"; return false; }
				auto [other, remote] = found->second;
				if (hub.Split(owner, other->owner)) return true;
				const uint32_t ordinal = ++sends;
				if (lane == NetTransportLane::InputUnreliable && hub.DuringFault() && hub.row.lossPercent &&
				    (owner == hub.row.subject || other->owner == hub.row.subject) && ordinal % 100 < hub.row.lossPercent) return true;
				other->events.push_back({hub.now + hub.Delay(owner, other->owner), {NetTransportEventType::PacketReceived, remote, lane, bytes, {}}}); return true;
			}
			void Disconnect(NetPeerId id, const std::string& reason) override {
				const auto found = connections.find(id); if (found == connections.end()) return;
				auto [other, remote] = found->second; connections.erase(found); other->connections.erase(remote);
				other->events.push_back({hub.now, {NetTransportEventType::PeerDisconnected, remote, NetTransportLane::ControlReliable, {}, reason}});
			}
			void Stop() override { while (!connections.empty()) Disconnect(connections.begin()->first, "fixture closes"); if (port && hub.listeners[port] == this) hub.listeners.erase(port); port = 0; events.clear(); }
			std::vector<NetTransportEvent> PollEvents() override {
				std::vector<NetTransportEvent> ready;
				for (auto it = events.begin(); it != events.end();) {
					if (it->at > hub.now) { ++it; continue; }
					if (!hub.Split(owner, connections.contains(it->value.peerId) ? connections.at(it->value.peerId).first->owner : owner)) ready.push_back(std::move(it->value));
					it = events.erase(it);
				}
				return ready;
			}
			uint32_t GetPeerPingMs(NetPeerId id) const override { return connections.contains(id) ? static_cast<uint32_t>(2 * hub.Delay(owner, connections.at(id).first->owner)) : 0; }
			bool IsPeerPingMeasured(NetPeerId id) const override { return connections.contains(id); }
		};
		struct World {
			uint64_t applied = 0;
			std::map<int64_t, int64_t> positions;
			std::map<uint64_t, NetHash32> hashes;
			bool Apply(const NetLockstepReadyFrame& ready, std::string& error) {
				if (ready.frame != applied + 1) { error = "a displayed frame was skipped or undone"; return false; }
				for (const auto* inputs: {&ready.localFrames, &ready.remoteFrames}) for (const auto& input: *inputs) positions[input.actorUniqueID] += input.analogMoveX;
				applied = ready.frame;
				std::string state = std::to_string(applied);
				for (const auto& [actor, position]: positions) state += ":" + std::to_string(actor) + ":" + std::to_string(position);
				hashes[applied] = NetIdentity::HashCanonicalText("session-plane-fixture", {{"state", state}}); return true;
			}
		};
		template<class Config> void FrameGroupConfig(Config& config, Hub& hub, uint8_t peer) {
			if constexpr (requires { config.peerFrameGroups; config.frameTieReferee; }) {
				config.peerFrameGroups = true;
				using Reply = typename std::decay_t<decltype(config.frameTieReferee)>::result_type;
				if (hub.row.internet) config.frameTieReferee = [&hub, peer](const auto& request) {
					Reply reply; reply.state = Reply::State::Waiting;
					if (!hub.DirectoryReachable(peer)) return reply;
					auto& tie = hub.ties[request.frame];
					if (!tie.at) tie.at = hub.now;
					if (!request.queryOnly && tie.winner.empty() && std::find(tie.confirmed.begin(), tie.confirmed.end(), request.members) == tie.confirmed.end()) tie.confirmed.push_back(request.members);
					if (tie.winner.empty() && hub.now >= tie.at + 20 && !tie.confirmed.empty()) {
						tie.winner = tie.confirmed.front();
						if (tie.confirmed.size() > 1) for (const auto& group: tie.confirmed) if (std::find(group.begin(), group.end(), request.host) != group.end()) tie.winner = group;
					}
					if (!tie.winner.empty()) { reply.state = Reply::State::Decided; reply.members = tie.winner; } return reply;
				};
			}
		}
		template<class Peer> bool CatchingUp(const Peer& peer) { if constexpr (requires { peer.PeerFrameCatchUpActive(); }) return peer.PeerFrameCatchUpActive(); else return false; }
		template<class Peer> uint32_t SenderResizes(const Peer& peer, uint8_t sender) {
			if constexpr (requires { peer.GetStats().peers.at(sender).delayResizes; }) return peer.GetStats().peers.at(sender).delayResizes;
			else return peer.GetStats().delayChangesCommitted;
		}
		template<class Peer> void Blackout(Peer& peer, uint64_t frame, uint64_t ms) { if constexpr (requires { peer.SetPeerFrameBlackoutForTest(frame, ms); }) peer.SetPeerFrameBlackoutForTest(frame, ms); }
		template<class Peer> constexpr bool HasBlackout() { return requires(Peer& peer) { peer.SetPeerFrameBlackoutForTest(1, 1); }; }
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
		struct Fixture {
			Hub hub;
			std::vector<std::unique_ptr<Wire>> wires;
			std::vector<std::unique_ptr<NetLockstepCoordinator>> peers;
			std::vector<World> worlds;
			std::vector<double> credits;
			std::vector<uint64_t> queued, waitAt, maxWait, caughtAt;
			std::vector<uint16_t> settledDelay;
			std::vector<uint32_t> settledChanges, settledHostWaits;
			std::vector<uint64_t> paceBegin, paceEnd;
			std::map<uint8_t, uint64_t> continuityFrom;
			uint64_t substitutes = 0, continuityFrames = 0, aiFrames = 0, hostChanges = 0, firstLostPressMs = UINT64_MAX;
			uint64_t shortGapSubstitutes = 0;
			bool wasFrozen = false;
			bool Start(const Case& row, std::string& error) {
				hub.row = row; hub.faultEnd = hub.faultAt + (row.steadyMs ? row.steadyMs : row.gapMs);
				worlds.resize(row.seats); credits.resize(row.seats); queued.resize(row.seats); waitAt.resize(row.seats);
				maxWait.resize(row.seats); caughtAt.resize(row.seats); settledDelay.resize(row.seats); settledChanges.resize(row.seats);
				settledHostWaits.resize(row.seats); paceBegin.resize(row.seats); paceEnd.resize(row.seats);
				for (uint8_t peer = 1; peer <= row.seats; ++peer) { wires.push_back(std::make_unique<Wire>(hub, peer)); peers.push_back(std::make_unique<NetLockstepCoordinator>()); }
				if (!wires[0]->StartHost(47901, &error)) return false;
				for (uint8_t peer = 2; peer <= row.seats; ++peer) if (!wires[peer - 1]->Connect("fixture", 47901, &error)) return false;
				auto match = NetMatchConfigUtil::MakeDefault(0xF001);
				match.peerCount = row.seats; match.players.clear(); match.successorOrder.clear(); match.migrationPeers.clear();
				match.inputDelayFrames = 1; match.peerInputDelayFrames.assign(row.seats, 1); match.roundId = 77;
				for (uint8_t peer = 1; peer <= row.seats; ++peer) {
					uint32_t rtt = 0;
					for (uint8_t other = 1; other <= row.seats; ++other) if (peer != other) rtt = std::max<uint32_t>(rtt, static_cast<uint32_t>(2 * hub.Delay(peer, other)));
					NetInputDelayEstimator estimator; estimator.Observe(0, rtt);
					match.peerInputDelayFrames[peer - 1] = InitialSenderDelay(estimator, peer == 1, match.inputDelayFrames);
				}
				for (uint8_t peer = 1; peer <= row.seats; ++peer) {
					match.players.push_back({peer, static_cast<uint8_t>(peer - 1), false, "Player"});
					if (peer != 1) match.successorOrder.push_back(peer);
					match.migrationPeers.push_back({peer, static_cast<uint16_t>(47900 + peer), {"fixture"}});
				}
				for (uint8_t peer = 1; peer <= row.seats; ++peer) {
					NetLockstepConfig config; config.sessionId = match.sessionId; config.matchConfig = match;
					config.localPeerId = peer; config.peerCount = row.seats; config.roundId = peer == 1 ? 77 : 0;
					config.startFrame = 1; config.inputDelayFrames = match.peerInputDelayFrames[peer - 1]; config.timeoutMs = 60000;
					config.frameLane = NetTransportLane::InputUnreliable; config.substituteSlowPeers = true; config.adaptiveInputDelay = true;
					config.slowPlayerBoundTicks = c_TestBoundTicks; config.simTickMs = c_TestTickMs; config.relayToOtherPeers = peer == 1;
					config.migrationKey.fill(0x39);
					for (uint8_t other = 1; other <= row.seats; ++other) { config.peerInputDelayFrames[other] = match.peerInputDelayFrames[other - 1]; config.peerIncarnations[other] = 1; }
					if (peer == 1) for (uint8_t other = 2; other <= row.seats; ++other) config.remoteTransportPeerIds[other] = other - 1;
					else config.remoteTransportPeerIds[1] = 1;
					config.migrationTransportFactory = [&hub = hub, peer] { return std::make_unique<Wire>(hub, peer); };
					FrameGroupConfig(config, hub, peer);
					if (!peers[peer - 1]->Start(*wires[peer - 1], config, &error)) return false;
					peers[peer - 1]->DeferStopsToTickBoundary();
				}
				return true;
			}
			bool IsAffected(uint8_t peer) const {
				if (hub.row.internet && hub.row.directoryDown) return true;
				if (hub.row.seats == 2 && !hub.row.internet) return peer != 1;
				return hub.row.tie ? peer >= 3 : peer == hub.row.subject;
			}
			bool CheckCommittedInputs(const NetLockstepReadyFrame& ready, uint8_t observer, std::string& error) {
				if (IsAffected(observer) || !hub.row.gapMs) return true;
				const uint8_t subject = hub.row.seats == 2 && !hub.row.internet ? 2 : hub.row.subject;
				const auto bridge = Bridges(*peers[observer - 1]);
				if (!bridge.contains(subject)) return true;
				if (peers[observer - 1]->IsSeatUnderAI(subject, ready.frame)) { ++aiFrames; ++substitutes; return true; }
				std::vector<ControllerFrame> frames;
				if (ready.localPeerId == subject) frames = ready.localFrames;
				else {
					size_t offset = 0;
					for (const auto& [peer, count]: ready.remoteFrameCounts) { if (peer == subject) frames.assign(ready.remoteFrames.begin() + offset, ready.remoteFrames.begin() + offset + count); offset += count; }
				}
				if (frames.empty()) return true;
				const auto [first, inserted] = continuityFrom.try_emplace(observer, ready.frame);
				const uint64_t elapsed = static_cast<uint64_t>(std::llround((ready.frame - first->second) * c_TestTickMs));
				for (const auto& frame: frames) {
					if (frame.analogMoveX != subject * 100 || frame.mouseDeltaX || frame.mouseDeltaY) { error = "the short substitute changed movement or repeated a delta"; return false; }
					if (elapsed >= c_TestReleaseMs && (frame.stateMask & ((1ULL << WEAPON_FIRE) | (1ULL << BODY_JUMP)))) { error = "a held fire or jump continued past its release bound"; return false; }
				}
				if (elapsed > c_TestContinuityMs) { error = "continuity continued past the AI takeover constant"; return false; }
				++continuityFrames; ++substitutes;
				firstLostPressMs = std::min(firstLostPressMs, hub.now >= hub.faultAt ? hub.now - hub.faultAt : 0);
				if (hub.row.gapMs == 150) ++shortGapSubstitutes;
				return true;
			}
			bool Step(std::string& error) {
				for (uint8_t id = 1; id <= hub.row.seats; ++id) {
					if (hub.row.frozen && id == hub.row.subject && hub.DuringFault()) peers[id - 1]->PlaneTick(hub.now);
					else peers[id - 1]->Tick(hub.now);
				}
				if (hub.row.blackout && hub.now == hub.faultAt) {
					Blackout(*peers[hub.row.subject - 1], worlds[hub.row.subject - 1].applied, hub.row.gapMs);
					if (!HasBlackout<NetLockstepCoordinator>()) hub.row.blackout = false;
				}
				for (uint8_t id = 1; id <= hub.row.seats; ++id) {
					auto& peer = *peers[id - 1]; auto& world = worlds[id - 1];
					if (peer.IsFailed()) { error = "peer " + std::to_string(id) + " failed: " + peer.GetStats().timeoutReason; return false; }
					if (peer.GetHostPeerId() != 1 && (!hub.row.gapMs || hub.row.subject != 1 || hub.row.gapMs < 15000)) { error = "lag changed the lobby host"; return false; }
					if (peer.IsStopped()) { error = "lag ended participation or required a manual rejoin"; return false; }
					if (!peer.IsRunning()) continue;
					if (hub.row.frozen && id == hub.row.subject && hub.DuringFault()) { credits[id - 1] = 0; wasFrozen = true; continue; }
					credits[id - 1] += (CatchingUp(peer) || (wasFrozen && id == hub.row.subject && hub.now >= hub.faultEnd) ? c_TestCatchUpRate : 1) / c_TestTickMs;
					credits[id - 1] = std::min(credits[id - 1], 6.0);
					while (credits[id - 1] >= 1) {
						const uint64_t next = world.applied + 1;
						if (queued[id - 1] != next && !peer.TimingDecisionPendingAt(next)) {
							ControllerFrame input; input.actorUniqueID = 1000 + id; input.analogMoveX = id * 100;
							input.stateMask = (1ULL << MOVE_RIGHT) | (1ULL << WEAPON_FIRE) | (1ULL << BODY_JUMP);
							if (!peer.DeferLocalInput(next, {input}) && !peer.QueueLocalInput(next, {input}, {}, &error)) return false;
							queued[id - 1] = next;
						}
						NetLockstepReadyFrame ready;
						if (next < peer.GetStats().effectiveStartFrame) { ready.frame = next; ready.localPeerId = id; }
						else if (!peer.PopReadyFrame(ready)) {
							if (!waitAt[id - 1]) waitAt[id - 1] = hub.now;
							peer.NoteFrameWait(next, hub.now); break;
						}
						if (waitAt[id - 1]) { maxWait[id - 1] = std::max(maxWait[id - 1], hub.now - waitAt[id - 1]); waitAt[id - 1] = 0; }
						peer.FinishFrameWait(hub.now);
						if (hub.row.internet && hub.row.directoryDown && hub.DuringFault() && !Bridges(peer).empty()) {
							error = "an unconfirmed internet side committed a substitute during the directory outage"; return false;
						}
						if (!CheckCommittedInputs(ready, id, error) || !world.Apply(ready, error)) return false;
						peer.FinishSimulationTick(ready.frame); credits[id - 1] -= 1;
					}
					if (hub.now < hub.faultEnd && hub.row.gapMs < 5000 && HoldVisible(peer, hub.row.subject, hub.now)) { error = "a short hiccup showed a held seat"; return false; }
					if (hub.row.frozen && HoldVisible(peer, hub.row.subject, hub.now)) { error = "flowing keepalives showed a held seat"; return false; }
					if (hub.now == hub.faultAt - 1) {
						settledDelay[id - 1] = peer.InputDelayAt(id, world.applied + 1); settledChanges[id - 1] = SenderResizes(peer, id);
						settledHostWaits[id - 1] = peer.GetStats().peers.contains(1) ? peer.GetStats().peers.at(1).waits : 0;
						paceBegin[id - 1] = world.applied;
					}
					if (hub.now == hub.faultEnd - 1) paceEnd[id - 1] = world.applied;
				}
				const uint64_t common = std::min_element(worlds.begin(), worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied;
				if (common) {
					for (const auto& world: worlds) if (world.hashes.at(common) != worlds[0].hashes.at(common)) { error = "return hashes differ at displayed frame " + std::to_string(common); return false; }
				}
				if (hub.now >= hub.faultEnd) {
					const uint64_t highest = std::max_element(worlds.begin(), worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied;
					for (uint8_t id = 1; id <= hub.row.seats; ++id) if (!caughtAt[id - 1] && worlds[id - 1].applied + 1 >= highest && common > 0 &&
					    !peers[id - 1]->HasHeldAISeat(hub.row.subject) && !Bridges(*peers[id - 1]).contains(hub.row.subject)) caughtAt[id - 1] = hub.now;
				}
				++hub.now; return true;
			}
			bool Run(const Case& row, std::string& error) {
				if (!Start(row, error)) return false;
				const uint64_t end = hub.faultEnd + (row.gapMs > 5000 ? row.gapMs / 2 : 0) + c_TestReturnAllowanceMs + 4000;
				while (hub.now <= end) if (!Step(error)) return false;
				for (uint8_t id = 1; id <= row.seats; ++id) {
					const bool doubleFailure = row.internet && row.directoryDown;
					if (!IsAffected(id) && !doubleFailure && maxWait[id - 1] > (row.internet && row.seats == 2 ? 1000 : static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs)))) {
						error = "non-lagging peer " + std::to_string(id) + " stalled " + std::to_string(maxWait[id - 1]) + "ms past its bound"; return false;
					}
					if (!caughtAt[id - 1]) { error = "a retained seat did not return automatically"; return false; }
					const uint8_t held = row.seats == 2 && !row.internet ? 2 : row.subject;
					if (peers[id - 1]->HasHeldAISeat(held) || Bridges(*peers[id - 1]).contains(held)) { error = "the returning human seat stayed bridged after catch-up"; return false; }
					if (row.gapMs <= 3000 && caughtAt[id - 1] > hub.faultEnd + c_TestReturnAllowanceMs) { error = "return exceeded the spike plus its catch-up allowance"; return false; }
				}
				if (row.gapMs == 150 && shortGapSubstitutes) { error = "a spike inside the input delay committed a substitute"; return false; }
				if (row.gapMs == 150 && maxWait[row.subject - 1]) { error = "a spike inside the input delay hitched the spiking player's view"; return false; }
				if (row.gapMs >= 400 && row.gapMs < c_TestContinuityMs && !continuityFrames) { error = "a short gap did not use its continuity stage"; return false; }
				if (row.gapMs >= 3000 && !row.directoryDown && (!continuityFrames || !aiFrames)) { error = "the committed tail did not contain both continuity and AI stages"; return false; }
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
				std::cout << "[session-plane-measure] name=" << row.name << " gap_ms=" << row.gapMs << " own_max_hitch_ms=" << maxWait[row.subject - 1]
				          << " max_wait_ms=" << greatestWait << " catch_up_ms=" << caughtAt[row.subject - 1] - hub.faultEnd
				          << " held_inputs=" << continuityFrames << " ai_inputs=" << aiFrames << " first_discarded_press_ms="
				          << (firstLostPressMs == UINT64_MAX ? -1 : static_cast<int64_t>(firstLostPressMs)) << " hashes=equal undone=0" << std::endl;
				return true;
			}
		};
	}

	int NetSessionPlaneSelfTest::Run() {
		if (!TimerMan::IsConstructed()) TimerMan::Construct();
		std::vector<Case> rows;
		const auto gap = [&](const char* name, uint8_t subject, uint64_t ms) { Case row{name}; row.subject = subject; row.gapMs = ms; return row; };
		rows.push_back(gap("peer_returns_2s", 2, 2000)); rows.push_back(gap("peer_returns_8s", 2, 8000)); rows.push_back(gap("peer_returns_40s", 2, 40000));
		auto frozen = gap("peer_frozen_10s_keepalives", 2, 10000); frozen.frozen = true; rows.push_back(frozen);
		rows.push_back(gap("host_absent_3s", 1, 3000)); rows.push_back(gap("host_absent_20s", 1, 20000));
		auto majority = gap("partition_3_1", 4, 3000); majority.partition = true; rows.push_back(majority);
		auto tie = gap("partition_2_2_host_tie", 3, 3000); tie.tie = tie.internet = tie.directoryBoth = true; rows.push_back(tie);
		auto two = gap("two_direct_host_3s", 1, 3000); two.seats = 2; rows.push_back(two);
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
		auto directHost = gap("two_host_3s_no_directory_configured", 1, 3000); directHost.seats = 2; rows.push_back(directHost);
		auto joiner = gap("two_joiner_3s_internet_and_direct", 2, 3000); joiner.seats = 2; joiner.internet = true; rows.push_back(joiner);
		auto both = gap("two_internet_host_and_directory_3s", 1, 3000); both.seats = 2; both.internet = both.directoryDown = true; rows.push_back(both);
		int passed = 0, failed = 0;
		std::cout << "[net-session-plane-selftest] topology=single-box proof=false forced_rows=29" << std::endl;
		for (const auto& row: rows) {
			std::string error; Fixture fixture; bool ok = fixture.Run(row, error);
			if (ok && row.name == std::string("two_joiner_3s_internet_and_direct")) { auto direct = row; direct.internet = false; Fixture arm; ok = arm.Run(direct, error); }
			std::cout << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << row.name << (error.empty() ? "" : ": " + error) << std::endl;
			if (ok) ++passed; else ++failed;
		}
		for (unsigned arm = 0; arm < 3; ++arm) {
			const char* name = arm == 0 ? "successor_unvalidated_boundary_refused" : arm == 1 ? "leave_during_handover_exits" : "malformed_traffic_does_not_renew_authority";
			std::string error; const bool ok = NetLockstepSelfTest::CheckSessionRecoveryGuard(arm, &error);
			std::cout << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << name << (error.empty() ? "" : ": " + error) << std::endl;
			if (ok) ++passed; else ++failed;
		}
		std::cout << "[net-session-plane-selftest] " << passed << " of " << passed + failed << " passed" << std::endl;
		std::cout << "[net-session-plane-selftest] " << (failed ? "FAIL" : "PASS") << std::endl;
		return failed ? 1 : 0;
	}
}
