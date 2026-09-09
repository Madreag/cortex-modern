#include "NetSession.h"

#include "NetLobbyProtocol.h"
#include "NetLockstep.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace RTE {

	namespace {
		using json = nlohmann::json;

		constexpr uint8_t c_HostAssignedPeerId = 0;

		std::string HashText(const NetHash32& hash) {
			return NetIdentity::HashHex(hash);
		}

		std::string VersionRange(uint16_t minVersion, uint16_t maxVersion) {
			return std::to_string(minVersion) + "-" + std::to_string(maxVersion);
		}

		NetIdentityMismatch MakeMismatch(const std::string& key, NetRejectReason reason, std::string expected, std::string actual, std::string summary) {
			NetIdentityMismatch mismatch;
			mismatch.key = key;
			mismatch.rejectReason = reason;
			mismatch.expectedShortValue = std::move(expected);
			mismatch.actualShortValue = std::move(actual);
			mismatch.summary = std::move(summary);
			return mismatch;
		}

		NetIdentityMismatch NoMismatch() {
			return {};
		}

		bool HasMismatch(const NetIdentityMismatch& mismatch) {
			return !mismatch.key.empty();
		}

		bool IsActive(NetSessionState state) {
			return state == NetSessionState::Handshake ||
			       state == NetSessionState::Accepted ||
			       state == NetSessionState::Ready;
		}

		json HashJson(const NetHash32& hash, bool present) {
			return present ? json(HashText(hash)) : json("");
		}
	}

	bool NetSession::StartHost(INetTransport& transport, NetSessionConfig config, std::string* error) {
		Close("restart");
		m_Transport = &transport;
		m_Config = std::move(config);
		m_Role = NetSessionRole::Host;
		m_State = NetSessionState::Stopped;
		m_StateStartedMs = m_NowMs;
		m_SessionId = m_Config.sessionId;
		m_LocalPeerId = c_HostAssignedPeerId;
		m_RemoteTransportPeerId = c_InvalidNetPeerId;
		m_NextSequence = 0;
		m_LastReceivedSequence = 0;
		m_HasReject = false;
		m_MismatchKey.clear();
		m_ExpectedValue.clear();
		m_ActualValue.clear();
		m_RejectSummary.clear();
		m_HasRemoteIdentityHash = false;
		m_Stats = {};
		m_Peers.clear();
		if (m_Config.maxPeers == 0) {
			if (error) *error = "host maxPeers must be nonzero";
			m_State = NetSessionState::Failed;
			return false;
		}
		if (!m_Transport->StartHost(m_Config.port, error)) {
			m_State = NetSessionState::Failed;
			return false;
		}
		m_State = NetSessionState::Listening;
		m_StateStartedMs = m_NowMs;
		return true;
	}

	bool NetSession::StartClient(INetTransport& transport, const std::string& address, NetSessionConfig config, std::string* error) {
		Close("restart");
		m_Transport = &transport;
		m_Config = std::move(config);
		m_Role = NetSessionRole::Client;
		m_State = NetSessionState::Connecting;
		m_StateStartedMs = m_NowMs;
		m_SessionId = 0;
		m_LocalPeerId = 0;
		m_RemoteTransportPeerId = c_InvalidNetPeerId;
		m_NextSequence = 0;
		m_LastReceivedSequence = 0;
		m_LastReceiveMs = m_NowMs;
		m_NextHeartbeatMs = m_NowMs + m_Config.heartbeatIntervalMs;
		m_HasReject = false;
		m_MismatchKey.clear();
		m_ExpectedValue.clear();
		m_ActualValue.clear();
		m_RejectSummary.clear();
		m_HasRemoteIdentityHash = false;
		m_Stats = {};
		m_Peers.clear();
		if (!m_Transport->Connect(address, m_Config.port, error)) {
			m_State = NetSessionState::Failed;
			return false;
		}
		return true;
	}

	void NetSession::Tick(uint64_t nowMs, bool pollTransport) {
		// Callers clock each setup phase from its own start; never let a later phase rewind us.
		m_NowMs = std::max(m_NowMs, nowMs);
		if (!m_Transport || m_State == NetSessionState::Stopped || m_State == NetSessionState::Closed ||
		    m_State == NetSessionState::Rejected || m_State == NetSessionState::Failed) {
			return;
		}

		if (pollTransport) {
			for (const NetTransportEvent& event : m_Transport->PollEvents()) {
				ProcessEvent(event);
				if (m_State == NetSessionState::Closed || m_State == NetSessionState::Rejected || m_State == NetSessionState::Failed) {
					return;
				}
			}
		}
		CheckTimeouts();
		MaybeSendHeartbeats();
		if (m_ReconnectHost) {
			m_ReconnectHost->Tick(m_NowMs);
		}
		if (m_ReconnectClient) {
			m_ReconnectClient->Tick(m_NowMs);
		}
		FlushReconnectOutbound();
	}

	void NetSession::TickKeepalive(uint64_t nowMs) {
		m_NowMs = std::max(m_NowMs, nowMs);
		if (!m_Transport || m_State == NetSessionState::Stopped || m_State == NetSessionState::Closed ||
		    m_State == NetSessionState::Rejected || m_State == NetSessionState::Failed) {
			return;
		}
		MaybeSendHeartbeats();
	}

	void NetSession::InjectEvent(const NetTransportEvent& event, uint64_t nowMs) {
		m_NowMs = std::max(m_NowMs, nowMs);
		if (!m_Transport || m_State == NetSessionState::Stopped || m_State == NetSessionState::Closed ||
		    m_State == NetSessionState::Rejected || m_State == NetSessionState::Failed) {
			return;
		}
		ProcessEvent(event);
		FlushReconnectOutbound();
	}

	void NetSession::TickAdmissionPlane(uint64_t nowMs) {
		m_NowMs = std::max(m_NowMs, nowMs);
		if (!m_Transport || !m_ReconnectHost || m_State == NetSessionState::Stopped || m_State == NetSessionState::Closed ||
		    m_State == NetSessionState::Rejected || m_State == NetSessionState::Failed) {
			return;
		}
		m_ReconnectHost->Tick(m_NowMs);
		FlushReconnectOutbound();
	}

	void NetSession::EndHostedSession(const std::string& reason) {
		if (m_Transport && m_Role == NetSessionRole::Host) {
			for (const PeerState& peer : m_Peers) {
				if (IsActive(peer.state)) {
					Send(peer.transportPeerId, NetDisconnect{static_cast<uint16_t>(NetRejectReason::SessionEnded), reason});
				}
			}
		}
		if (m_ReconnectHost) {
			m_ReconnectHost->EndHostedSession();
			m_ReconnectHost->TakeOutbound();
		}
		Close(reason);
	}

	void NetSession::Close(const std::string& reason) {
		if (!m_Transport) {
			return;
		}
		if (m_Role == NetSessionRole::Host) {
			for (const PeerState& peer : m_Peers) {
				if (IsActive(peer.state)) {
					Send(peer.transportPeerId, NetDisconnect{0, reason});
					m_Transport->Disconnect(peer.transportPeerId, reason);
				}
			}
		} else if (m_RemoteTransportPeerId != c_InvalidNetPeerId) {
			Send(m_RemoteTransportPeerId, NetDisconnect{0, reason});
			m_Transport->Disconnect(m_RemoteTransportPeerId, reason);
		}
		if (m_State != NetSessionState::Stopped && m_State != NetSessionState::Rejected && m_State != NetSessionState::Failed) {
			m_State = NetSessionState::Closed;
			m_StateStartedMs = m_NowMs;
		}
	}

	NetPeerId NetSession::GetRemoteTransportPeerId() const {
		if (m_RemoteTransportPeerId != c_InvalidNetPeerId) {
			return m_RemoteTransportPeerId;
		}
		if (m_Role == NetSessionRole::Host) {
			const auto it = std::find_if(m_Peers.begin(), m_Peers.end(), [](const PeerState& peer) {
				return peer.state == NetSessionState::Accepted || peer.state == NetSessionState::Ready;
			});
			if (it != m_Peers.end()) {
				return it->transportPeerId;
			}
		}
		return c_InvalidNetPeerId;
	}

	std::vector<NetSessionPeerInfo> NetSession::GetReadyPeers() const {
		std::vector<NetSessionPeerInfo> peers;
		if (m_Role == NetSessionRole::Host) {
			for (const PeerState& peer : m_Peers) {
				if (peer.state == NetSessionState::Ready) {
					peers.push_back({peer.transportPeerId, peer.assignedPeerId, peer.displayName, true});
				}
			}
			// Stable order (by session-assigned id) so both peers derive the same lockstep peer set.
			std::sort(peers.begin(), peers.end(), [](const NetSessionPeerInfo& lhs, const NetSessionPeerInfo& rhs) {
				return lhs.assignedPeerId < rhs.assignedPeerId;
			});
		} else if (m_State == NetSessionState::Ready && m_RemoteTransportPeerId != c_InvalidNetPeerId) {
			peers.push_back({m_RemoteTransportPeerId, c_HostAssignedPeerId, "Host", true});
		}
		return peers;
	}

	uint32_t NetSession::GetReadyPeerCount() const {
		if (m_Role == NetSessionRole::Host) {
			return static_cast<uint32_t>(std::count_if(m_Peers.begin(), m_Peers.end(), [](const PeerState& peer) {
				return peer.state == NetSessionState::Ready;
			}));
		}
		return (m_State == NetSessionState::Ready && m_RemoteTransportPeerId != c_InvalidNetPeerId) ? 1U : 0U;
	}

	bool NetSession::Send(NetPeerId peerId, NetPayload payload, std::string* error) {
		if (!m_Transport) {
			if (error) *error = "session has no transport";
			return false;
		}
		std::vector<uint8_t> bytes;
		NetProtocolError encodeError;
		NetMessage message;
		message.sequence = ++m_NextSequence;
		message.payload = std::move(payload);
		if (!NetProtocol::Encode(message, bytes, &encodeError)) {
			if (error) *error = encodeError.message;
			return false;
		}
		if (!m_Transport->Send(peerId, NetTransportLane::ControlReliable, bytes, error)) {
			return false;
		}
		++m_Stats.sentMessages;
		return true;
	}

	void NetSession::SendHeartbeat(NetPeerId peerId) {
		Send(peerId, NetHeartbeat{m_NowMs, m_LastReceivedSequence, static_cast<uint32_t>(m_State)});
	}

	void NetSession::MaybeSendHeartbeats() {
		if (m_Config.heartbeatIntervalMs == 0 || m_NowMs < m_NextHeartbeatMs) {
			return;
		}
		if (m_Role == NetSessionRole::Host) {
			for (const PeerState& peer : m_Peers) {
				if (peer.state == NetSessionState::Ready) {
					SendHeartbeat(peer.transportPeerId);
				}
			}
		} else if (m_State == NetSessionState::Ready && m_RemoteTransportPeerId != c_InvalidNetPeerId) {
			SendHeartbeat(m_RemoteTransportPeerId);
		}
		m_NextHeartbeatMs = m_NowMs + m_Config.heartbeatIntervalMs;
	}

	void NetSession::ProcessEvent(const NetTransportEvent& event) {
		switch (event.type) {
			case NetTransportEventType::PeerConnected:
				if (m_Role == NetSessionRole::Host) {
					if (FindPeer(event.peerId)) {
						break;
					}
					// Nothing else bounds this list: SessionFull only fires once a hello arrives, so a
					// joiner that connects and stays silent would otherwise grow it until it timed out.
					if (GetUnauthenticatedPeerCount() >= c_MaxUnauthenticatedPeers) {
						++m_Stats.unauthenticatedConnectionsRefused;
						RejectConnection(event.peerId, NetRejectReason::SessionFull, "unauthenticated_connections", std::to_string(c_MaxUnauthenticatedPeers), std::to_string(GetUnauthenticatedPeerCount()), "session is full");
						break;
					}
					PeerState peer;
					peer.transportPeerId = event.peerId;
					peer.state = NetSessionState::Handshake;
					peer.connectedAtMs = m_NowMs;
					peer.lastReceiveMs = m_NowMs;
					m_Peers.push_back(std::move(peer));
					RefreshHostState();
				} else if (m_Role == NetSessionRole::Client) {
					if (m_RemoteTransportPeerId == event.peerId && m_State != NetSessionState::Connecting) {
						break;
					}
					m_RemoteTransportPeerId = event.peerId;
					m_LastReceiveMs = m_NowMs;
					Send(event.peerId, BuildClientHello());
					m_State = NetSessionState::HelloSent;
					m_StateStartedMs = m_NowMs;
				}
				break;
			case NetTransportEventType::PeerDisconnected:
				if (m_Role == NetSessionRole::Host) {
					if (m_ReconnectHost && m_ReconnectHost->NotifyDisconnect(event.peerId, m_LockstepFrame) == NetH4DisconnectOutcome::Fenced) {
						// A superseded incarnation timing out; the seat's live holder is untouched.
						++m_Stats.fencedDisconnects;
						break;
					}
					if (PeerState* peer = FindPeer(event.peerId)) {
						peer->state = NetSessionState::Closed;
					}
					RefreshHostState();
				} else if (m_State != NetSessionState::Rejected && m_State != NetSessionState::Failed) {
					// Keep the close reason the host sent with the disconnect so the UI can show why.
					if (!m_HasReject) {
						RecordReject(NetRejectReason::InternalError, "", "", "", event.reason.empty() ? "connection closed by peer" : event.reason);
					}
					m_State = NetSessionState::Closed;
				}
				break;
			case NetTransportEventType::PacketReceived:
				if (m_Role == NetSessionRole::Host && m_ReconnectHost && m_ReconnectHost->IsFenced(event.peerId)) {
					// Delayed traffic from a transport the seat no longer answers to.
					++m_Stats.fencedPackets;
					m_ReconnectHost->CountFencedPacket();
					break;
				}
				ProcessPacket(event.peerId, event.bytes);
				break;
			case NetTransportEventType::LocalTransportFault:
				// Our own transport pump broke - genuinely fatal for either role.
				SetFailed(NetRejectReason::InternalError, "transport", "", event.reason, event.reason.empty() ? "local transport fault" : event.reason);
				break;
			case NetTransportEventType::ConnectionFailed:
			case NetTransportEventType::TransportError:
				if (m_Role == NetSessionRole::Host) {
					// Admission isolation: an unauthenticated joiner's half-open connection faulting must
					// not fail the host session for everyone else. A committed peer drops via PeerDisconnected.
					++m_Stats.unboundConnectionFaults;
				} else {
					// The client's lone link to the host faulted - it genuinely cannot proceed.
					SetFailed(NetRejectReason::InternalError, "transport", "", event.reason, event.reason.empty() ? "transport error" : event.reason);
				}
				break;
		}
	}

	void NetSession::ProcessPacket(NetPeerId peerId, const std::vector<uint8_t>& bytes) {
		const NetDecodeResult decoded = NetProtocol::Decode(bytes);
		if (!decoded.ok) {
			// Another phase's packet on the shared wire: a peer that finished its session handshake
			// starts its lobby round while we still wait for the others (N-peer), or a prior match's
			// in-flight lockstep frames. The lobby protocol tolerates our packets the same way.
			if (NetLobbyProtocol::Decode(bytes).ok || NetLockstepCodec::LooksLikePacket(bytes)) {
				++m_Stats.ignoredPhasePackets;
				return;
			}
			HandleMalformed(peerId, decoded.error, bytes);
			return;
		}
		++m_Stats.receivedMessages;
		m_LastReceivedSequence = decoded.message.sequence;
		m_LastReceiveMs = m_NowMs;
		if (m_Role == NetSessionRole::Host) {
			if (PeerState* peer = FindPeer(peerId)) {
				peer->lastReceiveMs = m_NowMs;
			}
			HandleHostMessage(peerId, decoded.message);
		} else {
			HandleClientMessage(peerId, decoded.message);
		}
	}

	void NetSession::HandleMalformed(NetPeerId peerId, const NetProtocolError& decodeError, const std::vector<uint8_t>& bytes) {
		++m_Stats.malformedMessages;
		const std::string summary = std::string("malformed ") + NetProtocol::ErrorCodeName(decodeError.code) + ": " + decodeError.message;
		if (m_Role == NetSessionRole::Host && decodeError.code == NetProtocolErrorCode::UnsupportedVersion) {
			RejectOldWirePeer(peerId, bytes);
			return;
		}
		if (m_Role == NetSessionRole::Host) {
			if (PeerState* peer = FindPeer(peerId)) {
				if (peer->state == NetSessionState::Handshake) {
					RejectPeer(*peer, NetRejectReason::MalformedMessage, "decode", "valid message", summary, summary);
				} else {
					Send(peerId, NetDisconnect{static_cast<uint16_t>(NetRejectReason::MalformedMessage), summary});
					m_Transport->Disconnect(peerId, summary);
					peer->state = NetSessionState::Failed;
					RefreshHostState();
				}
			}
		} else {
			SetFailed(NetRejectReason::MalformedMessage, "decode", "valid message", summary, summary);
			if (m_RemoteTransportPeerId != c_InvalidNetPeerId) {
				m_Transport->Disconnect(m_RemoteTransportPeerId, summary);
			}
		}
	}

	void NetSession::RejectOldWirePeer(NetPeerId peerId, const std::vector<uint8_t>& bytes) {
		uint16_t claimedVersion = 0;
		const bool haveVersion = NetProtocol::PeekHeaderVersion(bytes.data(), bytes.size(), claimedVersion);
		const std::string summary = "protocol version " + (haveVersion ? std::to_string(claimedVersion) : std::string("?")) +
		                            " does not match this build's " + std::to_string(NetProtocol::c_Version);
		RecordReject(NetRejectReason::ProtocolMismatch, "protocol_version", std::to_string(NetProtocol::c_Version),
		             haveVersion ? std::to_string(claimedVersion) : std::string("unknown"), summary);
		// §10: the envelope's magic and version sit at fixed offsets, so an explicit rejection is
		// possible whenever we can still write that version's payload schema. When we cannot, sending
		// one stamped at OUR version would be undecodable noise, so the disconnect reason carries it.
		if (haveVersion && NetProtocol::CanEncodeAtVersion(claimedVersion)) {
			NetMessage rejection;
			rejection.sequence = m_NextSequence++;
			rejection.payload = NetJoinRejected{NetRejectReason::ProtocolMismatch, summary, "protocol_version", std::to_string(NetProtocol::c_Version), haveVersion ? std::to_string(claimedVersion) : std::string("unknown")};
			std::vector<uint8_t> encoded;
			if (NetProtocol::EncodeAtVersion(rejection, claimedVersion, encoded) &&
			    m_Transport->Send(peerId, NetTransportLane::ControlReliable, encoded, nullptr)) {
				++m_Stats.sentMessages;
				++m_Stats.oldWireRejectionsSent;
			}
		} else {
			++m_Stats.oldWireDisconnects;
		}
		if (PeerState* peer = FindPeer(peerId)) {
			peer->state = NetSessionState::Closed;
		}
		m_Transport->Disconnect(peerId, summary);
		RefreshHostState();
	}

	void NetSession::HandleHostMessage(NetPeerId peerId, const NetMessage& message) {
		PeerState* peer = FindPeer(peerId);
		if (!peer || !IsActive(peer->state)) {
			return;
		}
		if (const auto* hello = std::get_if<NetClientHello>(&message.payload)) {
			if (peer->state != NetSessionState::Handshake) {
				RejectPeer(*peer, NetRejectReason::HostNotAccepting, "state", "handshake", StateName(peer->state), "host is not accepting another hello for this peer");
				return;
			}
			uint8_t assignedPeerId = AllocatePeerId();
			if (assignedPeerId == 0) {
				// §6: the seat's own id is still held by the incarnation this joiner may be about to
				// supersede, so it proves on a provisional one and takes the seat's id from the commit.
				assignedPeerId = AllocatePendingAdmissionPeerId();
				if (assignedPeerId == 0) {
					RejectPeer(*peer, NetRejectReason::SessionFull, "peer_count", std::to_string(m_Config.maxPeers), std::to_string(ActivePeerCount()), "session is full");
					return;
				}
				++m_Stats.pendingAdmissionJoins;
			}
			for (const PeerState& other : m_Peers) {
				if (&other != peer && IsActive(other.state) && other.clientNonce == hello->clientNonce) {
					RejectPeer(*peer, NetRejectReason::DuplicateClientNonce, "client_nonce", "unique", std::to_string(hello->clientNonce), "duplicate client nonce");
					return;
				}
			}
			const NetIdentityMismatch mismatch = ValidateClientHello(*hello);
			if (HasMismatch(mismatch)) {
				RejectPeer(*peer, mismatch.rejectReason, mismatch.key, mismatch.expectedShortValue, mismatch.actualShortValue, mismatch.summary);
				return;
			}
			peer->clientNonce = hello->clientNonce;
			peer->displayName = hello->displayName;
			peer->identityHash = hello->sessionIdentityHash;
			peer->assignedPeerId = assignedPeerId;
			m_RemoteIdentityHash = hello->sessionIdentityHash;
			m_HasRemoteIdentityHash = true;
			Send(peerId, BuildHostHello(peer->assignedPeerId));
			Send(peerId, BuildJoinAccepted(peer->assignedPeerId));
			peer->state = NetSessionState::Accepted;
			RefreshHostState();
			return;
		}
		if (const auto* ready = std::get_if<NetReadyState>(&message.payload)) {
			if (peer->state != NetSessionState::Accepted && peer->state != NetSessionState::Ready) {
				RejectPeer(*peer, NetRejectReason::HostNotAccepting, "state", "accepted", StateName(peer->state), "ready state arrived before acceptance");
				return;
			}
			if (ready->peerId != peer->assignedPeerId) {
				RejectPeer(*peer, NetRejectReason::ProtocolMismatch, "peer_id", std::to_string(peer->assignedPeerId), std::to_string(ready->peerId), "ready state peer id does not match accepted session");
				return;
			}
			if (ready->deterministicConfigHash != m_Config.localIdentity.deterministicConfigHash) {
				RejectPeer(*peer, NetRejectReason::DeterministicConfigMismatch, "deterministic_config_hash", HashText(m_Config.localIdentity.deterministicConfigHash), HashText(ready->deterministicConfigHash), "ready state deterministic config hash does not match accepted session");
				return;
			}
			if (ready->moduleManifestHash != m_Config.localIdentity.moduleManifestHash) {
				RejectPeer(*peer, NetRejectReason::ModuleManifestMismatch, "module_manifest_hash", HashText(m_Config.localIdentity.moduleManifestHash), HashText(ready->moduleManifestHash), "ready state module manifest hash does not match accepted session");
				return;
			}
			peer->state = ready->ready ? NetSessionState::Ready : NetSessionState::Accepted;
			peer->lastHeartbeatMs = m_NowMs;
			if (ready->ready) {
				m_NextHeartbeatMs = m_NowMs;
			}
			RefreshHostState();
			return;
		}
		if (std::holds_alternative<NetHeartbeat>(message.payload)) {
			peer->lastHeartbeatMs = m_NowMs;
			return;
		}
		if (std::holds_alternative<NetDisconnect>(message.payload)) {
			peer->state = NetSessionState::Closed;
			DropPeerTransport(peerId, "peer disconnected");
			RefreshHostState();
			return;
		}
		if (RouteAdmissionMessage(peerId, message.payload)) {
			return;
		}
		if (peer->state == NetSessionState::Handshake) {
			RejectPeer(*peer, NetRejectReason::MalformedMessage, "message_type", "ClientHello", NetProtocol::MessageTypeName(NetProtocol::MessageTypeOf(message.payload)), "unexpected handshake message");
		}
	}

	bool NetSession::RouteAdmissionMessage(NetPeerId peerId, const NetPayload& payload) {
		if (!NetProtocol::IsH4MessageType(NetProtocol::MessageTypeOf(payload))) {
			return false;
		}
		bool handled = false;
		if (m_Role == NetSessionRole::Host && m_ReconnectHost) {
			handled = m_ReconnectHost->HandleMessage(peerId, payload, m_NowMs);
		} else if (m_Role == NetSessionRole::Client && m_ReconnectClient) {
			handled = m_ReconnectClient->HandleMessage(payload, m_NowMs);
		}
		if (handled) {
			++m_Stats.admissionMessages;
			FlushReconnectOutbound();
		}
		return handled;
	}

	void NetSession::FlushReconnectOutbound() {
		if (m_ReconnectHost) {
			for (NetH4Outbound& outbound : m_ReconnectHost->TakeOutbound()) {
				Send(outbound.connection, std::move(outbound.payload));
			}
			for (const NetH4Commit& commit : m_ReconnectHost->TakeCommits()) {
				// The seat's own peer id, never a freshly allocated one: ownership resolution keys on it,
				// so a different id would silently re-point every actor the returner had.
				if (PeerState* peer = FindPeer(commit.connection)) {
					peer->assignedPeerId = commit.assignedPeerId;
					peer->state = NetSessionState::Ready;
					peer->lastReceiveMs = m_NowMs;
					peer->lastHeartbeatMs = m_NowMs;
				}
				if (commit.supersededConnection != c_InvalidNetPeerId) {
					if (PeerState* superseded = FindPeer(commit.supersededConnection)) {
						superseded->state = NetSessionState::Closed;
					}
					if (m_Transport) {
						m_Transport->Disconnect(commit.supersededConnection, "seat reclaimed by a newer connection");
					}
				}
				RefreshHostState();
			}
		}
		if (m_ReconnectClient && m_RemoteTransportPeerId != c_InvalidNetPeerId) {
			for (NetH4Outbound& outbound : m_ReconnectClient->TakeOutbound()) {
				Send(m_RemoteTransportPeerId, std::move(outbound.payload));
			}
			CompleteClientAdmission();
		}
	}

	void NetSession::CompleteClientAdmission() {
		if (m_Role != NetSessionRole::Client || m_State != NetSessionState::Accepted || !m_ReconnectClient) {
			return;
		}
		if (m_ReconnectClient->IsAdmitted()) {
			// The commit names the seat's own peer id (P4); adopt it before declaring ourselves Ready,
			// or the host's ready check would see a different id than the seat it just committed.
			m_LocalPeerId = m_ReconnectClient->GetAssignedPeerId();
			Send(m_RemoteTransportPeerId, BuildReadyState(true));
			m_State = NetSessionState::Ready;
			m_StateStartedMs = m_NowMs;
			m_NextHeartbeatMs = m_NowMs;
			return;
		}
		if (!m_ReconnectClient->IsAdmissionPending()) {
			// The ladder gave up or the local store failed: bounded by P3, never an open-ended wait.
			const std::string summary = m_ReconnectClient->GetError().empty() ? "the host did not admit this seat" : m_ReconnectClient->GetError();
			SetFailed(m_ReconnectClient->GetState() == NetH4ClientState::Failed ? NetRejectReason::InternalError : NetRejectReason::HostNotAccepting,
			          "admission", "committed", NetReconnectClientStateName(m_ReconnectClient->GetState()), summary);
			if (m_RemoteTransportPeerId != c_InvalidNetPeerId) {
				m_Transport->Disconnect(m_RemoteTransportPeerId, summary);
			}
		}
	}

	void NetSession::HandleClientMessage(NetPeerId peerId, const NetMessage& message) {
		if (peerId != m_RemoteTransportPeerId) {
			return;
		}
		if (const auto* rejected = std::get_if<NetJoinRejected>(&message.payload)) {
			// A refused reclaim is not automatically a refused join: the stored ticket may simply name a
			// hosted session that has ended. One fallback attempt, then a refusal is a refusal.
			if (m_ReconnectClient && m_State == NetSessionState::Accepted && m_ReconnectClient->AbsorbRejection(m_NowMs, rejected->rejectReason)) {
				FlushReconnectOutbound();
				return;
			}
			SetRejected(rejected->rejectReason, rejected->mismatchKey, rejected->expected, rejected->actual, rejected->humanMessage);
			m_Transport->Disconnect(peerId, rejected->humanMessage);
			return;
		}
		if (const auto* hostHello = std::get_if<NetHostHello>(&message.payload)) {
			const NetIdentityMismatch mismatch = ValidateHostHello(*hostHello);
			if (HasMismatch(mismatch)) {
				SetRejected(mismatch.rejectReason, mismatch.key, mismatch.expectedShortValue, mismatch.actualShortValue, mismatch.summary);
				Send(peerId, NetDisconnect{static_cast<uint16_t>(mismatch.rejectReason), mismatch.summary});
				m_Transport->Disconnect(peerId, mismatch.summary);
				return;
			}
			m_SessionId = hostHello->sessionId;
			m_RemoteIdentityHash = hostHello->sessionIdentityHash;
			m_HasRemoteIdentityHash = true;
			return;
		}
		if (const auto* accepted = std::get_if<NetJoinAccepted>(&message.payload)) {
			if (accepted->sessionId == 0 || accepted->selectedProtocolVersion != NetProtocol::c_Version || accepted->assignedPeerId == 0) {
				SetRejected(NetRejectReason::ProtocolMismatch, "join_accepted", "valid JoinAccepted", "invalid", "invalid JoinAccepted");
				m_Transport->Disconnect(peerId, "invalid JoinAccepted");
				return;
			}
			m_SessionId = accepted->sessionId;
			m_LocalPeerId = accepted->assignedPeerId;
			m_Config.heartbeatIntervalMs = accepted->heartbeatIntervalMs;
			m_Config.timeoutMs = accepted->timeoutMs;
			// §4 expands the handshake: with an admission plane attached, Ready waits for JoinCommitted,
			// which is also what hands back the seat's own peer id instead of this freshly allocated one.
			if (m_ReconnectClient && m_ReconnectClient->BeginAdmission(m_NowMs)) {
				m_State = NetSessionState::Accepted;
				m_StateStartedMs = m_NowMs;
				FlushReconnectOutbound();
				return;
			}
			Send(peerId, BuildReadyState(true));
			m_State = NetSessionState::Ready;
			m_StateStartedMs = m_NowMs;
			m_NextHeartbeatMs = m_NowMs;
			return;
		}
		if (std::holds_alternative<NetHeartbeat>(message.payload)) {
			m_LastReceiveMs = m_NowMs;
			return;
		}
		if (RouteAdmissionMessage(peerId, message.payload)) {
			return;
		}
		if (const auto* disconnect = std::get_if<NetDisconnect>(&message.payload)) {
			if (m_ReconnectClient && disconnect->disconnectReason == static_cast<uint16_t>(NetRejectReason::SessionEnded)) {
				// The one signal, other than a LeaveAck, that lets the recovery record be deleted.
				m_ReconnectClient->NotifyConfirmedSessionEnd();
			}
			if (m_State != NetSessionState::Rejected && m_State != NetSessionState::Failed) {
				if (!m_HasReject && !disconnect->message.empty()) {
					RecordReject(NetRejectReason::InternalError, "", "", "", disconnect->message);
				}
				m_State = NetSessionState::Closed;
			}
			return;
		}
	}

	void NetSession::CheckTimeouts() {
		if (m_Config.timeoutMs == 0) {
			return;
		}
		if (m_Role == NetSessionRole::Host) {
			for (PeerState& peer : m_Peers) {
				if (!IsActive(peer.state)) {
					continue;
				}
				if (m_NowMs >= peer.lastReceiveMs && m_NowMs - peer.lastReceiveMs > m_Config.timeoutMs) {
					++m_Stats.timeouts;
					if (peer.state == NetSessionState::Handshake) {
						RejectPeer(peer, NetRejectReason::Timeout, "timeout_ms", std::to_string(m_Config.timeoutMs), std::to_string(m_NowMs - peer.lastReceiveMs), "client hello timeout");
					} else {
						Send(peer.transportPeerId, NetDisconnect{static_cast<uint16_t>(NetRejectReason::Timeout), "heartbeat timeout"});
						DropPeerTransport(peer.transportPeerId, "heartbeat timeout");
						peer.state = NetSessionState::Failed;
						RecordReject(NetRejectReason::Timeout, "timeout_ms", std::to_string(m_Config.timeoutMs), std::to_string(m_NowMs - peer.lastReceiveMs), "heartbeat timeout");
						RefreshHostState();
					}
				}
			}
		} else if ((m_State == NetSessionState::Connecting || m_State == NetSessionState::HelloSent || m_State == NetSessionState::Ready) &&
		           m_NowMs >= m_LastReceiveMs && m_NowMs - m_LastReceiveMs > m_Config.timeoutMs) {
			++m_Stats.timeouts;
			SetFailed(NetRejectReason::Timeout, "timeout_ms", std::to_string(m_Config.timeoutMs), std::to_string(m_NowMs - m_LastReceiveMs), "session timeout");
			if (m_RemoteTransportPeerId != c_InvalidNetPeerId) {
				m_Transport->Disconnect(m_RemoteTransportPeerId, "session timeout");
			}
		}
	}

	void NetSession::DropPeerTransport(NetPeerId peerId, const std::string& reason) {
		if (m_Role == NetSessionRole::Host && m_ReconnectHost) {
			// Fenced connections are handled inside: a superseded incarnation keeps the seat.
			(void)m_ReconnectHost->NotifyDisconnect(peerId, m_LockstepFrame);
		}
		if (m_Transport) {
			m_Transport->Disconnect(peerId, reason);
		}
	}

	void NetSession::RejectPeer(PeerState& peer, NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary) {
		RejectConnection(peer.transportPeerId, reason, key, expected, actual, summary);
		peer.state = NetSessionState::Rejected;
		RefreshHostState();
	}

	void NetSession::RejectConnection(NetPeerId peerId, NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary) {
		RecordReject(reason, key, expected, actual, summary);
		Send(peerId, NetJoinRejected{reason, summary, key, expected, actual});
		if (m_Transport) {
			// The close reason rides the transport too, so a peer that misses the reject packet still sees why.
			DropPeerTransport(peerId, BuildRejectText());
		}
	}

	void NetSession::RecordReject(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary) {
		m_RejectReason = reason;
		m_HasReject = true;
		m_MismatchKey = key;
		m_ExpectedValue = expected;
		m_ActualValue = actual;
		m_RejectSummary = summary;
	}

	void NetSession::SetRejected(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary) {
		RecordReject(reason, key, expected, actual, summary);
		m_State = NetSessionState::Rejected;
		m_StateStartedMs = m_NowMs;
	}

	void NetSession::SetFailed(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary) {
		RecordReject(reason, key, expected, actual, summary);
		m_State = NetSessionState::Failed;
		m_StateStartedMs = m_NowMs;
	}

	std::string NetSession::BuildRejectText() const {
		if (!m_HasReject) {
			return "";
		}
		// Long values are identity hashes; the leading bytes are enough to tell two apart on screen.
		auto shortValue = [](const std::string& value) {
			return value.size() > 12 ? value.substr(0, 8) + ".." : value;
		};
		std::string text = m_RejectSummary.empty() ? NetProtocol::RejectReasonName(m_RejectReason) : m_RejectSummary;
		if (!m_MismatchKey.empty()) {
			text += " (" + m_MismatchKey + ": " + shortValue(m_ExpectedValue) + " vs " + shortValue(m_ActualValue) + ")";
		}
		return text;
	}

	NetSession::PeerState* NetSession::FindPeer(NetPeerId peerId) {
		const auto it = std::find_if(m_Peers.begin(), m_Peers.end(), [peerId](const PeerState& peer) {
			return peer.transportPeerId == peerId;
		});
		return it == m_Peers.end() ? nullptr : &*it;
	}

	const NetSession::PeerState* NetSession::FindPeer(NetPeerId peerId) const {
		const auto it = std::find_if(m_Peers.begin(), m_Peers.end(), [peerId](const PeerState& peer) {
			return peer.transportPeerId == peerId;
		});
		return it == m_Peers.end() ? nullptr : &*it;
	}

	uint32_t NetSession::GetUnauthenticatedPeerCount() const {
		return static_cast<uint32_t>(std::count_if(m_Peers.begin(), m_Peers.end(), [](const PeerState& peer) {
			return peer.state == NetSessionState::Handshake;
		}));
	}

	uint32_t NetSession::ActivePeerCount() const {
		return static_cast<uint32_t>(std::count_if(m_Peers.begin(), m_Peers.end(), [](const PeerState& peer) {
			return IsActive(peer.state);
		}));
	}

	uint8_t NetSession::AllocatePendingAdmissionPeerId() const {
		if (m_Role != NetSessionRole::Host || m_ReconnectHost == nullptr || !m_ReconnectHost->IsLiveMatch()) {
			return 0;
		}
		for (uint16_t candidate = static_cast<uint16_t>(m_Config.maxPeers) + 1;
		     candidate <= static_cast<uint16_t>(m_Config.maxPeers) + c_MaxPendingAdmissions && candidate <= UINT8_MAX; ++candidate) {
			const bool used = std::any_of(m_Peers.begin(), m_Peers.end(), [candidate](const PeerState& peer) {
				return IsActive(peer.state) && peer.assignedPeerId == candidate;
			});
			if (!used) {
				return static_cast<uint8_t>(candidate);
			}
		}
		return 0;
	}

	uint8_t NetSession::AllocatePeerId() const {
		for (uint16_t candidate = 1; candidate <= m_Config.maxPeers; ++candidate) {
			const bool used = std::any_of(m_Peers.begin(), m_Peers.end(), [candidate](const PeerState& peer) {
				return IsActive(peer.state) && peer.assignedPeerId == candidate;
			});
			if (!used) {
				return static_cast<uint8_t>(candidate);
			}
		}
		return 0;
	}

	void NetSession::RefreshHostState() {
		if (m_Role != NetSessionRole::Host) {
			return;
		}
		const bool hasReady = std::any_of(m_Peers.begin(), m_Peers.end(), [](const PeerState& peer) {
			return peer.state == NetSessionState::Ready;
		});
		const bool hasAccepted = std::any_of(m_Peers.begin(), m_Peers.end(), [](const PeerState& peer) {
			return peer.state == NetSessionState::Accepted;
		});
		const bool hasHandshake = std::any_of(m_Peers.begin(), m_Peers.end(), [](const PeerState& peer) {
			return peer.state == NetSessionState::Handshake;
		});
		const NetSessionState nextState = hasReady ? NetSessionState::Ready :
		                                  hasAccepted ? NetSessionState::Accepted :
		                                  hasHandshake ? NetSessionState::Handshake :
		                                  NetSessionState::Listening;
		if (m_State != nextState) {
			m_State = nextState;
			m_StateStartedMs = m_NowMs;
		}
	}

	NetClientHello NetSession::BuildClientHello() const {
		NetClientHello hello;
		hello.clientNonce = m_Config.localNonce;
		hello.minProtocolVersion = m_Config.minProtocolVersion;
		hello.maxProtocolVersion = m_Config.maxProtocolVersion;
		hello.controllerFrameVersion = m_Config.localIdentity.controllerFrameVersion;
		hello.controllerFrameEncodedSize = m_Config.localIdentity.controllerFrameEncodedSize;
		hello.platformId = PlatformId(m_Config.localIdentity.platform);
		hello.displayName = m_Config.displayName;
		hello.gameVersion = m_Config.localIdentity.gameVersion;
		hello.buildId = m_Config.localIdentity.buildId;
		hello.deterministicConfigHash = m_Config.localIdentity.deterministicConfigHash;
		hello.moduleManifestHash = m_Config.localIdentity.moduleManifestHash;
		hello.sessionRulesHash = m_Config.localIdentity.sessionRulesHash;
		hello.sessionIdentityHash = m_Config.localIdentity.sessionIdentityHash;
		hello.hasUserdataModules = m_Config.localIdentity.hasUserdataModules;
		return hello;
	}

	NetHostHello NetSession::BuildHostHello(uint8_t assignedPeerId) const {
		NetHostHello hello;
		hello.sessionId = m_SessionId;
		hello.hostNonce = m_Config.localNonce;
		hello.selectedProtocolVersion = NetProtocol::c_Version;
		hello.controllerFrameVersion = m_Config.localIdentity.controllerFrameVersion;
		hello.controllerFrameEncodedSize = m_Config.localIdentity.controllerFrameEncodedSize;
		hello.assignedPeerId = assignedPeerId;
		hello.maxPeers = m_Config.maxPeers;
		hello.hostPlatformId = PlatformId(m_Config.localIdentity.platform);
		hello.gameVersion = m_Config.localIdentity.gameVersion;
		hello.hostName = m_Config.displayName;
		hello.buildId = m_Config.localIdentity.buildId;
		hello.deterministicConfigHash = m_Config.localIdentity.deterministicConfigHash;
		hello.moduleManifestHash = m_Config.localIdentity.moduleManifestHash;
		hello.sessionRulesHash = m_Config.localIdentity.sessionRulesHash;
		hello.sessionIdentityHash = m_Config.localIdentity.sessionIdentityHash;
		hello.hasUserdataModules = m_Config.localIdentity.hasUserdataModules;
		return hello;
	}

	NetJoinAccepted NetSession::BuildJoinAccepted(uint8_t assignedPeerId) const {
		NetJoinAccepted accepted;
		accepted.sessionId = m_SessionId;
		accepted.assignedPeerId = assignedPeerId;
		accepted.maxPeers = m_Config.maxPeers;
		accepted.selectedProtocolVersion = NetProtocol::c_Version;
		accepted.heartbeatIntervalMs = m_Config.heartbeatIntervalMs;
		accepted.timeoutMs = m_Config.timeoutMs;
		return accepted;
	}

	NetReadyState NetSession::BuildReadyState(bool ready) const {
		NetReadyState state;
		state.peerId = m_LocalPeerId;
		state.ready = ready;
		state.deterministicConfigHash = m_Config.localIdentity.deterministicConfigHash;
		state.moduleManifestHash = m_Config.localIdentity.moduleManifestHash;
		return state;
	}

	NetIdentityMismatch NetSession::ValidateClientHello(const NetClientHello& hello) const {
		if (hello.minProtocolVersion > NetProtocol::c_Version || hello.maxProtocolVersion < NetProtocol::c_Version) {
			return MakeMismatch("network_protocol_version", NetRejectReason::ProtocolMismatch, std::to_string(NetProtocol::c_Version), VersionRange(hello.minProtocolVersion, hello.maxProtocolVersion), "network protocol version does not match");
		}
		if (hello.gameVersion != m_Config.localIdentity.gameVersion) {
			return MakeMismatch("game_version", NetRejectReason::GameVersionMismatch, m_Config.localIdentity.gameVersion, hello.gameVersion, "game version does not match");
		}
		if (hello.buildId != m_Config.localIdentity.buildId) {
			return MakeMismatch("build_id", NetRejectReason::BuildMismatch, m_Config.localIdentity.buildId, hello.buildId, "build id does not match");
		}
		if (hello.controllerFrameVersion != m_Config.localIdentity.controllerFrameVersion) {
			return MakeMismatch("controller_frame_version", NetRejectReason::ControllerFrameVersionMismatch, std::to_string(m_Config.localIdentity.controllerFrameVersion), std::to_string(hello.controllerFrameVersion), "ControllerFrame version does not match");
		}
		if (hello.controllerFrameEncodedSize != m_Config.localIdentity.controllerFrameEncodedSize) {
			return MakeMismatch("controller_frame_encoded_size", NetRejectReason::ControllerFrameSizeMismatch, std::to_string(m_Config.localIdentity.controllerFrameEncodedSize), std::to_string(hello.controllerFrameEncodedSize), "ControllerFrame encoded size does not match");
		}
		if (hello.deterministicConfigHash != m_Config.localIdentity.deterministicConfigHash) {
			return MakeMismatch("deterministic_config_hash", NetRejectReason::DeterministicConfigMismatch, HashText(m_Config.localIdentity.deterministicConfigHash), HashText(hello.deterministicConfigHash), "deterministic config hash does not match");
		}
		if (m_Config.rejectUserdataModules && hello.hasUserdataModules) {
			return MakeMismatch("userdata_modules", NetRejectReason::UserdataModulesNotAllowed, "false", "true", "userdata modules are not allowed in network sessions");
		}
		if (hello.moduleManifestHash != m_Config.localIdentity.moduleManifestHash) {
			return MakeMismatch("module_manifest_hash", NetRejectReason::ModuleManifestMismatch, HashText(m_Config.localIdentity.moduleManifestHash), HashText(hello.moduleManifestHash), "module manifest hash does not match");
		}
		if (hello.sessionRulesHash != m_Config.localIdentity.sessionRulesHash) {
			return MakeMismatch("session_rules_hash", NetRejectReason::SessionRulesMismatch, HashText(m_Config.localIdentity.sessionRulesHash), HashText(hello.sessionRulesHash), "session rules hash does not match");
		}
		if (hello.sessionIdentityHash != m_Config.localIdentity.sessionIdentityHash) {
			return MakeMismatch("session_identity_hash", NetRejectReason::BuildMismatch, HashText(m_Config.localIdentity.sessionIdentityHash), HashText(hello.sessionIdentityHash), "session identity hash does not match");
		}
		return NoMismatch();
	}

	NetIdentityMismatch NetSession::ValidateHostHello(const NetHostHello& hello) const {
		if (hello.selectedProtocolVersion != NetProtocol::c_Version) {
			return MakeMismatch("network_protocol_version", NetRejectReason::ProtocolMismatch, std::to_string(NetProtocol::c_Version), std::to_string(hello.selectedProtocolVersion), "selected protocol version does not match");
		}
		if (hello.gameVersion != m_Config.localIdentity.gameVersion) {
			return MakeMismatch("game_version", NetRejectReason::GameVersionMismatch, m_Config.localIdentity.gameVersion, hello.gameVersion, "game version does not match");
		}
		if (hello.buildId != m_Config.localIdentity.buildId) {
			return MakeMismatch("build_id", NetRejectReason::BuildMismatch, m_Config.localIdentity.buildId, hello.buildId, "build id does not match");
		}
		if (hello.controllerFrameVersion != m_Config.localIdentity.controllerFrameVersion) {
			return MakeMismatch("controller_frame_version", NetRejectReason::ControllerFrameVersionMismatch, std::to_string(m_Config.localIdentity.controllerFrameVersion), std::to_string(hello.controllerFrameVersion), "ControllerFrame version does not match");
		}
		if (hello.controllerFrameEncodedSize != m_Config.localIdentity.controllerFrameEncodedSize) {
			return MakeMismatch("controller_frame_encoded_size", NetRejectReason::ControllerFrameSizeMismatch, std::to_string(m_Config.localIdentity.controllerFrameEncodedSize), std::to_string(hello.controllerFrameEncodedSize), "ControllerFrame encoded size does not match");
		}
		if (hello.deterministicConfigHash != m_Config.localIdentity.deterministicConfigHash) {
			return MakeMismatch("deterministic_config_hash", NetRejectReason::DeterministicConfigMismatch, HashText(m_Config.localIdentity.deterministicConfigHash), HashText(hello.deterministicConfigHash), "deterministic config hash does not match");
		}
		if (m_Config.rejectUserdataModules && hello.hasUserdataModules) {
			return MakeMismatch("userdata_modules", NetRejectReason::UserdataModulesNotAllowed, "false", "true", "userdata modules are not allowed in network sessions");
		}
		if (hello.moduleManifestHash != m_Config.localIdentity.moduleManifestHash) {
			return MakeMismatch("module_manifest_hash", NetRejectReason::ModuleManifestMismatch, HashText(m_Config.localIdentity.moduleManifestHash), HashText(hello.moduleManifestHash), "module manifest hash does not match");
		}
		if (hello.sessionRulesHash != m_Config.localIdentity.sessionRulesHash) {
			return MakeMismatch("session_rules_hash", NetRejectReason::SessionRulesMismatch, HashText(m_Config.localIdentity.sessionRulesHash), HashText(hello.sessionRulesHash), "session rules hash does not match");
		}
		if (hello.sessionIdentityHash != m_Config.localIdentity.sessionIdentityHash) {
			return MakeMismatch("session_identity_hash", NetRejectReason::BuildMismatch, HashText(m_Config.localIdentity.sessionIdentityHash), HashText(hello.sessionIdentityHash), "session identity hash does not match");
		}
		return NoMismatch();
	}

	uint8_t NetSession::PlatformId(const std::string& platform) {
		if (platform == "windows") return 1;
		if (platform == "linux") return 2;
		if (platform == "macos") return 3;
		return 0;
	}

	std::string NetSession::BuildReportJson() const {
		json peers = json::array();
		if (m_Role == NetSessionRole::Host) {
			for (const PeerState& peer : m_Peers) {
				peers.push_back(json{
					{"peer_id", peer.assignedPeerId},
					{"transport_peer_id", peer.transportPeerId},
					{"display_name", peer.displayName},
					{"state", StateName(peer.state)},
					{"last_heartbeat_ms", peer.lastHeartbeatMs},
					{"identity_hash", HashJson(peer.identityHash, peer.clientNonce != 0)},
				});
			}
		} else if (m_RemoteTransportPeerId != c_InvalidNetPeerId) {
			peers.push_back(json{
				{"peer_id", c_HostAssignedPeerId},
				{"transport_peer_id", m_RemoteTransportPeerId},
				{"display_name", "Host"},
				{"state", StateName(m_State)},
				{"last_heartbeat_ms", m_LastReceiveMs},
				{"identity_hash", HashJson(m_RemoteIdentityHash, m_HasRemoteIdentityHash)},
			});
		}

		// The reconnect counters the 9a gates read: nothing here is a secret, only how many of each
		// kind of admission event the host saw.
		json admission = json::object();
		if (m_ReconnectHost) {
			const NetReconnectHostStats& stats = m_ReconnectHost->GetStats();
			admission = json{
				{"new_joins", stats.newJoins},
				{"ticket_offers_sent", stats.ticketOffersSent},
				{"ticket_offer_retransmits", stats.ticketOfferRetransmits},
				{"provisional_seats_opened", stats.provisionalSeatsOpened},
				{"provisional_seats_committed", stats.provisionalSeatsCommitted},
				{"provisional_seats_expired", stats.provisionalSeatsExpired},
				{"provisional_seats_refused", stats.provisionalSeatsRefused},
				{"provisional_seats_resumed", stats.provisionalSeatsResumed},
				{"persistence_failures", stats.persistenceFailures},
				{"reclaims_accepted", stats.reclaimsAccepted},
				{"identity_rejections", stats.identityRejections},
				{"denials_scheduled", stats.denialsScheduled},
				{"denials_released", stats.denialsReleased},
				{"replayed_results", stats.replayedResults},
				{"stale_epoch_drops", stats.staleEpochDrops},
				{"unknown_transaction_drops", stats.unknownTransactionDrops},
				{"fenced_packets", stats.fencedPackets},
				{"fenced_disconnects", stats.fencedDisconnects},
				{"incarnations_bound", stats.incarnationsBound},
				{"seats_dropped", stats.seatsDropped},
				{"seats_closed_by_leave", stats.seatsClosedByLeave},
				{"ledger_drops_recorded", stats.ledgerDropsRecorded},
				{"reseats_issued", stats.reseatsIssued},
				{"reclaim_retransmits_dropped", stats.reclaimRetransmitsDropped},
				{"seat_holds_expired", stats.seatHoldsExpired},
				{"seats_released_in_lobby", stats.seatsReleasedInLobby},
				{"applicants_registered", stats.applicantsRegistered},
				{"applicants_refused", stats.applicantsRefused},
				{"applicants_expired", stats.applicantsExpired},
				{"applicants_displaced", stats.applicantsDisplaced},
				{"substitution_offers_sent", stats.substitutionOffersSent},
				{"substitution_offer_retransmits", stats.substitutionOfferRetransmits},
				{"substitutions_committed", stats.substitutionsCommitted},
				{"substitutions_cancelled", stats.substitutionsCancelled},
				{"substitutions_superseded", stats.substitutionsSuperseded},
				{"substitution_ack_failures", stats.substitutionAckFailures},
				{"reassigned_reclaims_refused", stats.reassignedReclaimsRefused},
				{"pending_applicants", m_ReconnectHost->GetApplicantCount()},
				{"outstanding_challenges", m_ReconnectHost->GetAdmission().GetOutstandingChallengeCount()},
				{"synthetic_challenges", m_ReconnectHost->GetAdmission().GetSyntheticChallenges()},
				{"rate_limited_attempts", m_ReconnectHost->GetAdmission().GetRateLimitedAttempts()},
				{"ledger_seats", m_ReconnectHost->GetLedger().Size()},
			};
		}

		json report{
			{"schema", 1},
			{"session_id", std::to_string(m_SessionId)},
			{"role", RoleName(m_Role)},
			{"final_state", StateName(m_State)},
			{"accepted", m_State == NetSessionState::Accepted || m_State == NetSessionState::Ready},
			{"rejected", m_State == NetSessionState::Rejected},
			{"reject_reason", m_HasReject ? NetProtocol::RejectReasonName(m_RejectReason) : ""},
			{"mismatch_key", m_MismatchKey},
			{"expected", m_ExpectedValue},
			{"actual", m_ActualValue},
			{"summary", m_RejectSummary},
			{"local_identity_hash", HashText(m_Config.localIdentity.sessionIdentityHash)},
			{"local_identity", {
				{"build_id", m_Config.localIdentity.buildId},
				{"deterministic_config_hash", HashText(m_Config.localIdentity.deterministicConfigHash)},
				{"module_manifest_hash", HashText(m_Config.localIdentity.moduleManifestHash)},
				{"session_rules_hash", HashText(m_Config.localIdentity.sessionRulesHash)},
				{"has_userdata_modules", m_Config.localIdentity.hasUserdataModules},
				{"num_lua_states", m_Config.localIdentity.deterministicConfig.numLuaStates},
				{"num_lua_states_override", m_Config.localIdentity.deterministicConfig.numLuaStatesOverride},
				{"selected_module", m_Config.localIdentity.deterministicConfig.selectedModule},
			}},
			{"remote_identity_hash", HashJson(m_RemoteIdentityHash, m_HasRemoteIdentityHash)},
			{"peers", peers},
			{"admission", admission},
			{"stats", {
				{"sent_messages", m_Stats.sentMessages},
				{"received_messages", m_Stats.receivedMessages},
				{"malformed_messages", m_Stats.malformedMessages},
				{"ignored_phase_packets", m_Stats.ignoredPhasePackets},
				{"timeouts", m_Stats.timeouts},
				{"unbound_connection_faults", m_Stats.unboundConnectionFaults},
				{"unauthenticated_connections_refused", m_Stats.unauthenticatedConnectionsRefused},
				{"fenced_packets", m_Stats.fencedPackets},
				{"fenced_disconnects", m_Stats.fencedDisconnects},
				{"admission_messages", m_Stats.admissionMessages},
				{"old_wire_rejections_sent", m_Stats.oldWireRejectionsSent},
				{"old_wire_disconnects", m_Stats.oldWireDisconnects},
				{"pending_admission_joins", m_Stats.pendingAdmissionJoins},
			}},
		};
		return report.dump(2);
	}

	const char* NetSession::RoleName(NetSessionRole role) {
		switch (role) {
			case NetSessionRole::None: return "none";
			case NetSessionRole::Host: return "host";
			case NetSessionRole::Client: return "client";
		}
		return "unknown";
	}

	const char* NetSession::StateName(NetSessionState state) {
		switch (state) {
			case NetSessionState::Stopped: return "Stopped";
			case NetSessionState::Listening: return "Listening";
			case NetSessionState::Connecting: return "Connecting";
			case NetSessionState::HelloSent: return "HelloSent";
			case NetSessionState::Handshake: return "Handshake";
			case NetSessionState::Accepted: return "Accepted";
			case NetSessionState::Ready: return "Ready";
			case NetSessionState::Rejected: return "Rejected";
			case NetSessionState::Closed: return "Closed";
			case NetSessionState::Failed: return "Failed";
		}
		return "Unknown";
	}

} // namespace RTE
