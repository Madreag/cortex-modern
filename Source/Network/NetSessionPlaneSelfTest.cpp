#include "NetSessionPlaneSelfTest.h"

#include "NetLockstep.h"
#include "NetLockstepSelfTest.h"
#include "NetMatchService.h"
#if __has_include("NetPeerSessionWire.h")
#include "NetPeerSessionWire.h"
#endif
#include "NetIdentity.h"
#include "NetLobbySession.h"
#include "NetMuxTransport.h"
#include "NetProtocol.h"
#include "TimerMan.h"
#include "lz4.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <variant>
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
			bool captureFrames = false, realFrozen = false;
			bool adminDirectory = false, adminDirectoryDown = false;
			unsigned asymmetricBridge = 0;
			uint32_t planeIntervalMs = 1, recoveryRttMs = 0;
			bool delayStartup = false;
			bool receiptGap = false;
			uint64_t postReturnMs = 0;
			bool laterSessionMessageFirst = false;
			// The joiner's inputs stop reaching the host this long before the host goes dark, as inputs in flight do over a relay.
			uint64_t joinerInputsLostFirstMs = 0;
			// The referee names the held seat: whichever half it decides for holds the other, and that half returns.
			bool refereeDecides = false;
			// The absent seat is gone past the donor's history and hands itself to a private image.
			bool imageReturn = false;
			// No proposal or certificate of the subject's own return reaches it: it learns the return from the committed tail alone.
			bool returnByTailOnly = false;
			// The subject's machine simulates at this share of the round's rate through the fault, its catch-up included (0: off).
			uint32_t slowPercent = 0;
		};
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
			bool Split(uint8_t a, uint8_t b) const {
				if (!DuringFault() || row.frozen || row.blackout || !row.gapMs) return false;
				if (row.tie) return (a <= 2) != (b <= 2);
				return a == row.subject || b == row.subject;
			}
			uint64_t Delay(uint8_t a, uint8_t b) const {
				uint32_t rtt = a == row.subject || b == row.subject ? row.rttMs : row.otherRttMs;
				if (row.recoveryRttMs && now >= faultEnd && now < faultEnd + 8000 && (a == row.subject || b == row.subject)) rtt = row.recoveryRttMs;
				return (rtt + 1) / 2;
			}
			bool DirectoryReachable(uint8_t peer) const {
				if (!DuringFault()) return true;
				if (row.directoryDown) return false;
				return row.directoryBoth || peer != row.subject;
			}
			// A two-seat internet round whose host still reaches the referee keeps the host's history, so the joiner is the seat that returns.
			uint8_t ReturningSeat() const {
				if (row.refereeDecides) return heldSeat;
				return row.seats == 2 && row.internet && row.directoryBoth ? 2 : row.subject;
			}
			uint8_t heldSeat = 0;
			struct Tie { uint64_t at = 0; std::vector<std::vector<uint16_t>> confirmed; std::vector<uint16_t> winner; };
			std::map<uint64_t, Tie> ties;
			std::map<uint8_t, NetHostChangeRequest> adminReports;
			NetHostChangeReply adminDecision;
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
			struct Event { uint64_t at; NetTransportEvent value; bool crossesFault = false; };
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
				if (congested) *congested = false;
				const auto found = connections.find(id);
				if (found == connections.end()) { if (error) *error = "the fixture connection is absent"; return false; }
				auto [other, remote] = found->second;
				uint64_t sessionDelay = 0;
				std::vector<uint8_t> delivered = bytes;
				NetHostMigrationMessage decision; NetHash32 key; key.fill(0x39);
				if (hub.row.laterSessionMessageFirst && owner == 2 && NetHostMigrationCodec::Decode(bytes, key, decision) &&
				    decision.type == NetHostMigrationMessageType::PeerSession) {
					const auto decoded = NetProtocol::Decode(decision.bytes);
					if (decoded.ok && std::holds_alternative<NetChat>(decoded.message.payload)) {
						sessionDelay = 300; ++hub.delayedChatSends;
						decision.appliedFrame = hub.now; NetHostMigrationCodec::Encode(decision, key, delivered);
					}
				}
				if (hub.row.receiptGap && NetHostMigrationCodec::Decode(bytes, key, decision) && RemoveReceiptGap(decision, delivered)) return true;
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
				if (hub.dropReceiptsFrom == owner && NetHostMigrationCodec::Decode(bytes, key, decision) && decision.type == NetHostMigrationMessageType::PeerReceipt) return true;
				if (hub.dropInputsFrom == owner && NetHostMigrationCodec::Decode(bytes, key, decision) &&
				    (decision.type == NetHostMigrationMessageType::PeerInput || decision.type == NetHostMigrationMessageType::PeerForwardInput)) return true;
				if (lane == NetTransportLane::ControlReliable && NetHostMigrationCodec::Decode(bytes, key, decision)) {
					[]<class Message>(Hub& hub, const Message& packet) {
						using Type = std::decay_t<decltype(packet.type)>;
						if constexpr (requires { Type::PeerBridgeCommit; }) {
							if (packet.type == Type::PeerBridgeCommit && packet.preparedFrame <= 1)
								for (uint8_t peer: packet.members) hub.bridgeBoundaries[peer][packet.frame] = packet.preparedFrame == 0;
						}
					}(hub, decision);
				}
				if (hub.row.returnByTailOnly && other->owner == hub.row.subject && NetHostMigrationCodec::Decode(bytes, key, decision) &&
				    (decision.type == NetHostMigrationMessageType::PeerBridge || decision.type == NetHostMigrationMessageType::PeerBridgeCommit) && decision.preparedFrame == 1 &&
				    std::find(decision.members.begin(), decision.members.end(), hub.row.subject) != decision.members.end()) { ++hub.droppedReturns; return true; }
				unsigned asymmetric = 0;
				if (hub.row.asymmetricBridge && hub.DuringFault()) {
					NetHostMigrationMessage packet; NetHash32 key; key.fill(0x39);
					if (NetHostMigrationCodec::Decode(bytes, key, packet)) asymmetric = DelayAsymmetricInput(packet, *other, remote, lane, bytes);
				}
				if (asymmetric == 1 || (asymmetric != 2 && hub.Split(owner, other->owner))) return true;
				const uint32_t ordinal = ++sends;
				if (lane == NetTransportLane::InputUnreliable && hub.DuringFault() && hub.row.lossPercent &&
				    (owner == hub.row.subject || other->owner == hub.row.subject) && ordinal % 100 < hub.row.lossPercent) return true;
#if __has_include("NetPeerSessionWire.h")
				if (hub.row.receiptGap && NetHostMigrationCodec::Decode(delivered, key, decision)) {
					if (decision.type == NetHostMigrationMessageType::PeerTail)
						hub.tailQueuePeak = std::max(hub.tailQueuePeak, GetPeerQueueStats(id).reliableBytes + delivered.size());
					if (decision.type == NetHostMigrationMessageType::PeerTailRequest)
						for (unsigned duplicate = 0; duplicate < 5; ++duplicate)
							other->events.push_back({hub.now + hub.Delay(owner, other->owner), {NetTransportEventType::PacketReceived, remote, lane, delivered, {}}, false});
				}
#endif
				other->events.push_back({hub.now + hub.Delay(owner, other->owner) + sessionDelay, {NetTransportEventType::PacketReceived, remote, lane, std::move(delivered), {}}, asymmetric == 2}); return true;
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
				for (auto it = events.begin(); it != events.end();) {
					if (it->at > hub.now) { ++it; continue; }
					if (it->crossesFault || !hub.Split(owner, connections.contains(it->value.peerId) ? connections.at(it->value.peerId).first->owner : owner)) ready.push_back(std::move(it->value));
					it = events.erase(it);
				}
				return ready;
			}
			uint32_t GetPeerPingMs(NetPeerId id) const override { return connections.contains(id) ? static_cast<uint32_t>(2 * hub.Delay(owner, connections.at(id).first->owner)) : 0; }
			bool IsPeerPingMeasured(NetPeerId id) const override { return connections.contains(id); }
#if __has_include("NetPeerSessionWire.h")
			NetTransportQueueStats GetPeerQueueStats(NetPeerId id) const override {
				const auto connection = connections.find(id); if (connection == connections.end()) return {};
				const auto [other, remote] = connection->second; uint64_t bytes = 0;
				for (const auto& event: other->events) if (event.value.peerId == remote && event.value.lane == NetTransportLane::ControlReliable) bytes += event.value.bytes.size();
				return {bytes, 0, 0};
			}
#endif
		};
		struct World {
			uint64_t applied = 0;
			std::map<int64_t, int64_t> positions;
			// The seats this world's simulation handed to the AI: a hand-over or hand-back is a simulated event like any input.
			std::set<uint8_t> aiSeats;
			std::map<uint64_t, std::string> aiAt; //!< The AI-held seats at each displayed frame, named when two worlds differ.
			std::map<uint64_t, NetHash32> hashes;
			bool Apply(const NetLockstepReadyFrame& ready, std::string& error) {
				if (ready.frame != applied + 1) { error = "a displayed frame was skipped or undone: expected=" + std::to_string(applied + 1) + " actual=" + std::to_string(ready.frame); return false; }
				for (const auto* inputs: {&ready.localFrames, &ready.remoteFrames}) for (const auto& input: *inputs) positions[input.actorUniqueID] += input.analogMoveX;
				for (uint8_t peer: ready.reclaimedPeerIds) aiSeats.erase(peer);
				for (uint8_t peer: ready.aiHeldPeerIds) aiSeats.insert(peer);
				applied = ready.frame;
				std::string state = std::to_string(applied);
				for (const auto& [actor, position]: positions) state += ":" + std::to_string(actor) + ":" + std::to_string(position);
				std::string ai;
				for (uint8_t peer: aiSeats) ai += (ai.empty() ? "" : ",") + std::to_string(peer);
				if (!ai.empty()) { state += ":ai" + ai; aiAt[applied] = ai; }
				hashes[applied] = NetIdentity::HashCanonicalText("session-plane-fixture", {{"state", state}}); return true;
			}
		};
		template<class Config> void FrameGroupConfig(Config& config, Hub& hub, uint8_t peer) {
			if constexpr (requires { config.peerFrameGroups; config.frameTieReferee; }) {
				config.peerFrameGroups = true;
				config.peerSessionLinks = std::make_shared<typename std::decay_t<decltype(config.peerSessionLinks)>::element_type>();
				if (hub.row.adminDirectory || hub.row.adminDirectoryDown) config.hostChangeReferee = [&hub, peer](const NetHostChangeRequest& request) {
					hub.firstAdminRequestMs = std::min(hub.firstAdminRequestMs, hub.now);
					if (hub.row.adminDirectoryDown) return NetHostChangeReply{};
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
					hub.adminDecision = reply; return reply;
				};
				using Reply = typename std::decay_t<decltype(config.frameTieReferee)>::result_type;
				if (hub.row.internet) config.frameTieReferee = [&hub, peer](const auto& request) {
					Reply reply; reply.state = Reply::State::Waiting;
					if (!hub.DirectoryReachable(peer)) return reply;
					if (request.queryOnly && !hub.ties.contains(request.frame)) return reply;
					auto [found, inserted] = hub.ties.try_emplace(request.frame);
					auto& tie = found->second;
					if (inserted) tie.at = hub.now;
					// The directory's rule: the first half to check in keeps the split at once.
					if (!request.queryOnly && tie.winner.empty()) { tie.confirmed.push_back(request.members); tie.winner = request.members; }
					if (!tie.winner.empty()) { reply.state = Reply::State::Decided; reply.members = tie.winner; } return reply;
				};
			}
		}
		template<class Peer> bool CatchingUp(const Peer& peer) { if constexpr (requires { peer.PeerFrameCatchUpActive(); }) return peer.PeerFrameCatchUpActive(); else return false; }
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
		struct Fixture {
			Hub hub;
			std::vector<std::unique_ptr<Wire>> wires;
			std::vector<std::unique_ptr<NetLockstepCoordinator>> peers;
			std::vector<World> worlds;
			std::vector<std::map<uint64_t, NetLockstepReadyFrame>> committed;
			std::function<void(NetLockstepCoordinator&, INetTransport&)> onFreeze;
			std::function<void()> onThaw;
			bool freezeStarted = false, thawed = false;
			uint64_t freezeWallMs = 0;
			std::vector<double> credits;
			std::vector<uint64_t> queued, waitAt, maxWait, caughtAt;
			std::vector<uint64_t> displayedAt, blackoutGap, returnedAt, postReturnWait, imageAt;
			std::vector<uint64_t> firstDisplayedAt;
			std::vector<uint64_t> startupResumeAt;
			std::vector<int64_t> preFaultPaceLeadMs;
			std::vector<bool> seenAI;
			std::vector<uint16_t> settledDelay;
			std::vector<uint32_t> settledChanges, settledHostWaits;
			std::vector<uint64_t> paceBegin, paceEnd;
			std::vector<uint64_t> firstAdminChangeMs;
			std::map<uint8_t, uint64_t> continuityFrom;
			uint64_t substitutes = 0, continuityFrames = 0, aiFrames = 0, hostChanges = 0, firstLostPressMs = UINT64_MAX;
			uint64_t shortGapSubstitutes = 0;
			bool Start(const Case& row, std::string& error) {
				hub.row = row; hub.faultEnd = hub.faultAt + (row.steadyMs ? row.steadyMs : row.gapMs);
				worlds.resize(row.seats); committed.resize(row.seats); credits.resize(row.seats); queued.resize(row.seats); waitAt.resize(row.seats);
				maxWait.resize(row.seats); caughtAt.resize(row.seats); settledDelay.resize(row.seats); settledChanges.resize(row.seats);
				displayedAt.resize(row.seats); blackoutGap.resize(row.seats); returnedAt.resize(row.seats); postReturnWait.resize(row.seats); seenAI.resize(row.seats);
				imageAt.resize(row.seats);
				firstDisplayedAt.resize(row.seats); startupResumeAt.resize(row.seats); preFaultPaceLeadMs.resize(row.seats);
				settledHostWaits.resize(row.seats); paceBegin.resize(row.seats); paceEnd.resize(row.seats);
				firstAdminChangeMs.assign(row.seats, UINT64_MAX);
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
				if (hub.row.refereeDecides) return hub.heldSeat != 0 && peer == hub.heldSeat;
				if (hub.row.internet && hub.row.directoryDown) return true;
				if (hub.row.seats == 2 && (!hub.row.internet || hub.row.directoryBoth)) return peer != 1;
				return hub.row.tie ? peer >= 3 : peer == hub.row.subject;
			}
			bool CheckCommittedInputs(const NetLockstepReadyFrame& ready, uint8_t observer, std::string& error) {
				if (IsAffected(observer) || !hub.row.gapMs) return true;
				const uint8_t subject = hub.row.refereeDecides ? hub.heldSeat : hub.row.seats == 2 && (!hub.row.internet || hub.row.directoryBoth) ? 2 : hub.row.subject;
				if (subject == 0) return true;
				const auto bridge = Bridges(*peers[observer - 1]);
				if (!bridge.contains(subject)) return true;
				uint64_t continuityStart = 0;
				if (const auto changes = hub.bridgeBoundaries.find(subject); changes != hub.bridgeBoundaries.end()) {
					const auto at = changes->second.upper_bound(ready.frame);
					if (at == changes->second.begin() || !std::prev(at)->second) return true;
					continuityStart = std::prev(at)->first;
				}
				if (peers[observer - 1]->IsSeatUnderAI(subject, ready.frame)) { ++aiFrames; ++substitutes; return true; }
				std::vector<ControllerFrame> frames;
				if (ready.localPeerId == subject) frames = ready.localFrames;
				else {
					size_t offset = 0;
					for (const auto& [peer, count]: ready.remoteFrameCounts) { if (peer == subject) frames.assign(ready.remoteFrames.begin() + offset, ready.remoteFrames.begin() + offset + count); offset += count; }
				}
				if (frames.empty()) return true;
				const auto [first, inserted] = continuityFrom.try_emplace(observer, ready.frame);
				const uint64_t elapsed = static_cast<uint64_t>(std::llround((ready.frame - (continuityStart ? continuityStart : first->second)) * c_TestTickMs));
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
				if (hub.row.realFrozen && hub.DuringFault()) {
					if (!freezeStarted) {
						freezeStarted = true; freezeWallMs = NetLockstepNowMs();
						if (onFreeze) onFreeze(*peers[hub.row.subject - 1], *wires[hub.row.subject - 1]);
					}
					// The actual background sender runs while this seat's main loop
					// does no simulation or plane work for the entire ten seconds.
					while (NetLockstepNowMs() - freezeWallMs < hub.now - hub.faultAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				} else if (freezeStarted && !thawed) { if (onThaw) onThaw(); thawed = true; }
				NetLockstepPlaneGuard plane;
				std::lock_guard lock(hub.mutex);
				if (hub.row.adminDirectory && hub.now == hub.faultAt - 1) {
					// Put the last authenticated host traffic at the silence lever's
					// boundary, rather than at the preceding 250 ms heartbeat slot.
					std::vector<uint8_t> heartbeat;
					if (!NetProtocol::Encode({static_cast<uint32_t>(hub.now), 0, NetHeartbeat{hub.now, 0, 3}}, heartbeat, nullptr)) return false;
					for (uint8_t id = 2; id <= hub.row.seats; ++id)
						peers[id - 1]->InjectEvent({NetTransportEventType::PacketReceived, 1, NetTransportLane::ControlReliable, heartbeat, {}}, hub.now);
				}
				for (uint8_t id = 1; id <= hub.row.seats; ++id) {
					if ((hub.now + id * 3) % hub.row.planeIntervalMs != 0) continue;
					if (hub.row.realFrozen && id == hub.row.subject && hub.DuringFault()) continue;
					if (hub.row.frozen && id == hub.row.subject && hub.DuringFault()) peers[id - 1]->PlaneTick(hub.now);
					else peers[id - 1]->Tick(hub.now);
				}
				if (hub.row.joinerInputsLostFirstMs && hub.now == hub.faultAt - hub.row.joinerInputsLostFirstMs) hub.dropInputsFrom = 2;
				if (hub.row.joinerInputsLostFirstMs && hub.now == hub.faultAt) hub.dropInputsFrom = 0;
				if (hub.row.blackout && hub.now == hub.faultAt) {
					Blackout(*peers[hub.row.subject - 1], worlds[hub.row.subject - 1].applied, hub.row.gapMs, hub.row.internet && !hub.row.directoryBoth);
					if (!HasBlackout<NetLockstepCoordinator>()) hub.row.blackout = false;
				}
				for (uint8_t id = 1; id <= hub.row.seats; ++id) {
					auto& peer = *peers[id - 1]; auto& world = worlds[id - 1];
					if (peer.IsFailed()) { error = "peer " + std::to_string(id) + " failed: " + peer.GetStats().timeoutReason; return false; }
					if (peer.GetHostPeerId() != 1 && (!hub.row.gapMs || hub.row.subject != 1 || hub.row.gapMs < 15000)) { error = "lag changed the lobby host"; return false; }
					if (peer.GetHostPeerId() != 1 && firstAdminChangeMs[id - 1] == UINT64_MAX) {
						firstAdminChangeMs[id - 1] = hub.now;
						if (hub.now + hub.Delay(1, id) < hub.faultAt + 15000) { error = "admin host changed before fifteen seconds of authenticated silence"; return false; }
					}
					if (peer.IsStopped()) {
						// An absent seat gone past the donor's history hands itself to a private image, a path past this fixture.
						if (hub.row.imageReturn && peer.GetStats().timeoutReason.starts_with("PeerHeld:")) { if (!imageAt[id - 1]) imageAt[id - 1] = hub.now; continue; }
						error = "lag ended participation or required a manual rejoin"; return false;
					}
					if (!peer.IsRunning()) continue;
					// A joiner reaches its first displayed tick later, then the
					// normal 3x consumer catches it up to the host. This reproduces
					// the stale first-tick anchor without forcing it ahead of play.
					if (hub.row.delayStartup && id > 1 && !world.applied) {
						if (!startupResumeAt[id - 1]) startupResumeAt[id - 1] = hub.now + (id - 1) * 70;
						if (hub.now < startupResumeAt[id - 1]) continue;
					}
					if (hub.row.frozen && id == hub.row.subject && hub.DuringFault()) { credits[id - 1] = 0; continue; }
					const bool slowed = hub.row.slowPercent && id == hub.row.subject && hub.DuringFault();
					credits[id - 1] += (slowed ? hub.row.slowPercent / 100.0 : CatchingUp(peer) ? c_TestCatchUpRate : 1) / c_TestTickMs;
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
						if (waitAt[id - 1]) {
							const uint64_t wait = hub.now - waitAt[id - 1];
							maxWait[id - 1] = std::max(maxWait[id - 1], wait);
							if (returnedAt[id - 1]) postReturnWait[id - 1] = std::max(postReturnWait[id - 1], wait);
							waitAt[id - 1] = 0;
						}
						peer.FinishFrameWait(hub.now);
						if (hub.row.internet && hub.row.directoryDown && hub.DuringFault() && !Bridges(peer).empty()) {
							error = "an unconfirmed internet side committed a substitute during the directory outage"; return false;
						}
						if (!CheckCommittedInputs(ready, id, error) || !world.Apply(ready, error)) return false;
						if (hub.row.captureFrames) committed[id - 1][ready.frame] = ready;
						peer.FinishSimulationTick(ready.frame); credits[id - 1] -= 1;
						// What the tick cost this machine, as the engine reports it: a light tick, or a slow machine's whole share of its rate.
						peer.NoteLocalTickCost(ready.frame, slowed ? c_TestTickMs * 100.0 / hub.row.slowPercent : 1.0);
						if (displayedAt[id - 1] && hub.DuringFault()) blackoutGap[id - 1] = std::max(blackoutGap[id - 1], hub.now - displayedAt[id - 1]);
						displayedAt[id - 1] = hub.now;
						if (!firstDisplayedAt[id - 1]) firstDisplayedAt[id - 1] = hub.now;
						if (peer.IsSeatUnderAI(hub.ReturningSeat(), ready.frame)) seenAI[id - 1] = true;
						else if (seenAI[id - 1] && !returnedAt[id - 1]) returnedAt[id - 1] = hub.now;
					}
					if (hub.now < hub.faultEnd && hub.row.gapMs < 5000 && HoldVisible(peer, hub.row.subject, hub.now)) { error = "a short hiccup showed a held seat"; return false; }
					if (hub.row.frozen && HoldVisible(peer, hub.row.subject, hub.now)) { error = "flowing keepalives showed a held seat"; return false; }
					if (hub.now == hub.faultAt - 1) {
						preFaultPaceLeadMs[id - 1] = static_cast<int64_t>(std::llround((world.applied - 1) * c_TestTickMs)) -
						    static_cast<int64_t>(displayedAt[id - 1] - firstDisplayedAt[id - 1]);
						settledDelay[id - 1] = peer.InputDelayAt(id, world.applied + 1); settledChanges[id - 1] = SenderResizes(peer, id);
						settledHostWaits[id - 1] = peer.GetStats().peers.contains(1) ? peer.GetStats().peers.at(1).waits : 0;
						paceBegin[id - 1] = world.applied;
					}
					if (hub.now == hub.faultEnd - 1) paceEnd[id - 1] = world.applied;
				}
				if (hub.row.refereeDecides && !hub.heldSeat)
					for (uint8_t id = 1; id <= hub.row.seats && !hub.heldSeat; ++id)
						for (const auto& [seat, at]: Bridges(*peers[id - 1])) if (seat != id) { hub.heldSeat = seat; break; }
				const uint64_t common = std::min_element(worlds.begin(), worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied;
				if (common) {
					for (const auto& world: worlds) if (world.hashes.at(common) != worlds[0].hashes.at(common)) {
						error = "return hashes differ at displayed frame " + std::to_string(common) + ":";
						for (size_t index = 0; index < worlds.size(); ++index) {
							const auto ai = worlds[index].aiAt.find(common);
							error += " peer " + std::to_string(index + 1) + " AI seats [" + (ai == worlds[index].aiAt.end() ? std::string() : ai->second) + "]";
						}
						return false;
					}
				}
				if (hub.now >= hub.faultEnd) {
					const uint64_t highest = std::max_element(worlds.begin(), worlds.end(), [](const World& a, const World& b) { return a.applied < b.applied; })->applied;
					for (uint8_t id = 1; id <= hub.row.seats; ++id) if (!caughtAt[id - 1] && worlds[id - 1].applied + 1 >= highest && common > 0 &&
					    !peers[id - 1]->HasHeldAISeat(hub.ReturningSeat()) && !Bridges(*peers[id - 1]).contains(hub.ReturningSeat())) caughtAt[id - 1] = hub.now;
				}
				++hub.now; return true;
			}
			bool Run(const Case& row, std::string& error) {
				if (!Start(row, error)) return false;
				return RunStarted(row, error);
			}
			bool RunStarted(const Case& row, std::string& error) {
				const uint64_t end = hub.faultEnd + (row.gapMs > 5000 ? row.gapMs / 2 : 0) + c_TestReturnAllowanceMs + 4000 + row.postReturnMs;
				while (hub.now <= end) if (!Step(error)) return false;
				if (row.asymmetricBridge && row.asymmetricBridge < 3 && !hub.asymmetricReleased) { error = "the asymmetric input lever did not force both pending bridge votes"; return false; }
				if (row.returnByTailOnly && !hub.droppedReturns) { error = "the tail-only lever withheld no proposal or certificate of the subject's return"; return false; }
				const uint32_t requesters = ((1U << row.seats) - 1) & ~(1U << (row.subject - 1)) & ~(1U << 2);
				if (row.asymmetricBridge == 3 && (hub.asymmetricDropped != requesters || hub.asymmetricResent == 0)) { error = "the lost-forward lever did not recover an accepted input from its surviving witness"; return false; }
				// Check every unaffected consumer before a missing return can hide its wait.
				for (uint8_t id = 1; id <= row.seats; ++id) {
					const bool doubleFailure = row.internet && row.directoryDown;
					if (!IsAffected(id) && !doubleFailure && maxWait[id - 1] > (row.internet && row.seats == 2 && !row.refereeDecides ? 1000 : static_cast<uint64_t>(std::ceil(c_TestBoundTicks * c_TestTickMs)))) {
						error = "non-lagging peer " + std::to_string(id) + " stalled " + std::to_string(maxWait[id - 1]) + "ms past its bound"; return false;
					}
				}
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
				if (row.refereeDecides && !hub.heldSeat) { error = "the outage held no seat: both halves stalled through it"; return false; }
				if (row.imageReturn && !imageAt[hub.heldSeat - 1]) { error = "the absent seat never handed itself to a private image after the donor's history ran out"; return false; }
				for (uint8_t id = 1; id <= row.seats; ++id) {
					if (row.imageReturn && imageAt[id - 1]) continue;
					// The survivor keeps the absent seat under the AI until the private image brings it back.
					if (row.imageReturn) {
						if (!peers[id - 1]->HasHeldAISeat(hub.heldSeat) && !Bridges(*peers[id - 1]).contains(hub.heldSeat)) { error = "the survivor released the absent seat before its private image returned"; return false; }
						continue;
					}
					if (!caughtAt[id - 1]) {
						error = "a retained seat did not return automatically: peer=" + std::to_string(id) + " applied=" + std::to_string(worlds[id - 1].applied) +
						    " prepared=" + std::to_string(peers[id - 1]->GetStats().nextFrame) + " bridge_count=" + std::to_string(Bridges(*peers[id - 1]).size());
						return false;
					}
					const uint8_t held = row.refereeDecides ? hub.heldSeat : row.seats == 2 && (!row.internet || row.directoryBoth) ? 2 : row.subject;
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
				          << " max_wait_ms=" << greatestWait << " catch_up_ms=" << (caughtAt[row.subject - 1] ? caughtAt[row.subject - 1] - hub.faultEnd : 0)
				          << " held_inputs=" << continuityFrames << " ai_inputs=" << aiFrames << " first_discarded_press_ms="
				          << (firstLostPressMs == UINT64_MAX ? -1 : static_cast<int64_t>(firstLostPressMs)) << " hashes=equal undone=0" << std::endl;
				return true;
			}
		};

		bool CheckRelayReturnAndDepartures(std::string& error) {
			Case row{"three_peer_relay_host_return"};
			row.seats = 3; row.subject = 1; row.gapMs = 8000; row.blackout = true;
			row.rttMs = 250; row.otherRttMs = 100;
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
				    phased.postReturnWait[id - 1] > 100 || peer.GetStats().peers.at(1).rejoins != 1 ||
				    (id != 1 && (phased.blackoutGap[id - 1] > 50 || phased.preFaultPaceLeadMs[id - 1] < 60))) {
					error = "phased relay return failed: peer=" + std::to_string(id) + " blackout_gap_ms=" + std::to_string(phased.blackoutGap[id - 1]) +
					    " post_return_wait_ms=" + std::to_string(phased.postReturnWait[id - 1]) + " returns=" + std::to_string(peer.GetStats().peers.at(1).rejoins); return false;
				}
				std::cout << "[net-session-plane-selftest] relay-phased peer=" << static_cast<int>(id) << " blackout_gap_ms=" << phased.blackoutGap[id - 1]
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
				std::cout << "[net-session-plane-selftest] relay-repeated peer=" << static_cast<int>(id)
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
				std::cout << "[net-session-plane-selftest] relay-receipt peer=" << static_cast<int>(id) << " gap_frame=" << receipts.hub.receiptGapFrame
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
				std::cout << "[net-session-plane-selftest] relay-departure arm=" << arm << " elapsed_ms=" << ended.hub.now - at
				          << " applied=" << applied << " stopped=" << host.IsStopped() << " result=PASS" << std::endl;
			}
			return true;
		}
	}

	bool NetSessionPlaneSelfTest::CheckOwnerHitch(unsigned arm, std::string* error) {
		Case row{"owner_hitch"};
		row.seats = 3; row.subject = arm == 3 ? 1 : 2;
		row.gapMs = arm == 0 ? 2000 : arm == 1 ? 6000 : arm == 2 ? 10000 : 3000;
		row.frozen = arm != 1; row.realFrozen = arm == 2; row.captureFrames = true;
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
		if (arm == 2 && keepalives < 100) { if (error) *error = "the stopped main loop did not retain its independent authenticated sender for ten seconds"; return false; }
		const size_t first = row.subject == 1 ? 1 : 0, second = 2;
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
		std::cout << "[net-lockstep-selftest] PASS combat_bridge case=" << arm << " stall_ms=" << row.gapMs << " F=" << heldAt << " G=" << returnedAt
		          << " keepalives=" << keepalives << " host_changes=0 images=0 undone=0" << std::endl;
		return true;
	}

	bool NetSessionPlaneSelfTest::CheckHostAdministration(unsigned arm, std::string* error) {
		Case row{"certified_admin_continuity"}; row.subject = 1; row.gapMs = 20000;
		// The host returns after the votes, before their future activation frame.
		if (arm == 4) row.gapMs = 15100;
		row.seats = arm == 1 || arm == 3 ? 2 : 4; row.captureFrames = true;
		row.adminDirectory = row.internet = arm == 1; row.adminDirectoryDown = arm == 2;
		Fixture fixture; std::string why;
		if (!fixture.Run(row, why)) { if (error) *error = why; return false; }
		if (arm == 3) {
			for (const auto& peer: fixture.peers) if (peer->GetHostPeerId() != 1 || peer->GetMigrationResult().hostPeerId != 0 || peer->IsMigrating()) {
				if (error) *error = "a lone direct joiner promoted itself across the host's frame tie"; return false;
			}
			std::cout << "[net-lockstep-selftest] PASS certified_admin_continuity arm=3 silence_ms=20000 solo_promotions=0 automatic_return=1" << std::endl;
			return true;
		}
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
		std::cout << "[net-lockstep-selftest] PASS certified_admin_continuity arm=" << arm
		          << " silence_ms=15000 directory_wait_ms=" << (row.adminDirectory ? 1500 : 0)
		          << " every_survivor=1 frames_moved=0 automatic_return=1" << std::endl;
		return true;
	}

	bool NetSessionPlaneSelfTest::CheckSceneLifecycle(const std::string& name, std::string* error) {
		Hub hub;
		const uint8_t count = name == "rematch-chat" || name == "kick-received-two" || name == "heavy-input" || name == "leave-rejoin-two" || name == "drop-after-input" || name == "drop-before-host-input" || name == "referee-input-runway" ? 2 : 3;
		hub.row = Case{"scene_lifecycle"}; hub.row.seats = count; hub.row.rttMs = hub.row.otherRttMs = 100;
		auto ip = std::make_unique<Wire>(hub, 1);
		auto relay = std::make_unique<Wire>(hub, 1);
		NetMuxTransport hostWire(std::move(ip), std::move(relay));
		GnsP2PConfig relayConfig; relayConfig.connectionMode = 2;
		hostWire.SetHostP2P(3, relayConfig);
		Wire clientWire(hub, 2), otherWire(hub, 3), returningWire(hub, 2);
		NetSession hostSession, clientSession, otherSession, returningSession;
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
		const uint16_t delay = count == 3 ? 24 : 12;
		match.inputDelayFrames = delay; match.peerInputDelayFrames.assign(count, delay);
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
			// The directory gives a tie to the group that holds the host.
			if (name == "drop-after-input" || name == "drop-before-host-input") config.frameTieReferee = [&hub](const NetFrameTieRequest& request) {
				NetFrameTieReply reply; reply.state = NetFrameTieReply::State::Waiting;
				if (!hub.tieAnswersHeld && !request.queryOnly && std::find(request.members.begin(), request.members.end(), request.host) != request.members.end()) { reply.state = NetFrameTieReply::State::Decided; reply.members = request.members; }
				return reply;
			};
			if (name == "referee-input-runway") config.frameTieReferee = [&, index](const NetFrameTieRequest& request) {
				const uint64_t sentAt = refereeRequests[index].try_emplace(request.frame, hub.now).first->second;
				NetFrameTieReply reply; reply.state = NetFrameTieReply::State::Waiting;
				// The real directory is asynchronous, including its immediate
				// host verdict. Keep that exchange inside the existing bound.
				if (hub.now >= sentAt + 200 && !request.queryOnly &&
				    std::find(request.members.begin(), request.members.end(), request.host) != request.members.end()) {
					reply.state = NetFrameTieReply::State::Decided; reply.members = request.members;
				}
				return reply;
			};
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
			for (size_t index = 0; index < count; ++index) {
				if (!joinerPlaying && index == 1) continue;
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
		if (name == "referee-input-runway") {
			for (const uint64_t until = hub.now + 10000; hub.now < until;) if (!step(true)) return false;
			const size_t healthyQueries = refereeRequests[0].size() + refereeRequests[1].size();
			std::cout << "[net-session-scene] referee-input-runway healthy_queries=" << healthyQueries
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
			std::cout << "[net-session-scene] referee-input-runway drop_queries=" << refereeRequests[0].size()
			          << " before=" << before << " after=" << applied[0] << " longest_wait_ms=" << longestWait << " held=" << held << std::endl;
			if (refereeRequests[0].empty() || longestWait > 1000 || hub.now - displayedAt[0] > 50 || applied[0] < before + 110 || !held) {
				*error = "a real dropped link did not receive its bounded directory decision and AI hold"; return false;
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
			std::cout << "[net-session-scene] kick-pending before=" << before << " after=" << applied[0] << " two_updates_ms=" << firstTwoAt
			          << " greatest_gap_ms=" << greatestGap << " final_wait_ms=" << finalWait << " removed_repairs=" << host.m_PeerReceiptRepairs - repairs << " released=" << released << " named=" << named << std::endl;
			std::cout << "[net-session-scene] kick-survivor before=" << otherBefore << " after=" << applied[2]
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
			std::cout << "[net-session-scene] heavy-input played=" << played << " reliable_ticks=" << hub.reliableInputTicks.size() << " judged=" << ticks
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
			std::cout << "[net-session-scene] drop-after-input before=" << before << " after=" << applied[0] << " longest_wait_ms=" << longestWait
			          << " final_wait_ms=" << hub.now - displayedAt[0] << " held=" << held << " next=" << host.GetStats().nextFrame << std::endl;
			// The two-seat internet bound includes the referee's answer, as in the drop rows above.
			if (longestWait > 1000 || hub.now - displayedAt[0] > 50 || applied[0] < before + 110 || !held) {
				*error = "a joiner that left after its last input kept the host waiting instead of holding its seat"; return false;
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
			std::cout << "[net-session-scene] drop-before-host-input stalled=" << stalled << " joiner_input_ahead=" << ahead << " undecided=" << undecided << " before=" << before << " after=" << applied[0]
			          << " longest_wait_ms=" << longestWait << " final_wait_ms=" << hub.now - displayedAt[0] << " held=" << held << " next=" << host.GetStats().nextFrame << std::endl;
			if (!ahead || !undecided) { *error = "the host never waited on its own input with the joiner's input in hand and no tie decided"; return false; }
			if (longestWait > 1000 || hub.now - displayedAt[0] > 50 || applied[0] < before + 110 || !held) {
				*error = "a joiner whose link closed before it took the host's input kept the host waiting on its own input instead of holding its seat"; return false;
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
				std::cout << "[net-session-scene] " << name << " peer=" << index + 1 << " boundary=" << boundary << " before=" << before[index] << " after=" << applied[index]
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
			std::cout << "[net-session-scene] rematch-chat received=" << hostLines << " host_messages=" << lobbies[0].GetStats().messagesReceived
			          << " joiner_messages=" << lobbies[1].GetStats().messagesReceived << " countdown=" << countdown << " canceled=" << canceled << std::endl;
			for (size_t index = 0; index < 2; ++index) std::cout << "[net-session-scene] lobby-state peer=" << index + 1
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
			std::cout << "[net-session-scene] lobby-order delayed_chat_sends=" << hub.delayedChatSends << " quiet_sends=" << quietSends
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
			std::cout << "[net-session-scene] next-round chat=" << nextLines << " host_frame=" << applied[0] << " joiner_frame=" << applied[1] << std::endl;
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
			std::cout << "[net-session-scene] leave-rejoin live=" << live << " held=" << held << " directory_steps=" << directorySteps
			          << " answered_ms=" << (answerAt ? answerAt - 6500 : 0) << " ready=" << ready.size() << " returning_seat=" << static_cast<int>(returningSession.GetLocalPeerId() + 1) << std::endl;
			std::cout << "[net-session-scene] departure-state next=" << rounds[0]->GetStats().nextFrame << " applied=" << applied[0]
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
			// A private catch-up reaches the agreed frame a moment after the return is agreed.
			if (two) for (const uint64_t live = hub.now + 300; hub.now < live;) if (!hostStep(false)) return false;
			config.peerIncarnations[2] = config.initialSeatReclaims.at(2).seatIncarnation;
			if (config.initialSeatReclaims.at(2).activationFrame != activation || config.peerIncarnations.at(2) != 2) {
				*error = "the return snapshot changed its activation or player incarnation"; return false;
			}
			rounds[1] = std::make_unique<NetLockstepCoordinator>();
			if (!rounds[1]->Start(returningWire, config, error)) return false;
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
			std::cout << "[net-session-scene] returned-host-gap greatest_gap_ms=" << hostGap << std::endl;
			if (two && hostGap > 50) { *error = "the host waited on a returning seat that had not started playing"; return false; }
			const uint64_t accepted = rounds[0]->m_PeerAcceptedThrough[2];
			std::cout << "[net-session-scene] returned-input host_frame=" << applied[0] << " joiner_frame=" << applied[1]
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
			std::cout << "[net-session-scene] host-end completed=" << (rounds[0]->GetState() == NetLockstepState::Stopped)
			          << " after_ms=" << hub.now - endAt << std::endl;
			if (rounds[0]->GetState() != NetLockstepState::Stopped || rounds[0]->GetStats().timeoutReason.rfind("Complete:", 0) != 0) {
				*error = "the host's End Match waited on a seat that had already left"; return false;
			}
			return true;
		}
		*error = "unknown scene lifecycle row"; return false;
	}

	int NetSessionPlaneSelfTest::Run() {
		if (!TimerMan::IsConstructed()) TimerMan::Construct();
		if (const char* name = std::getenv("CC_TEST_SESSION_SCENE_CASE")) {
			std::string error;
			const bool passed = CheckSceneLifecycle(name, &error);
			std::cout << "[net-session-plane-selftest] " << (passed ? "PASS " : "FAIL ") << name << (error.empty() ? "" : ": " + error) << std::endl;
			if (passed) std::cout << "[net-session-plane-selftest] PASS" << std::endl;
			return passed ? 0 : 1;
		}
		std::vector<Case> rows;
		const auto gap = [&](const char* name, uint8_t subject, uint64_t ms) { Case row{name}; row.subject = subject; row.gapMs = ms; return row; };
		rows.push_back(gap("peer_returns_2s", 2, 2000)); rows.push_back(gap("peer_returns_8s", 2, 8000)); rows.push_back(gap("peer_returns_40s", 2, 40000));
		auto frozen = gap("peer_frozen_10s_keepalives", 2, 10000); frozen.frozen = true; rows.push_back(frozen);
		auto tailReturn = frozen; tailReturn.name = "peer_frozen_10s_return_by_tail_only"; tailReturn.returnByTailOnly = true; rows.push_back(tailReturn);
		rows.push_back(gap("host_absent_3s", 1, 3000)); rows.push_back(gap("host_absent_20s", 1, 20000));
		auto slowMachine = gap("joiner_machine_at_90_percent_20s", 2, 20000); slowMachine.slowPercent = 90; rows.push_back(slowMachine);
		auto slowHost = gap("two_internet_host_machine_at_90_percent_20s", 1, 20000); slowHost.seats = 2; slowHost.internet = true; slowHost.slowPercent = 90; rows.push_back(slowHost);
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
		// The relay kit's lever: the host's peer link goes dark while both still reach the directory, so the host keeps the tie and the joiner returns.
		auto hostBlackout = gap("two_internet_host_blackout_directory_reachable_by_both", 1, 8000);
		hostBlackout.seats = 2; hostBlackout.internet = hostBlackout.directoryBoth = hostBlackout.blackout = true;
		hostBlackout.rttMs = hostBlackout.otherRttMs = 24; hostBlackout.joinerInputsLostFirstMs = 50; rows.push_back(hostBlackout);
		// With no input in flight both halves stall on their own input; the first to reach the referee plays on and the other returns.
		auto bothStall = gap("two_internet_host_blackout_both_halves_stall", 1, 8000);
		bothStall.seats = 2; bothStall.internet = bothStall.directoryBoth = bothStall.blackout = bothStall.refereeDecides = true;
		bothStall.rttMs = bothStall.otherRttMs = 24; rows.push_back(bothStall);
		// The host's whole connection goes dark, the directory with it: the joiner keeps the split inside the bound and the host returns.
		auto hostDark = gap("two_internet_host_blackout_directory_dark", 1, 8000);
		hostDark.seats = 2; hostDark.internet = hostDark.blackout = hostDark.refereeDecides = true;
		hostDark.rttMs = hostDark.otherRttMs = 24; rows.push_back(hostDark);
		// A joiner's whole connection gone longer than the host keeps history returns through a private image.
		auto pastHistory = gap("two_internet_joiner_dark_past_history", 2, 75000);
		pastHistory.seats = 2; pastHistory.internet = pastHistory.blackout = pastHistory.refereeDecides = pastHistory.imageReturn = true;
		pastHistory.rttMs = pastHistory.otherRttMs = 24; rows.push_back(pastHistory);
		int passed = 0, failed = 0;
		const char* selected = std::getenv("CC_TEST_SESSION_PLANE_CASE");
		std::cout << "[net-session-plane-selftest] topology=single-box proof=false forced_rows=" << (selected ? 1 : rows.size() + 3) << std::endl;
		for (const auto& row: rows) {
			if (selected && std::string_view(selected) != row.name) continue;
			std::string error; Fixture fixture; bool ok = fixture.Run(row, error);
			if (ok && row.name == std::string("relay_link_blackout_return")) ok = CheckRelayReturnAndDepartures(error);
			if (row.name == std::string("partition_3_1") || row.name == std::string("host_absent_3s"))
			    for (unsigned order = row.subject == 1 ? 3 : 1; order <= 3; ++order) {
				auto asymmetric = row; asymmetric.asymmetricBridge = order; asymmetric.rttMs = asymmetric.otherRttMs = 4;
				Fixture arm; std::string detail; const bool converged = arm.Run(asymmetric, detail);
				std::cout << "[net-session-plane-selftest] bridge-input-race subject=" << static_cast<int>(row.subject) << " order=" << (order == 1 ? "input-first" : order == 2 ? "receipt-first" : "lost-forward")
				          << " two_pending_votes=" << arm.hub.asymmetricReleased << " dropped=" << arm.hub.asymmetricDropped << " resent=" << arm.hub.asymmetricResent
				          << " result=" << (converged ? "PASS" : "FAIL") << (detail.empty() ? "" : ": " + detail) << std::endl;
				if (!converged) { ok = false; if (error.empty()) error = detail; }
			}
			if (ok && row.name == std::string("two_joiner_3s_internet_and_direct")) { auto direct = row; direct.internet = false; Fixture arm; ok = arm.Run(direct, error); }
			std::cout << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << row.name << (error.empty() ? "" : ": " + error) << std::endl;
			if (ok) ++passed; else ++failed;
		}
		for (unsigned arm = 0; arm < 3; ++arm) {
			const char* name = arm == 0 ? "successor_unvalidated_boundary_refused" : arm == 1 ? "leave_during_handover_exits" : "malformed_traffic_does_not_renew_authority";
			if (selected && std::string_view(selected) != name) continue;
			std::string error; const bool ok = NetLockstepSelfTest::CheckSessionRecoveryGuard(arm, &error);
			std::cout << "[net-session-plane-selftest] " << (ok ? "PASS " : "FAIL ") << name << (error.empty() ? "" : ": " + error) << std::endl;
			if (ok) ++passed; else ++failed;
		}
		std::cout << "[net-session-plane-selftest] " << passed << " of " << passed + failed << " passed" << std::endl;
		std::cout << "[net-session-plane-selftest] " << (failed ? "FAIL" : "PASS") << std::endl;
		return failed || passed == 0 ? 1 : 0;
	}
}
