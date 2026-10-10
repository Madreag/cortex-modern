#include "NetConnectionAuthority.h"
#include "GnsTransport.h"

#include "Base64/base64.h"
#include "NetAuthCrypto.h"
#include "NetDirectoryCodec.h"
#include "NetHttpClient.h"
#include "NetLanDiscovery.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace RTE {
	namespace {
		using json = nlohmann::json;
		constexpr char c_TokenDomain[] = "CortexSeatToken1";
		constexpr char c_RequestDomain[] = "CortexCheckIn1";
		constexpr size_t c_FieldsBytes = 130;
		constexpr const char* c_Waiting = "The directory is unavailable. Reconnecting will retry; Cancel returns to Multiplayer.";

		std::string Hex(const uint8_t* bytes, size_t size) {
			constexpr char digits[] = "0123456789abcdef";
			std::string out(size * 2, '0');
			for (size_t i = 0; i < size; ++i) { out[2 * i] = digits[bytes[i] >> 4]; out[2 * i + 1] = digits[bytes[i] & 15]; }
			return out;
		}
		template<size_t N> std::string Hex(const std::array<uint8_t, N>& value) { return Hex(value.data(), value.size()); }
		template<size_t N> bool Unhex(const std::string& text, std::array<uint8_t, N>& out) {
			if (text.size() != N * 2) return false;
			auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
			for (size_t i = 0; i < N; ++i) {
				const int a = nibble(text[2 * i]), b = nibble(text[2 * i + 1]);
				if (a < 0 || b < 0) return false;
				out[i] = static_cast<uint8_t>((a << 4) | b);
			}
			return true;
		}
		bool UuidBytes(const std::string& text, NetAuthBytes16& out) {
			out = {};
			if (text.empty()) return true;
			if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') return false;
			std::string compact;
			for (char c: text) if (c != '-') compact += c;
			return Unhex(compact, out);
		}
		std::string UuidText(const NetAuthBytes16& bytes) {
			if (bytes == NetAuthBytes16{}) return {};
			const std::string hex = Hex(bytes);
			return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
		}
		void Append(std::vector<uint8_t>& out, uint64_t value, size_t bytes) { for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i))); }
		template<size_t N> void Append(std::vector<uint8_t>& out, const std::array<uint8_t, N>& bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }
		uint64_t Read(const std::string& raw, size_t& offset, size_t count) {
			uint64_t value = 0;
			for (size_t i = 0; i < count; ++i) value |= static_cast<uint64_t>(static_cast<uint8_t>(raw[offset++])) << (8 * i);
			return value;
		}
		template<size_t N> void Read(const std::string& raw, size_t& offset, std::array<uint8_t, N>& bytes) { std::memcpy(bytes.data(), raw.data() + offset, N); offset += N; }
		bool Fields(const NetSeatLease& lease, std::vector<uint8_t>& bytes) {
			NetAuthBytes16 directory{};
			if (!UuidBytes(lease.directorySessionId, directory) || lease.generation == 0 || lease.seat > NetSeatLease::c_MaxStableSeat || lease.hostSessionId == 0 ||
			    lease.epoch == NetAuthBytes16{} || lease.participant == NetParticipantId{} || lease.credential == NetAuthBytes32{} ||
			    lease.expiresAt <= lease.issuedAt || lease.expiresAt - lease.issuedAt > NetSeatLease::c_LifetimeSeconds) return false;
			bytes.clear(); bytes.reserve(c_FieldsBytes);
			Append(bytes, NetSeatLease::c_Version, 2); Append(bytes, NetProtocol::c_Version, 2);
			Append(bytes, directory); Append(bytes, lease.epoch); Append(bytes, lease.hostSessionId, 8); Append(bytes, lease.seat, 2);
			Append(bytes, lease.generation, 4); Append(bytes, lease.participant); Append(bytes, lease.issuedAt, 8); Append(bytes, lease.expiresAt, 8); Append(bytes, lease.credential);
			return bytes.size() == c_FieldsBytes;
		}
		bool Protocol(const json& body, std::string& error) {
			const auto version = body.find("connection_protocol");
			const int64_t actual = version != body.end() && version->is_number_integer() ? version->get<int64_t>() : 0;
			if (actual == NetSeatLease::c_Version) return true;
			error = "Your connection protocol is " + std::to_string(NetSeatLease::c_Version) + "; the directory uses " + std::to_string(actual) + ". Update the game or directory so the versions match.";
			return false;
		}
		std::string ReplyError(const NetHttpClient::Response& response, bool& refused, bool& ended) {
			refused = false;
			ended = false;
			const auto body = json::parse(response.body, nullptr, false);
			if (body.is_object()) {
				const auto field = body.find("error");
				const std::string code = field != body.end() && field->is_string() ? field->get<std::string>() : std::string();
				if (code == "connection_version") {
					refused = true;
					const auto version = body.find("directory_version");
					const int64_t remote = version != body.end() && version->is_number_integer() ? version->get<int64_t>() : 0;
					return "Your connection protocol is " + std::to_string(NetSeatLease::c_Version) + "; the directory uses " + std::to_string(remote) + ". Update the game or directory so the versions match.";
				}
				if (code == "match_ended") { refused = ended = true; return "The host ended this match. Choose another game."; }
				if (response.statusCode == 404 && code == "not_found") {
					refused = true;
					return "Your connection protocol is " + std::to_string(NetSeatLease::c_Version) + "; this directory uses legacy protocol 0. Update the directory before joining Internet games.";
				}
				if (code == "seat_taken" || code == "player_removed" || code == "seat_reassigned" || code == "seat_expired" || code == "connection_clock") {
					refused = true;
					// These fields are authored by the directory and contain only bounded names and actions.
					const auto message = body.find("message");
					if (message != body.end() && message->is_string() && message->get_ref<const std::string&>().size() <= 256)
						return NetRelayLogins::Scrub(message->get<std::string>());
					return "This device cannot reclaim the seat. Join another open seat.";
				}
				if (code == "seat_in_use") {
					const auto message = body.find("message");
					if (message != body.end() && message->is_string() && message->get_ref<const std::string&>().size() <= 256)
						return NetRelayLogins::Scrub(message->get<std::string>());
					return "Your original connection is still active. Waiting for it to close; Cancel stops rejoining.";
				}
				if (code == "seat_unproven" || code == "player_unproven" || response.statusCode == 403) {
					refused = true; return "This device could not prove it owns the seat. Rejoin on the device that owns it.";
				}
			}
			return c_Waiting;
		}
	}

	bool NetSeatLease::SameSeat(const NetSeatLease& other) const {
		return directorySessionId == other.directorySessionId && epoch == other.epoch && hostSessionId == other.hostSessionId &&
		       seat == other.seat && generation == other.generation && participant == other.participant && credential == other.credential;
	}

	bool NetSeatLease::Decode(const std::string& token, const NetParticipantId& authority, NetSeatLease& out) {
		if (token.empty() || token.size() > c_MaxTokenBytes || authority == NetParticipantId{}) return false;
		std::string raw;
		try { raw = base64_decode(token); } catch (...) { return false; }
		if (raw.size() != c_FieldsBytes + 64 || base64_encode(raw) != token) return false;
		std::vector<uint8_t> signedBytes(std::begin(c_TokenDomain), std::end(c_TokenDomain));
		signedBytes.insert(signedBytes.end(), raw.begin(), raw.begin() + c_FieldsBytes);
		uint8_t key[32], signature[64];
		std::memcpy(key, authority.data(), 32); std::memcpy(signature, raw.data() + c_FieldsBytes, 64);
		if (!GetNetParticipantCrypto().Verify(key, signedBytes.data(), signedBytes.size(), signature)) return false;
		size_t offset = 0;
		if (Read(raw, offset, 2) != c_Version || Read(raw, offset, 2) != NetProtocol::c_Version) return false;
		NetSeatLease lease;
		NetAuthBytes16 directory{};
		Read(raw, offset, directory); Read(raw, offset, lease.epoch);
		lease.directorySessionId = UuidText(directory); lease.hostSessionId = Read(raw, offset, 8);
		lease.seat = static_cast<uint16_t>(Read(raw, offset, 2)); lease.generation = static_cast<uint32_t>(Read(raw, offset, 4));
		Read(raw, offset, lease.participant); lease.issuedAt = Read(raw, offset, 8); lease.expiresAt = Read(raw, offset, 8); Read(raw, offset, lease.credential);
		std::vector<uint8_t> canonical;
		if (!Fields(lease, canonical)) return false;
		lease.token = token; lease.authority = authority;
		NetRelayLogins::Remember(token);
		out = std::move(lease);
		return true;
	}

	NetConnectionAuthority::NetConnectionAuthority() = default;
	bool NetSeatLease::Sign(const NetAuthBytes32& matchSigningKey, NetSeatLease& lease) {
		std::vector<uint8_t> fields;
		if (matchSigningKey == NetAuthBytes32{} || !lease.directorySessionId.empty() || !Fields(lease, fields)) return false;
		std::vector<uint8_t> bytes(std::begin(c_TokenDomain), std::end(c_TokenDomain));
		bytes.insert(bytes.end(), fields.begin(), fields.end());
		uint8_t privateKey[32], publicKey[32], signature[64];
		std::memcpy(privateKey, matchSigningKey.data(), sizeof(privateKey));
		const bool signedLease = GetNetParticipantCrypto().PublicFromPrivate(privateKey, publicKey) &&
			GetNetParticipantCrypto().Sign(privateKey, bytes.data(), bytes.size(), signature);
		std::fill(std::begin(privateKey), std::end(privateKey), 0);
		if (!signedLease) return false;
		std::copy(std::begin(publicKey), std::end(publicKey), lease.authority.begin());
		fields.insert(fields.end(), std::begin(signature), std::end(signature));
		lease.token = base64_encode(fields.data(), fields.size());
		NetRelayLogins::Remember(lease.token);
		return true;
	}
	void NetConnectionAuthority::SetHostSigningKey(const NetAuthBytes32& key) {
		std::lock_guard lock(m_Mutex);
		m_HostSigningKey = key;
	}
	NetConnectionAuthority::~NetConnectionAuthority() = default;
	void NetConnectionAuthority::Suspend() {
		std::lock_guard lock(m_Mutex);
		m_CheckIn.reset(); m_Bootstrap.reset(); m_Operations.clear(); m_LocalLease.reset(); m_HostToken.clear();
		m_HostChange.reset(); m_HostChangeRequest.reset(); m_HostChangeReply = {}; m_HostChangeToken.clear();
		m_FrameTie.reset(); m_FrameTieRequest.reset(); m_FrameTieReply = {}; m_NextFrameTie = 0;
		m_HostSigningKey.fill(0);
		m_BootstrapWanted = false;
		m_Suspended = true;
	}
	void NetConnectionAuthority::Reset() {
		std::lock_guard lock(m_Mutex);
		m_Operations.clear(); m_CheckIn.reset(); m_Bootstrap.reset(); m_LocalLease.reset(); m_Host.reset();
		m_HostChange.reset(); m_HostChangeRequest.reset(); m_HostChangeReply = {}; m_HostChangeToken.clear(); m_NextHostChange = 0;
		m_FrameTie.reset(); m_FrameTieRequest.reset(); m_FrameTieReply = {}; m_NextFrameTie = 0;
		m_SessionId.clear(); m_HostToken.clear(); m_DirectoryKey = {}; m_Route = {}; m_Relay = {};
		m_HostSigningKey.fill(0);
		m_Removals.clear(); m_PeerRoutes.clear();
		m_HostGeneration = 0; m_NextCheckIn = m_NextBootstrap = m_CheckIns = 0; m_BootstrapWanted = m_Refused = m_Ended = false; m_Error.clear();
		m_CheckedGeneration = m_RelayGeneration = UINT32_MAX;
		m_Suspended = false;
	}
	void NetConnectionAuthority::Configure(NetParticipantIdentityStore* player, std::string baseUrl, std::string installKey, std::string certPin) {
		std::lock_guard lock(m_Mutex);
		if (!m_LanBrowser) m_LanBrowser = std::make_unique<NetLanDiscovery>();
		(void)m_LanBrowser->StartBrowser();
		m_Player = player; m_BaseUrl = std::move(baseUrl); m_InstallKey = std::move(installKey); m_CertPin = std::move(certPin);
		while (!m_BaseUrl.empty() && m_BaseUrl.back() == '/') m_BaseUrl.pop_back();
		if (!m_BaseUrl.empty() && !m_BaseUrl.starts_with("https://")) m_BaseUrl = "https://" + m_BaseUrl;
		if (m_Instance == NetAuthBytes16{}) (void)GetNetAuthCrypto().RandomBytes(m_Instance.data(), m_Instance.size());
	}
	void NetConnectionAuthority::SetDirectory(std::string sessionId, std::string hostToken, uint32_t hostGeneration, std::string authorityKey) {
		std::lock_guard lock(m_Mutex);
		if (m_SessionId != sessionId) {
			m_FrameTie.reset(); m_FrameTieRequest.reset(); m_FrameTieReply = {}; m_NextFrameTie = 0;
			m_HostChange.reset(); m_HostChangeRequest.reset(); m_HostChangeReply = {}; m_HostChangeToken.clear(); m_NextHostChange = 0;
			m_Host.reset(); m_Bootstrap.reset(); m_CheckIn.reset(); m_Operations.clear(); m_DirectoryKey = {}; m_Removals.clear(); m_PeerRoutes.clear(); m_Relay = {};
			if (m_LocalLease && m_LocalLease->directorySessionId != sessionId) m_LocalLease.reset();
			m_CheckIns = m_NextCheckIn = m_NextBootstrap = 0; m_BootstrapWanted = m_Refused = m_Ended = false; m_Error.clear();
			m_CheckedGeneration = m_RelayGeneration = UINT32_MAX;
		}
		if (!m_HostToken.empty() && hostToken.empty()) { m_Operations.clear(); m_Removals.clear(); }
		if (m_HostToken != hostToken || m_HostGeneration != hostGeneration)
			for (auto& [key, operation]: m_Operations) { (void)key; operation.error.clear(); operation.retryAt = 0; }
		m_SessionId = std::move(sessionId); m_HostToken = std::move(hostToken); m_HostGeneration = hostGeneration;
		if (!authorityKey.empty()) (void)Unhex(authorityKey, m_DirectoryKey);
	}
	std::string NetConnectionAuthority::DirectorySessionId() const { std::lock_guard lock(m_Mutex); return m_SessionId; }
	NetParticipantId NetConnectionAuthority::LocalParticipant() const { std::lock_guard lock(m_Mutex); return m_Player ? m_Player->PublicId() : NetParticipantId{}; }
	std::optional<NetSeatLease> NetConnectionAuthority::LocalLease() const { std::lock_guard lock(m_Mutex); return m_LocalLease; }
	std::optional<NetConnectionHost> NetConnectionAuthority::Host() const { std::lock_guard lock(m_Mutex); return m_Host; }
	NetRelayConfig NetConnectionAuthority::Relay() const { std::lock_guard lock(m_Mutex); return m_Relay; }
	std::string NetConnectionAuthority::Error() const { std::lock_guard lock(m_Mutex); return m_Error; }
	bool NetConnectionAuthority::Refused() const { std::lock_guard lock(m_Mutex); return m_Refused; }
	bool NetConnectionAuthority::Ended() const { std::lock_guard lock(m_Mutex); return m_Ended; }
	uint64_t NetConnectionAuthority::CheckIns() const { std::lock_guard lock(m_Mutex); return m_CheckIns; }
	bool NetConnectionAuthority::RouteCheckedIn() const { std::lock_guard lock(m_Mutex); return m_CheckIns != 0 && m_CheckedGeneration == m_Route.generation; }
	bool NetConnectionAuthority::RelayRefreshed() const { std::lock_guard lock(m_Mutex); return m_CheckIns != 0 && m_RelayGeneration == m_Route.generation; }
	uint32_t NetConnectionAuthority::NetworkRevision() const { std::lock_guard lock(m_Mutex); return m_NetworkRevision; }
	std::map<uint16_t, NetConnectionRoute> NetConnectionAuthority::PeerRoutes() const { std::lock_guard lock(m_Mutex); return m_PeerRoutes; }
	std::vector<std::string> NetConnectionAuthority::LocalAddresses() const { std::lock_guard lock(m_Mutex); return m_NetworkAddresses; }
	std::optional<NetConnectionRoute> NetConnectionAuthority::DirectHost(uint64_t matchId) const {
		std::lock_guard lock(m_Mutex);
		const auto found = m_DirectHosts.find(matchId);
		return found == m_DirectHosts.end() ? std::optional<NetConnectionRoute>{} : found->second;
	}
	void NetConnectionAuthority::CheckInNow() { std::lock_guard lock(m_Mutex); m_NextCheckIn = 0; }
	void NetConnectionAuthority::RefreshRoute() {
		std::lock_guard lock(m_Mutex);
		++m_Route.generation; m_NextCheckIn = m_NextBootstrap = 0;
		m_Host.reset(); m_BootstrapWanted = !m_SessionId.empty();
	}
	void NetConnectionAuthority::RequestBootstrap() { std::lock_guard lock(m_Mutex); m_BootstrapWanted = true; m_NextBootstrap = 0; }
	void NetConnectionAuthority::SetRoute(NetConnectionRoute route) {
		std::lock_guard lock(m_Mutex);
		route.generation = m_Route.generation;
		if (route.iceIdentity != m_Route.iceIdentity || route.iceVirtualPort != m_Route.iceVirtualPort ||
		    route.listenAddrs != m_Route.listenAddrs || route.listenPort != m_Route.listenPort) ++route.generation;
		if (m_Route != route) { m_Route = std::move(route); m_NextCheckIn = 0; }
	}
	bool NetConnectionAuthority::AdoptLocalLease(const NetSeatLease& lease) {
		std::lock_guard lock(m_Mutex);
		if (m_Suspended || !m_Player || lease.participant != m_Player->PublicId()) return false;
		if (!lease.directorySessionId.empty() && m_DirectoryKey != NetParticipantId{} && lease.authority != m_DirectoryKey) return false;
		if (m_LocalLease && m_LocalLease->token == lease.token && m_LocalLease->authority == lease.authority) return true;
		NetSeatLease verified;
		if (!NetSeatLease::Decode(lease.token, lease.authority, verified) || !verified.SameSeat(lease)) return false;
		if (m_LocalLease && m_LocalLease->SameSeat(lease) && m_LocalLease->issuedAt > lease.issuedAt) return true;
		m_LocalLease = lease; m_NextCheckIn = 0; m_Refused = false; m_Error.clear();
		return true;
	}
	bool NetConnectionAuthority::RestoreLease(const NetSeatLease& lease) {
		std::lock_guard lock(m_Mutex);
		NetSeatLease checked;
		if (!NetSeatLease::Decode(lease.token, lease.authority, checked) || !checked.SameSeat(lease) || lease.directorySessionId != m_SessionId ||
		    (m_DirectoryKey != NetParticipantId{} && lease.authority != m_DirectoryKey)) return false;
		m_DirectoryKey = lease.authority;
		m_Suspended = false;
		m_LocalLease = lease; m_NextCheckIn = 0; m_Refused = false; m_Error.clear();
		return true;
	}

	NetConnectionAuthority::Result NetConnectionAuthority::Issue(NetSeatLease wanted, const std::string& name, NetSeatLease& issued, std::string& error) {
		std::lock_guard lock(m_Mutex);
		if (m_Suspended) return Result::Pending;
		if (!m_Player || !m_Player->HasKey()) { error = "Your player key is unavailable. Restart the game and try again."; return Result::Refused; }
		if (wanted.directorySessionId.empty()) {
			if (!NetSeatLease::Sign(m_HostSigningKey, wanted)) { error = "The host could not sign this seat. Restart the host and try again."; return Result::Refused; }
			issued = std::move(wanted); return Result::Ready;
		}
		if (m_SessionId.empty() || m_HostToken.empty()) return Result::Pending;
		if (m_SessionId != wanted.directorySessionId) { error = "This join names another match. Cancel and choose the host's current game."; return Result::Refused; }
		const std::string key = Hex(wanted.epoch) + ":" + std::to_string(wanted.seat) + ":" + std::to_string(wanted.generation);
		std::erase_if(m_Operations, [&](const auto& item) {
			const auto& op = item.second;
			return !op.removal && !op.request && op.wanted.epoch == wanted.epoch && op.wanted.seat == wanted.seat && op.wanted.generation < wanted.generation;
		});
		auto found = m_Operations.find(key);
		if (found != m_Operations.end()) {
			Operation& op = found->second;
			if (!op.wanted.SameSeat(wanted)) { error = "This seat already belongs to another player. Join another open seat."; return Result::Refused; }
			if (!op.error.empty()) { error = op.error; return Result::Refused; }
			if (op.issued && op.issued->expiresAt > wanted.issuedAt + NetSeatLease::c_LifetimeSeconds / 2) { issued = *op.issued; return Result::Ready; }
			if (op.issued) { op.issued.reset(); op.retryAt = 0; }
			return Result::Pending;
		}
		if (m_Operations.size() >= 2 * (NetSeatLease::c_MaxStableSeat + 1)) return Result::Pending;
		Operation operation;
		operation.wanted = wanted;
		operation.body = json{{"connection_protocol", NetSeatLease::c_Version}, {"operation", "issue"}, {"token", m_HostToken}, {"host_generation", m_HostGeneration},
			{"epoch", Hex(wanted.epoch)}, {"seat", wanted.seat}, {"generation", wanted.generation}, {"credential", Hex(wanted.credential)},
			{"host_session", wanted.hostSessionId}, {"network_protocol", NetProtocol::c_Version}, {"participant", Hex(wanted.participant)}, {"name", name}}.dump();
		m_Operations.emplace(key, std::move(operation));
		return Result::Pending;
	}

	void NetConnectionAuthority::Remove(const NetParticipantId& player, const std::string& name, bool ban) {
		std::lock_guard lock(m_Mutex);
		if (m_SessionId.empty() || m_HostToken.empty()) return;
		const std::string key = "remove:" + Hex(player);
		if (!m_Removals.insert(key).second) return;
		Operation operation; operation.removal = true;
		operation.body = json{{"connection_protocol", NetSeatLease::c_Version}, {"operation", "remove"}, {"token", m_HostToken}, {"host_generation", m_HostGeneration},
			{"participant", Hex(player)}, {"name", name}, {"action", ban ? "banned" : "removed"}}.dump();
		m_Operations[key] = std::move(operation);
	}

	std::unique_ptr<NetHttpClient> NetConnectionAuthority::StartRequest(const std::string& method, const std::string& body, int timeoutMs) {
		if (m_BaseUrl.empty() || m_SessionId.empty()) return nullptr;
		auto request = std::make_unique<NetHttpClient>();
		request->Start(method, m_BaseUrl + "/v1/sessions/" + m_SessionId + "/connections", {{"Content-Type", "application/json"}, {"X-Install-Key", m_InstallKey},
			{"X-Connection-Protocol", std::to_string(NetSeatLease::c_Version)}}, body, m_CertPin, timeoutMs);
		return request;
	}
	bool NetConnectionAuthority::ReadLeaseReply(const std::string& text, NetSeatLease& lease, std::string& error) {
		try {
			const auto body = json::parse(text);
			if (!Protocol(body, error)) return false;
			NetParticipantId key{};
			if (!Unhex(body.at("authority_key").get<std::string>(), key) || (m_DirectoryKey != NetParticipantId{} && key != m_DirectoryKey) ||
			    !NetSeatLease::Decode(body.at("seat_token").get<std::string>(), key, lease) || lease.directorySessionId != m_SessionId) return false;
			m_DirectoryKey = key;
			return true;
		} catch (const json::exception&) { return false; }
	}
	bool NetConnectionAuthority::ReadHostReply(const std::string& text, NetConnectionHost& host, std::string& error) {
		try {
			const auto body = json::parse(text);
			if (!Protocol(body, error)) return false;
			if (!NetDirectoryCodec::DecodeSessionRow(body.at("session").dump(), host.row, error) || host.row.sessionId != m_SessionId) return false;
			const auto protocol = host.row.networkProtocolVersion;
			if (protocol != NetProtocol::c_Version) {
				error = "Your network protocol is " + std::to_string(NetProtocol::c_Version) + "; the host uses " + std::to_string(protocol) + ". Update both games to the same version.";
				return false;
			}
			if (!Unhex(body.at("authority_key").get<std::string>(), host.authority) || (m_DirectoryKey != NetParticipantId{} && m_DirectoryKey != host.authority)) return false;
			const auto generation = body.at("host_generation");
			if (!generation.is_number_unsigned() || generation.get<uint64_t>() > UINT32_MAX) return false;
			host.generation = generation.get<uint32_t>();
			host.relayEnabled = body.at("relay_enabled").get<bool>();
			m_DirectoryKey = host.authority;
			return true;
		} catch (const json::exception&) { return false; }
	}

	void NetConnectionAuthority::PollOperations(uint64_t steadyMs) {
		size_t active = 0;
		for (auto it = m_Operations.begin(); it != m_Operations.end();) {
			Operation& op = it->second;
			if (op.request) {
				if (op.request->Poll() == NetHttpClient::PollResult::Pending) { ++active; ++it; continue; }
				const auto response = op.request->GetResponse(); op.request.reset();
				if (response.statusCode == 200) {
					if (op.removal) { it = m_Operations.erase(it); continue; }
					NetSeatLease lease;
					if (ReadLeaseReply(response.body, lease, op.error) && lease.SameSeat(op.wanted)) op.issued = std::move(lease);
					else if (op.error.empty()) op.error = "The directory returned a different seat. Cancel and rejoin the host's game.";
				} else {
					bool refused = false, ended = false;
					const std::string error = ReplyError(response, refused, ended);
					if (refused) op.error = error;
					else { op.retryAt = steadyMs + 5000; m_Error = error; }
				}
			}
			if (!op.request && !op.issued && op.error.empty() && steadyMs >= op.retryAt && active < 2) {
				// Host migration may rotate the directory token while this request waits.
				auto body = json::parse(op.body); body["token"] = m_HostToken; body["host_generation"] = m_HostGeneration;
				op.request = StartRequest("POST", body.dump()); if (op.request) ++active;
			}
			++it;
		}
	}
	void NetConnectionAuthority::PollBootstrap(uint64_t steadyMs) {
		if (m_Bootstrap) {
			if (m_Bootstrap->Poll() == NetHttpClient::PollResult::Pending) return;
			const auto response = m_Bootstrap->GetResponse(); m_Bootstrap.reset();
			if (response.statusCode == 200) {
				NetConnectionHost host; std::string error;
				if (ReadHostReply(response.body, host, error)) { m_Host = std::move(host); m_BootstrapWanted = false; m_Error.clear(); }
				else { m_Refused = true; m_Error = error.empty() ? "The directory's connection reply was invalid. Update the game and directory." : error; }
			} else m_Error = ReplyError(response, m_Refused, m_Ended);
			m_NextBootstrap = steadyMs + 1000;
		}
		if (m_BootstrapWanted && !m_Bootstrap && !m_Refused && steadyMs >= m_NextBootstrap) m_Bootstrap = StartRequest("GET", "");
	}
	std::string NetConnectionAuthority::SignedSeatRequest(const std::string& operation, uint64_t unixSeconds, const NetHostChangeRequest* change, const NetFrameTieRequest* tie) {
		NetAuthBytes16 nonce{};
		if (!GetNetAuthCrypto().RandomBytes(nonce.data(), nonce.size())) return {};
		json contents{{"session_id", m_SessionId}, {"seat_token", m_LocalLease->token}, {"nonce", Hex(nonce)}, {"instance", Hex(m_Instance)}, {"sent_at", unixSeconds},
			{"route", {{"ice_identity", m_Route.iceIdentity}, {"ice_virtual_port", m_Route.iceVirtualPort}, {"listen_port", m_Route.listenPort}, {"listen_addrs", m_Route.listenAddrs}, {"generation", m_Route.generation}, {"state", m_Route.state}}}};
		if (!change && !m_HostToken.empty()) contents["host"] = {{"token", m_HostToken}, {"generation", m_HostGeneration}};
		if (change) {
			contents["host_change"] = {{"generation", change->generation}, {"round_id", change->roundId},
				{"applied_frame", change->appliedFrame}, {"prepared_frame", change->preparedFrame}, {"config_hash", Hex(change->configHash)}};
			if (change->agreedHost != UINT16_MAX)
				contents["host_change"]["agreement"] = {{"host_seat", change->agreedHost}, {"boundary", change->agreedBoundary}, {"members", change->agreedMembers}};
		}
		if (tie) contents["frame_tie"] = {{"generation", tie->generation}, {"round_id", tie->roundId}, {"frame", tie->frame},
			{"config_hash", Hex(tie->configHash)}, {"host_seat", tie->host}, {"owners", tie->owners}, {"members", tie->members}, {"query_only", tie->queryOnly}};
		const std::string request = contents.dump();
		std::vector<uint8_t> message(std::begin(c_RequestDomain), std::end(c_RequestDomain)); message.insert(message.end(), request.begin(), request.end());
		NetParticipantSignature signature{};
		if (!m_Player->Sign(message, signature)) { m_Error = "Your player key is unavailable. Restart the game and rejoin."; m_Refused = true; return {}; }
		return json{{"connection_protocol", NetSeatLease::c_Version}, {"operation", operation}, {"participant", Hex(m_Player->PublicId())}, {"signature", Hex(signature)}, {"signed_request", base64_encode(request)}}.dump();
	}

	void NetConnectionAuthority::PollCheckIn(uint64_t steadyMs, uint64_t unixSeconds) {
		if (!m_LocalLease || m_LocalLease->directorySessionId.empty() || m_LocalLease->directorySessionId != m_SessionId || !m_Player || m_Instance == NetAuthBytes16{}) return;
		if (m_CheckIn) {
			if (m_CheckIn->Poll() == NetHttpClient::PollResult::Pending) return;
			const auto response = m_CheckIn->GetResponse(); m_CheckIn.reset();
			m_NextCheckIn = steadyMs + 5000;
			if (response.statusCode == 200) {
				NetSeatLease lease; std::string error;
				if (!ReadLeaseReply(response.body, lease, error) || !lease.SameSeat(*m_LocalLease) || lease.participant != m_Player->PublicId()) { m_Error = error.empty() ? "The directory could not renew this seat. Cancel and rejoin your match." : error; m_Refused = true; return; }
				m_LocalLease = std::move(lease); ++m_CheckIns; m_CheckedGeneration = m_CheckInGeneration; m_Error.clear();
				try {
					const auto body = json::parse(response.body);
					if (body.contains("host")) { NetConnectionHost host; if (ReadHostReply(body.at("host").dump(), host, error)) m_Host = std::move(host); else { m_Error = error; m_Refused = !error.empty(); } }
					if (body.contains("relay")) {
						NetRelayConfig relay;
						if (NetRelayConfig::FromJson(body.at("relay").dump(), relay) && relay.Usable(unixSeconds)) {
							m_Relay = std::move(relay);
							if (body.value("relay_current", false)) m_RelayGeneration = m_CheckInGeneration;
						}
					}
					if (body.contains("relay_error")) m_Error = "The directory could not renew the relay. Reconnecting will retry; Cancel returns to Multiplayer.";
					const auto peers = body.find("peers");
					if (peers != body.end() && peers->is_array() && peers->size() <= NetSeatLease::c_MaxStableSeat + 1) {
						std::map<uint16_t, NetConnectionRoute> routes;
						for (const auto& peer: *peers) {
							const auto& route = peer.at("route");
							if (route.empty()) continue;
							const auto seat = peer.at("seat").get<uint64_t>();
							if (seat > NetSeatLease::c_MaxStableSeat) continue;
							NetConnectionRoute item;
							item.iceIdentity = route.at("ice_identity").get<std::string>();
							item.iceVirtualPort = route.at("ice_virtual_port").get<uint16_t>();
							item.listenPort = route.at("listen_port").get<uint16_t>();
							item.listenAddrs = route.at("listen_addrs").get<std::vector<std::string>>();
							item.generation = route.at("generation").get<uint32_t>(); item.state = route.at("state").get<std::string>();
							if (item.iceIdentity.size() <= 128 && item.listenAddrs.size() <= 8) routes.emplace(static_cast<uint16_t>(seat), std::move(item));
						}
						m_PeerRoutes = std::move(routes);
					}
				} catch (const json::exception&) { m_Error = c_Waiting; }
				if (m_CheckedGeneration != m_Route.generation) m_NextCheckIn = 0;
			} else m_Error = ReplyError(response, m_Refused, m_Ended);
		}
		if (m_CheckIn || m_Refused || steadyMs < m_NextCheckIn) return;
		const std::string body = SignedSeatRequest("check-in", unixSeconds);
		if (body.empty()) return;
		m_CheckInGeneration = m_Route.generation;
		m_CheckIn = StartRequest("POST", body);
	}
	NetHostChangeReply NetConnectionAuthority::QueryHostChange(const NetHostChangeRequest& request) {
		std::lock_guard lock(m_Mutex);
		if (m_Suspended || m_SessionId.empty() || m_BaseUrl.empty() || !m_Player || !m_LocalLease || m_LocalLease->directorySessionId != m_SessionId) return {};
		if (!m_HostChangeRequest || *m_HostChangeRequest != request) {
			m_HostChange.reset(); m_HostChangeRequest = request; m_NextHostChange = 0;
			m_HostChangeReply = {}; m_HostChangeReply.state = NetHostChangeReply::State::Waiting;
		}
		return m_HostChangeReply;
	}
	std::string NetConnectionAuthority::HostChangeToken(uint64_t generation) const {
		std::lock_guard lock(m_Mutex);
		return m_HostChangeReply.state == NetHostChangeReply::State::Decided && m_HostChangeReply.generation == generation ? m_HostChangeToken : std::string();
	}
	NetFrameTieReply NetConnectionAuthority::QueryFrameTie(const NetFrameTieRequest& request) {
		std::lock_guard lock(m_Mutex);
		if (m_Suspended || m_SessionId.empty() || m_BaseUrl.empty() || !m_Player || !m_LocalLease || m_LocalLease->directorySessionId != m_SessionId) return {};
		if (!m_FrameTieRequest || *m_FrameTieRequest != request) {
			if (m_RetiredFrameTies.size() >= 16) return {};
			// A query on the sim thread retires an old request without joining its worker.
			if (m_FrameTie) m_RetiredFrameTies.push_back(std::move(m_FrameTie));
			m_FrameTieRequest = request; m_NextFrameTie = 0;
			m_FrameTieReply = {}; m_FrameTieReply.state = NetFrameTieReply::State::Waiting;
		}
		return m_FrameTieReply;
	}

	void NetConnectionAuthority::PollFrameTie(uint64_t steadyMs, uint64_t unixSeconds) {
		std::erase_if(m_RetiredFrameTies, [](const auto& request) { return request->Poll() == NetHttpClient::PollResult::Done; });
		if (!m_FrameTieRequest || !m_LocalLease || !m_Player || m_Instance == NetAuthBytes16{} || m_FrameTieReply.state == NetFrameTieReply::State::Decided) return;
		if (m_FrameTie) {
			if (m_FrameTie->Poll() == NetHttpClient::PollResult::Pending) return;
			const auto response = m_FrameTie->GetResponse(); m_FrameTie.reset(); m_NextFrameTie = steadyMs + 50;
			m_FrameTieReply = {};
			if (response.statusCode == 200) try {
				const auto body = json::parse(response.body);
				std::string error;
				const auto& expected = *m_FrameTieRequest;
				NetHash32 hash{};
				if (!Protocol(body, error) || body.at("round_id") != expected.roundId || body.at("generation") != expected.generation ||
				    body.at("frame") != expected.frame || !Unhex(body.at("config_hash").get<std::string>(), hash) || hash != expected.configHash) return;
				m_FrameTieReply.state = NetFrameTieReply::State::Waiting;
				if (body.at("status") == "decided") {
					auto members = body.at("members").get<std::vector<uint16_t>>();
					if (members.empty() || members.size() * 2 != expected.owners.size() || !std::is_sorted(members.begin(), members.end()) ||
					    std::adjacent_find(members.begin(), members.end()) != members.end() || !std::includes(expected.owners.begin(), expected.owners.end(), members.begin(), members.end())) return;
					m_FrameTieReply.state = NetFrameTieReply::State::Decided; m_FrameTieReply.members = std::move(members);
				}
			} catch (const json::exception&) { m_FrameTieReply = {}; }
		}
		if (m_FrameTie || steadyMs < m_NextFrameTie || m_FrameTieReply.state == NetFrameTieReply::State::Decided) return;
		const std::string body = SignedSeatRequest("frame-tie", unixSeconds, nullptr, &*m_FrameTieRequest);
		if (!body.empty()) m_FrameTie = StartRequest("POST", body, static_cast<int>(c_NetFrameTieDeadlineMs));
	}

	void NetConnectionAuthority::PollHostChange(uint64_t steadyMs, uint64_t unixSeconds) {
		if (!m_HostChangeRequest || !m_LocalLease || !m_Player || m_Instance == NetAuthBytes16{}) return;
		if (m_HostChange) {
			if (m_HostChange->Poll() == NetHttpClient::PollResult::Pending) return;
			const auto response = m_HostChange->GetResponse(); m_HostChange.reset(); m_NextHostChange = steadyMs + 1000;
			m_HostChangeReply.state = NetHostChangeReply::State::Unavailable;
			if (response.statusCode == 200) {
				try {
					const auto body = json::parse(response.body);
					std::string error;
					if (!Protocol(body, error)) return;
					if (body.at("status") == "waiting") { m_HostChangeReply.state = NetHostChangeReply::State::Waiting; return; }
					const auto& decision = body.at("decision");
					const auto& request = *m_HostChangeRequest;
					NetHash32 hash{};
					if (decision.at("session_id") != m_SessionId || decision.at("previous_generation") != request.generation ||
					    decision.at("generation") != request.generation + 1 || decision.at("round_id") != request.roundId ||
					    !Unhex(decision.at("config_hash").get<std::string>(), hash) || hash != request.configHash) return;
					NetHostChangeReply reply;
					reply.generation = decision.at("generation").get<uint64_t>(); reply.boundary = decision.at("boundary").get<uint64_t>();
					const auto host = decision.at("host_seat").get<uint64_t>(), donor = decision.at("donor_seat").get<uint64_t>();
					const auto members = decision.at("members").get<std::vector<uint64_t>>();
					if (host > NetSeatLease::c_MaxStableSeat || donor > NetSeatLease::c_MaxStableSeat || reply.boundary == UINT64_MAX ||
					    members.empty() || members.size() > NetMatchConfigUtil::c_MaxPlayers || !std::is_sorted(members.begin(), members.end()) ||
					    std::adjacent_find(members.begin(), members.end()) != members.end() || members.back() > NetSeatLease::c_MaxStableSeat ||
					    !std::binary_search(members.begin(), members.end(), host) || !std::binary_search(members.begin(), members.end(), donor)) return;
					if (request.agreedHost != UINT16_MAX && (host != request.agreedHost || reply.boundary != request.agreedBoundary ||
					    std::vector<uint64_t>(request.agreedMembers.begin(), request.agreedMembers.end()) != members)) return;
					reply.host = static_cast<uint16_t>(host); reply.donor = static_cast<uint16_t>(donor);
					for (const auto seat: members) reply.members.push_back(static_cast<uint16_t>(seat));
					if (host == m_LocalLease->seat && body.contains("host_token")) {
						const std::string token = body.at("host_token").get<std::string>();
						if (token.empty() || token.size() > NetSeatLease::c_MaxTokenBytes) return;
						m_HostChangeToken = token; NetRelayLogins::Remember(token);
					}
					reply.state = NetHostChangeReply::State::Decided; m_HostChangeReply = std::move(reply);
				} catch (const json::exception&) { /* Invalid replies never end an established match. */ }
			}
		}
		if (!m_HostChange && m_HostChangeReply.state != NetHostChangeReply::State::Decided && steadyMs >= m_NextHostChange) {
			const std::string body = SignedSeatRequest("host-change", unixSeconds, &*m_HostChangeRequest);
			if (!body.empty()) m_HostChange = StartRequest("POST", body);
		}
	}

	void NetConnectionAuthority::Update(uint64_t steadyMs, uint64_t unixSeconds) {
		std::lock_guard lock(m_Mutex);
		if (m_LanBrowser) {
			m_LanBrowser->Tick(steadyMs);
			m_DirectHosts.clear();
			auto hosts = m_LanBrowser->GetHosts(steadyMs);
			std::sort(hosts.begin(), hosts.end(), [](const auto& a, const auto& b) { return a.lastSeenMs > b.lastSeenMs; });
			for (const auto& host: hosts) {
				if (host.matchId == 0 || host.port == 0) continue;
				const auto found = m_DirectHosts.find(host.matchId);
				if (found != m_DirectHosts.end() && found->second.generation >= host.hostGeneration) continue;
				NetConnectionRoute route;
				route.listenPort = host.port; route.listenAddrs = {host.address}; route.generation = host.hostGeneration;
				m_DirectHosts[host.matchId] = std::move(route);
			}
		}
		ObserveNetwork(steadyMs, unixSeconds);
		if (m_Suspended) { PollBootstrap(steadyMs); return; }
		PollOperations(steadyMs); PollBootstrap(steadyMs); PollCheckIn(steadyMs, unixSeconds); PollHostChange(steadyMs, unixSeconds); PollFrameTie(steadyMs, unixSeconds);
	}

	void NetConnectionAuthority::ObserveNetwork(uint64_t steadyMs, uint64_t unixSeconds) {
		const bool resumed = m_LastWallSeconds != 0 && unixSeconds > m_LastWallSeconds + 20;
		m_LastWallSeconds = unixSeconds;
		if (!resumed && steadyMs < m_NextNetworkProbe) return;
		m_NextNetworkProbe = steadyMs + 2000;
		auto addresses = NetLanDiscovery::GetLocalAddresses();
		const uint32_t native = GnsTransport::LocalRouteRevision();
		if (resumed || addresses != m_NetworkAddresses || native != m_NativeRouteRevision) {
			m_NativeRouteRevision = native;
			m_NetworkAddresses = std::move(addresses); ++m_NetworkRevision; ++m_Route.generation;
			m_NextCheckIn = 0;
		}
	}

} // namespace RTE
