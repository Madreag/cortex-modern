#include "NetConnectionAuthority.h"
#include "NetAuthCrypto.h"
#include "NetDirectoryClient.h"
#include "NetDirectoryCodec.h"
#include "NetDirectoryRetry.h"
#include "NetHttpClient.h"
#include "NetMatchService.h"
#include "NetParticipantCrypto.h"
#include "NetProtocol.h"
#include "NetReconnectSession.h"
#include "SettingsMan.h"

#include "Base64/base64.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace RTE {

	struct NetConnectionSelfTestAccess {
		static void ReadRejoinCheckIn(const NetConnectionAuthority& authority, bool allowRelay, bool relayOnly, const std::optional<NetConnectionHost>& host,
		                              uint64_t localUnixSeconds, bool& checkedIn, bool& relayReady) {
			NetMatchService::ReadRejoinCheckIn(authority, allowRelay, relayOnly, host, localUnixSeconds, checkedIn, relayReady);
		}
		static NetMatchService& Live(NetMatchService& service) {
			service.m_State = NetMatchServiceState::Running;
			return service;
		}
		static NetConnectionAuthority& Authority(NetMatchService& service) { return service.m_ConnectionAuthority; }
		static void UpdateConnectionAuthority(NetMatchService& service, uint64_t nowMs) { service.UpdateConnectionAuthority(nowMs); }
		static NetRelayConfig RelaySnapshot(const NetMatchService& service) {
			const auto snapshot = service.m_RelaySnapshot.load();
			return snapshot ? *snapshot : NetRelayConfig{};
		}
		static bool PrepareSeatLease(NetReconnectHost& host, const std::string& name, NetSeatLease& lease, std::string& error) {
			return host.PrepareSeatLease(c_InvalidNetPeerId, name, lease, error);
		}
		static bool ReadSeatLease(NetReconnectClient& client, const std::string& token, const NetAuthBytes32& key, NetH4TicketRecord& record) {
			return client.ReadSeatLease(token, key, record);
		}
	};

	namespace NetConnectionSelfTest {
		namespace {
			using json = nlohmann::json;

			std::string Hex(const uint8_t* bytes, size_t size) {
				constexpr char digits[] = "0123456789abcdef";
				std::string out(size * 2, '0');
				for (size_t i = 0; i < size; ++i) { out[2 * i] = digits[bytes[i] >> 4]; out[2 * i + 1] = digits[bytes[i] & 15]; }
				return out;
			}
			template<size_t N> std::string Hex(const std::array<uint8_t, N>& value) { return Hex(value.data(), value.size()); }
			void Append(std::vector<uint8_t>& out, uint64_t value, size_t bytes) { for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i))); }
			template<size_t N> void Append(std::vector<uint8_t>& out, const std::array<uint8_t, N>& bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

			uint64_t RealUnixSeconds() {
				return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
			}

			/// The directory's side of the connection plane, answered in process: it signs seat tokens as
			/// tools/session_directory/connection_authority.py does and keeps its own clock.
			struct FakeDirectory {
				std::string sessionId = "5d1c0a3e-7777-4888-9999-aaaabbbbcccc";
				std::array<uint8_t, 32> priv{}, pub{};
				int64_t skewSeconds = 0; //!< The directory's clock minus the player's.
				std::atomic<uint64_t> localUnix{0};
				NetSeatLease seat;
				std::mutex mutex;
				std::vector<json> requests; //!< Every request body, with the decoded signed request under "signed".
				std::vector<std::string> paths;
				std::function<NetHttpClient::Response(const json& body, const json& signedRequest)> checkIn;

				bool Init(const NetParticipantId& player, std::string* error) {
					uint8_t secret[32], key[32];
					if (!GetNetParticipantCrypto().IsRealCrypto() || !GetNetParticipantCrypto().GenerateKey(secret, key)) { *error = "the connection rows need the real participant crypto provider"; return false; }
					std::memcpy(priv.data(), secret, 32); std::memcpy(pub.data(), key, 32);
					localUnix = RealUnixSeconds();
					seat.directorySessionId = sessionId;
					seat.epoch.fill(0x10); seat.hostSessionId = 42; seat.seat = 1; seat.generation = 1;
					seat.participant = player; seat.credential.fill(0x20);
					std::copy(pub.begin(), pub.end(), seat.authority.begin());
					return true;
				}
				uint64_t Now() const { return static_cast<uint64_t>(static_cast<int64_t>(localUnix.load()) + skewSeconds); }
				std::string Token(uint64_t issued) const {
					std::vector<uint8_t> fields;
					Append(fields, NetSeatLease::c_Version, 2); Append(fields, NetProtocol::c_Version, 2);
					std::string compact;
					for (char c: sessionId) if (c != '-') compact += c;
					for (size_t i = 0; i < 16; ++i) fields.push_back(static_cast<uint8_t>(std::stoi(compact.substr(2 * i, 2), nullptr, 16)));
					Append(fields, seat.epoch); Append(fields, seat.hostSessionId, 8); Append(fields, seat.seat, 2); Append(fields, seat.generation, 4);
					Append(fields, seat.participant); Append(fields, issued, 8); Append(fields, issued + NetSeatLease::c_LifetimeSeconds, 8); Append(fields, seat.credential);
					static constexpr char domain[] = "CortexSeatToken1";
					std::vector<uint8_t> message(std::begin(domain), std::end(domain));
					message.insert(message.end(), fields.begin(), fields.end());
					uint8_t secret[32], signature[64];
					std::memcpy(secret, priv.data(), 32);
					(void)GetNetParticipantCrypto().Sign(secret, message.data(), message.size(), signature);
					fields.insert(fields.end(), std::begin(signature), std::end(signature));
					return base64_encode(fields.data(), fields.size());
				}
				NetSeatLease Lease() const {
					NetSeatLease lease = seat;
					lease.issuedAt = Now(); lease.expiresAt = lease.issuedAt + NetSeatLease::c_LifetimeSeconds; lease.token = Token(lease.issuedAt);
					return lease;
				}
				json LeaseReply() const {
					return json{{"connection_protocol", NetSeatLease::c_Version}, {"authority_key", Hex(pub)}, {"seat_token", Token(Now())}, {"seat", seat.seat},
						{"generation", seat.generation}, {"expires_at", Now() + NetSeatLease::c_LifetimeSeconds}, {"check_in_s", 5}, {"route_revision", 1},
						{"peers", json::array()}, {"directory_time", Now()}};
				}
				json Relay(const std::string& login, uint64_t lifetime = 600) const {
					return json{{"match_id", sessionId}, {"expires_at", Now() + lifetime},
						{"iceServers", json::array({json{{"urls", json::array({"turn:relay.test:3478?transport=udp"})}, {"username", login}, {"credential", login + "-password"}}})}};
				}
				json Bootstrap() const {
					NetDirectorySessionRow row;
					row.name = "Host"; row.activity = "P4 Alpha Duel"; row.scene = "Grasslands"; row.mode = "pvp-skirmish";
					row.peerCount = 2; row.seatsFree = 0; row.gameVersion = "7.0.0"; row.buildId = "connection-selftest";
					row.networkProtocolVersion = NetProtocol::c_Version; row.lockstepCodecVersion = 20; row.controllerFrameVersion = 6;
					row.matchConfigHash = std::string(64, 'a'); row.sessionIdentityHash = std::string(64, 'b'); row.moduleManifestHash = std::string(64, 'c');
					row.listenPort = 41010; row.listenAddrs = {"192.0.2.1"}; row.joinMode = "ip";
					row.sessionId = sessionId; row.ageS = 0; row.observedIp = "192.0.2.1"; row.state = "running";
					return json{{"connection_protocol", NetSeatLease::c_Version}, {"session", json::parse(NetDirectoryCodec::EncodeSessionRow(row))},
						{"authority_key", Hex(pub)}, {"host_generation", 0}, {"relay_enabled", true}};
				}
				static NetHttpClient::Response Reply(long status, const json& body) { return {status, body.dump(), "", ""}; }
				NetHttpClient::Response operator()(const std::string& method, const std::string& url, const std::string& text) {
					json body = json::parse(text, nullptr, false);
					json signedRequest;
					if (body.is_object() && body.contains("signed_request") && body["signed_request"].is_string())
						signedRequest = json::parse(base64_decode(body["signed_request"].get<std::string>()), nullptr, false);
					{
						std::lock_guard lock(mutex);
						paths.push_back(method + " " + url.substr(url.find("/v1/")));
						json logged = body.is_object() ? body : json::object();
						if (!signedRequest.is_null()) logged["signed"] = signedRequest;
						requests.push_back(std::move(logged));
					}
					const std::string operation = body.is_object() ? body.value("operation", std::string()) : std::string();
					if (method == "GET" && url.ends_with("/connections")) return Reply(200, Bootstrap());
					if (method == "GET" && url.ends_with("/ice-servers")) return Reply(200, Relay("lent-login"));
					if (operation == "remove") return Reply(200, json{{"ok", true}, {"connection_protocol", NetSeatLease::c_Version}});
					if (operation == "issue") return Reply(200, LeaseReply());
					if (operation == "check-in" || operation == "frame-tie") {
						if (checkIn) return checkIn(body, signedRequest);
						return Reply(200, LeaseReply());
					}
					return Reply(404, json{{"error", "not_found"}});
				}
				size_t Count(const std::string& operation) {
					std::lock_guard lock(mutex);
					return static_cast<size_t>(std::count_if(requests.begin(), requests.end(), [&](const json& request) { return request.value("operation", std::string()) == operation; }));
				}
				json Last(const std::string& operation) {
					std::lock_guard lock(mutex);
					for (auto it = requests.rbegin(); it != requests.rend(); ++it) if (it->value("operation", std::string()) == operation) return *it;
					return {};
				}
			};

			/// One player key on disk for the rows, removed afterwards.
			struct PlayerKey {
				std::filesystem::path folder;
				NetParticipantIdentityStore store;
				bool Init(const std::string& name, std::string* error) {
					std::array<uint8_t, 8> nonce{};
					(void)GetNetAuthCrypto().RandomBytes(nonce.data(), nonce.size());
					folder = std::filesystem::temp_directory_path() / ("cccp-connection-selftest-" + Hex(nonce));
					std::error_code ignored;
					std::filesystem::create_directories(folder, ignored);
					store.SetPath((folder / (name + ".key")).string());
					return store.LoadOrCreate(error);
				}
				~PlayerKey() { std::error_code ignored; if (!folder.empty()) std::filesystem::remove_all(folder, ignored); }
			};

			struct Responder {
				explicit Responder(FakeDirectory& directory) { NetHttpClient::SetResponderForTest([&directory](const std::string& method, const std::string& url, const std::string& body) { return directory(method, url, body); }); }
				~Responder() { NetHttpClient::SetResponderForTest(nullptr); }
			};

			/// Drives one authority on the player's clock until the check returns true or the steps run out.
			bool Drive(NetConnectionAuthority& authority, FakeDirectory& directory, uint64_t& steady, int steps, const std::function<bool()>& done) {
				for (int step = 0; step < steps; ++step) {
					authority.Update(steady, directory.localUnix.load());
					if (done()) return true;
					steady += 1000; directory.localUnix += 1;
				}
				return done();
			}

			struct Seated {
				PlayerKey key;
				FakeDirectory directory;
				NetConnectionAuthority authority;
				uint64_t steady = NetDirectoryRetry::NowMs();
				bool Init(const std::string& name, int64_t skew, std::string* error) {
					if (!key.Init(name, error) || !directory.Init(key.store.PublicId(), error)) return false;
					directory.skewSeconds = skew;
					authority.Configure(&key.store, "https://" + name + ".directory.test", "connection" + name, "");
					authority.SetDirectory(directory.sessionId);
					if (!authority.RestoreLease(directory.Lease())) { *error = name + ": the fixture's directory seat would not restore"; return false; }
					authority.RefreshRoute();
					return true;
				}
			};

			bool TestClockRefusalRetriesOnTheDirectoryClock(std::string* error) {
				Seated player;
				if (!player.Init("clock", 120, error)) return false;
				Responder responder(player.directory);
				uint64_t refusedAt = 0;
				player.directory.checkIn = [&](const json&, const json& signedRequest) {
					const int64_t sent = signedRequest.value("sent_at", int64_t{0});
					const int64_t now = static_cast<int64_t>(player.directory.Now());
					if (sent < now - 60 || sent > now + 60) {
						++refusedAt;
						return FakeDirectory::Reply(409, json{{"error", "connection_clock"}, {"message", "This device's clock differs from the directory. Correct its clock and rejoin."},
							{"connection_protocol", NetSeatLease::c_Version}, {"directory_time", now}});
					}
					return FakeDirectory::Reply(200, player.directory.LeaseReply());
				};
				const bool checkedIn = Drive(player.authority, player.directory, player.steady, 40, [&] { return player.authority.CheckIns() != 0 || player.authority.Refused(); });
				if (!checkedIn || player.authority.Refused() || player.authority.CheckIns() == 0) {
					*error = "a check-in two minutes off the directory's clock was refused for the rest of the match: " + player.authority.Error(); return false;
				}
				bool seated = false, relayReady = false;
				NetConnectionSelfTestAccess::ReadRejoinCheckIn(player.authority, false, false, std::nullopt, player.directory.localUnix.load(), seated, relayReady);
				if (!seated) { *error = "a seat the directory renewed on its own clock read as unusable on the player's"; return false; }
				{
					// A player 15 minutes ahead of the directory keeps its ten-minute logins alive on its own clock.
					Seated ahead;
					if (!ahead.Init("clockahead", -900, error)) return false;
					Responder aheadResponder(ahead.directory);
					ahead.directory.checkIn = [&](const json&, const json& signedRequest) {
						const int64_t sent = signedRequest.value("sent_at", int64_t{0});
						const int64_t now = static_cast<int64_t>(ahead.directory.Now());
						if (sent < now - 60 || sent > now + 60)
							return FakeDirectory::Reply(409, json{{"error", "connection_clock"}, {"message", "This device's clock differs from the directory. Correct its clock and rejoin."},
								{"connection_protocol", NetSeatLease::c_Version}, {"directory_time", now}});
						json reply = ahead.directory.LeaseReply();
						reply["relay"] = ahead.directory.Relay("seat-login"); reply["relay_current"] = true;
						return FakeDirectory::Reply(200, reply);
					};
					(void)Drive(ahead.authority, ahead.directory, ahead.steady, 40, [&] { return ahead.authority.RelayRefreshed(); });
					const uint64_t local = ahead.directory.localUnix.load();
					if (!ahead.authority.Relay().Usable(local) || ahead.authority.Relay().expiresAt <= local + 500) {
						*error = "a seat relay login with ten minutes left on the directory's clock read as expired on a player 15 minutes ahead"; return false;
					}
					NetDirectoryClient browse;
					browse.Configure("https://clockahead-fetch.directory.test", "connectionclockahead", "");
					browse.SetDirectoryClockOffset(static_cast<int64_t>(ahead.authority.DirectoryNow(local)) - static_cast<int64_t>(local));
					if (!browse.FetchIceServers(ahead.directory.sessionId)) { *error = "the far-clock joiner's fetch did not start"; return false; }
					for (int i = 0; i < 4 && browse.IceRequestPending(); ++i) browse.Update(NetDirectoryRetry::NowMs());
					if (!browse.IceServers().Usable(local)) { *error = "a joiner 15 minutes ahead of the directory dropped the relay login it was lent"; return false; }
				}
				std::cout << "[net-directory-selftest] PASS connection_clock_retries_on_the_directory_clock refusals=" << refusedAt << " skew_s=120,-900" << std::endl;
				return true;
			}

			bool TestAutomaticRejoinDialsThroughAMintFailure(std::string* error) {
				Seated player;
				if (!player.Init("mint", 0, error)) return false;
				Responder responder(player.directory);
				std::optional<json> relay;
				bool mintFails = true;
				player.directory.checkIn = [&](const json&, const json&) {
					json reply = player.directory.LeaseReply();
					if (relay) reply["relay"] = *relay;
					reply["relay_current"] = !mintFails;
					if (mintFails) { reply["relay_error"] = "relay_provider_unavailable"; reply["relay_retry_after_s"] = 5; }
					return FakeDirectory::Reply(200, reply);
				};
				NetConnectionHost host;
				host.relayEnabled = true;
				const auto read = [&](bool relayOnly, bool& checkedIn, bool& ready) {
					NetConnectionSelfTestAccess::ReadRejoinCheckIn(player.authority, true, relayOnly, host, player.directory.localUnix.load(), checkedIn, ready);
				};
				if (!Drive(player.authority, player.directory, player.steady, 20, [&] { return player.authority.CheckIns() != 0; })) {
					*error = "the rejoin's check-in never landed: " + player.authority.Error(); return false;
				}
				bool checkedIn = false, ready = false;
				read(false, checkedIn, ready);
				if (!checkedIn || !ready) { *error = "an Automatic rejoin waited on a relay mint the directory could not make, though direct would connect"; return false; }
				read(true, checkedIn, ready);
				if (ready) { *error = "a Relay only rejoin dialled with no relay login"; return false; }
				std::cout << "[net-directory-selftest] PASS automatic_rejoin_dials_through_a_relay_mint_failure" << std::endl;
				// A Relay only rejoin takes the seat's unexpired login while the renewal fails, and a fresh one once it lands.
				relay = player.directory.Relay("held-login");
				const uint64_t before = player.authority.CheckIns();
				if (!Drive(player.authority, player.directory, player.steady, 20, [&] { return player.authority.CheckIns() > before; })) { *error = "the second check-in never landed"; return false; }
				read(true, checkedIn, ready);
				if (!ready || player.authority.RelayRefreshed()) { *error = "a Relay only rejoin did not dial on the seat's unexpired login while the renewal failed"; return false; }
				mintFails = false; relay = player.directory.Relay("renewed-login");
				player.authority.RefreshRoute();
				if (!Drive(player.authority, player.directory, player.steady, 20, [&] { return player.authority.RelayRefreshed(); }) || player.authority.Relay().iceServers.front().username != "renewed-login") {
					*error = "a renewed relay login never replaced the held one"; return false;
				}
				std::cout << "[net-directory-selftest] PASS rejoin_relay_gate_waits_only_in_relay_only" << std::endl;
				return true;
			}

			bool TestARotatedRelayLoginReachesTheLiveTransport(std::string* error) {
				PlayerKey key;
				FakeDirectory directory;
				if (!key.Init("rotation", error) || !directory.Init(key.store.PublicId(), error)) return false;
				Responder responder(directory);
				std::string login = "first-login";
				directory.checkIn = [&](const json&, const json&) {
					json reply = directory.LeaseReply();
					reply["relay"] = directory.Relay(login); reply["relay_current"] = true;
					return FakeDirectory::Reply(200, reply);
				};
				struct RestoreAdmission {
					bool enabled = NetMatchService::IsAdmissionEnabled();
					~RestoreAdmission() { NetMatchService::SetAdmissionEnabled(enabled); }
				} restore;
				NetMatchService::SetAdmissionEnabled(true);
				if (!SettingsMan::IsConstructed()) SettingsMan::Construct();
				NetMatchService service;
				NetConnectionSelfTestAccess::Live(service);
				NetConnectionAuthority& authority = NetConnectionSelfTestAccess::Authority(service);
				authority.Configure(&key.store, "https://rotation.directory.test", "connectionrotation", "");
				authority.SetDirectory(directory.sessionId);
				if (!authority.RestoreLease(directory.Lease())) { *error = "rotation: the fixture's seat would not restore"; return false; }
				uint64_t steady = NetDirectoryRetry::NowMs();
				const auto pumpUntil = [&](const std::string& wanted) {
					for (int step = 0; step < 30; ++step) {
						NetConnectionSelfTestAccess::UpdateConnectionAuthority(service, steady);
						const NetRelayConfig live = NetConnectionSelfTestAccess::RelaySnapshot(service);
						if (!live.Empty() && live.iceServers.front().username == wanted) return true;
						steady += 1000; directory.localUnix += 1;
					}
					return false;
				};
				if (!pumpUntil("first-login")) { *error = "the seat's relay login never reached the match's relay offer"; return false; }
				login = "rotated-login";
				authority.RefreshRoute();
				if (!pumpUntil("rotated-login")) { *error = "a rotated relay login on a live seat never replaced the transport's offer"; return false; }
				std::cout << "[net-directory-selftest] PASS relay_login_rotation_reaches_the_live_transport" << std::endl;
				return true;
			}

			bool TestARouteGenerationReachesTheCheckIn(std::string* error) {
				Seated player;
				if (!player.Init("route", 0, error)) return false;
				Responder responder(player.directory);
				std::vector<std::string> addresses = {"192.0.2.10"};
				std::mutex addressMutex;
				player.authority.SetAddressSourceForTest([&] { std::lock_guard lock(addressMutex); return addresses; });
				if (!Drive(player.authority, player.directory, player.steady, 20, [&] { return player.authority.RouteCheckedIn(); })) { *error = "the seat's first route never checked in"; return false; }
				NetConnectionRoute route = player.authority.Route();
				route.listenAddrs = {"198.51.100.7"}; route.listenPort = 41012;
				player.authority.SetRoute(route);
				const uint32_t moved = player.authority.Route().generation;
				if (player.authority.RouteCheckedIn()) { *error = "a new route read as checked in before the directory heard it"; return false; }
				if (!Drive(player.authority, player.directory, player.steady, 20, [&] { return player.authority.RouteCheckedIn(); })) { *error = "a new route never checked in"; return false; }
				const json sent = player.directory.Last("check-in")["signed"]["route"];
				if (sent.value("generation", 0u) != moved || sent.value("listen_addrs", std::vector<std::string>{}) != std::vector<std::string>{"198.51.100.7"}) {
					*error = "the check-in carried route generation " + std::to_string(sent.value("generation", 0u)) + " for the seat's new route " + std::to_string(moved); return false;
				}
				// A changed adapter set is a new route generation the next check-in carries.
				const uint32_t revision = player.authority.NetworkRevision();
				{ std::lock_guard lock(addressMutex); addresses = {"192.0.2.10", "2001:db8::77"}; }
				if (!Drive(player.authority, player.directory, player.steady, 20, [&] { return player.authority.NetworkRevision() != revision && player.authority.RouteCheckedIn(); })) {
					*error = "a changed adapter set never reached the directory as a new route"; return false;
				}
				if (player.directory.Last("check-in")["signed"]["route"].value("generation", 0u) != player.authority.Route().generation) { *error = "an adapter change checked in under an old generation"; return false; }
				std::cout << "[net-directory-selftest] PASS route_generation_change_reaches_the_check_in" << std::endl;
				return true;
			}

			bool TestDirectoryRefusalsReadAsTheirWords(std::string* error) {
				{
					Seated player;
					if (!player.Init("removed", 0, error)) return false;
					Responder responder(player.directory);
					const std::string words = "Original player was banned by the host. Choose another match.";
					player.directory.checkIn = [&](const json&, const json&) {
						return FakeDirectory::Reply(403, json{{"error", "player_removed"}, {"message", words}, {"connection_protocol", NetSeatLease::c_Version}});
					};
					(void)Drive(player.authority, player.directory, player.steady, 10, [&] { return player.authority.Refused(); });
					if (!player.authority.Refused() || player.authority.Error() != words || player.directory.Count("check-in") != 1) {
						*error = "a removed player's check-in read as '" + player.authority.Error() + "' after " + std::to_string(player.directory.Count("check-in")) + " requests"; return false;
					}
				}
				{
					Seated player;
					if (!player.Init("inuse", 0, error)) return false;
					Responder responder(player.directory);
					const std::string words = "Original player is still connected. Waiting for that connection to close; Cancel stops rejoining.";
					int refusals = 2;
					player.directory.checkIn = [&](const json&, const json&) {
						if (refusals-- > 0) return FakeDirectory::Reply(409, json{{"error", "seat_in_use"}, {"message", words}, {"connection_protocol", NetSeatLease::c_Version}});
						return FakeDirectory::Reply(200, player.directory.LeaseReply());
					};
					bool sawWords = false;
					(void)Drive(player.authority, player.directory, player.steady, 40, [&] { sawWords = sawWords || player.authority.Error() == words; return player.authority.CheckIns() != 0 || player.authority.Refused(); });
					if (player.authority.Refused() || player.authority.CheckIns() == 0 || !sawWords || !player.authority.Error().empty()) {
						*error = "a seat still held by its old connection did not wait it out with the directory's words: " + player.authority.Error(); return false;
					}
				}
				{
					Seated player;
					if (!player.Init("ended", 0, error)) return false;
					Responder responder(player.directory);
					player.directory.checkIn = [&](const json&, const json&) { return FakeDirectory::Reply(404, json{{"error", "not_found"}}); };
					(void)Drive(player.authority, player.directory, player.steady, 10, [&] { return player.authority.Ended(); });
					const uint64_t now = NetDirectoryRetry::NowMs();
					const bool notice = NetDirectoryBudget::TakeEndedNotice("https://ended.directory.test", "connectionended", player.directory.sessionId, now);
					const bool again = NetDirectoryBudget::TakeEndedNotice("https://ended.directory.test", "connectionended", player.directory.sessionId, now);
					if (!player.authority.Ended() || !player.authority.Refused() || !notice || again || player.authority.Error().find("no longer in the directory") == std::string::npos) {
						*error = "a match gone from the directory mid-match did not end the check-ins with one notice: " + player.authority.Error(); return false;
					}
				}
				std::cout << "[net-directory-selftest] PASS player_removed_seat_in_use_and_ended_replies_read_as_their_words" << std::endl;
				return true;
			}

			bool TestARemovalWaitsForTheHostToken(std::string* error) {
				PlayerKey key;
				FakeDirectory directory;
				if (!key.Init("removal", error) || !directory.Init(key.store.PublicId(), error)) return false;
				Responder responder(directory);
				NetConnectionAuthority host;
				host.Configure(&key.store, "https://removal.directory.test", "connectionremoval", "");
				host.SetDirectory(directory.sessionId);
				NetParticipantId kicked{}; kicked.fill(0x5a);
				uint64_t steady = NetDirectoryRetry::NowMs();
				const auto pump = [&](int steps) { for (int step = 0; step < steps; ++step) { host.Update(steady, directory.localUnix.load()); steady += 1000; directory.localUnix += 1; } };
				// A kick made before the host's directory token arrives.
				host.Remove(kicked, "Kicked player", false);
				pump(3);
				host.SetDirectory(directory.sessionId, "first-host-token", 0);
				pump(5);
				json sent = directory.Last("remove");
				if (directory.Count("remove") != 1 || sent.value("token", std::string()) != "first-host-token" || sent.value("participant", std::string()) != Hex(kicked) || sent.value("action", std::string()) != "removed") {
					*error = "a kick made before the host held its directory token reached the directory " + std::to_string(directory.Count("remove")) + " times"; return false;
				}
				// A ban of the same player after the kick is a new answer for the directory.
				host.Remove(kicked, "Kicked player", true);
				pump(5);
				if (directory.Count("remove") != 2 || directory.Last("remove").value("action", std::string()) != "banned") {
					*error = "a ban after a kick of the same player never reached the directory"; return false;
				}
				// A ban queued while the host has no token survives its leave and goes with a successor's token.
				NetParticipantId banned{}; banned.fill(0x6b);
				host.SetDirectory(directory.sessionId);
				host.Remove(banned, "Banned player", true);
				host.Suspend();
				pump(3);
				NetSeatLease lease = directory.Lease();
				if (!host.RestoreLease(lease)) { *error = "removal: the fixture's seat would not restore"; return false; }
				host.SetDirectory(directory.sessionId, "successor-token", 1);
				pump(5);
				sent = directory.Last("remove");
				if (directory.Count("remove") != 3 || sent.value("token", std::string()) != "successor-token" || sent.value("host_generation", 0) != 1 || sent.value("participant", std::string()) != Hex(banned)) {
					*error = "a ban queued across the host's leave never reached the directory under its successor"; return false;
				}
				std::cout << "[net-directory-selftest] PASS removal_with_no_host_token_waits_for_one" << std::endl;
				return true;
			}

			bool TestATieQueryNeverWaitsForTheAdapterScan(std::string* error) {
				Seated player;
				if (!player.Init("scan", 0, error)) return false;
				Responder responder(player.directory);
				std::mutex gate;
				std::condition_variable changed;
				bool entered = false, release = false;
				player.authority.SetAddressSourceForTest([&] {
					std::unique_lock lock(gate);
					entered = true; changed.notify_all();
					changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
					return std::vector<std::string>{"192.0.2.10"};
				});
				std::thread pump([&] { player.authority.Update(player.steady, player.directory.localUnix.load()); });
				{
					std::unique_lock lock(gate);
					changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
				}
				NetFrameTieRequest tie;
				tie.generation = 1; tie.roundId = 9; tie.frame = 100; tie.host = 0; tie.owners = {0, 1}; tie.members = {1};
				auto query = std::async(std::launch::async, [&] { return player.authority.QueryFrameTie(tie); });
				const bool answered = query.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
				{
					std::lock_guard lock(gate);
					release = true; changed.notify_all();
				}
				pump.join();
				(void)query.get();
				if (!entered || !answered) { *error = "a sim tick's tie query waited for the adapter enumeration"; return false; }
				std::cout << "[net-directory-selftest] PASS tie_query_never_waits_for_the_adapter_scan" << std::endl;
				return true;
			}

			bool TestATypedAddressJoinerNeedsNoDirectory(std::string* error) {
				PlayerKey hostKey, joinerKey;
				FakeDirectory directory;
				if (!hostKey.Init("listed-host", error) || !joinerKey.Init("typed-joiner", error) || !directory.Init(joinerKey.store.PublicId(), error)) return false;
				Responder responder(directory);
				NetConnectionAuthority host, joiner;
				host.Configure(&hostKey.store, "https://listed.directory.test", "connectionlisted", "");
				host.SetDirectory(directory.sessionId, "listed-host-token", 0);
				NetAuthBytes32 signing{};
				(void)GetNetAuthCrypto().RandomBytes(signing.data(), signing.size());
				host.SetHostSigningKey(signing);
				joiner.Configure(&joinerKey.store, "https://listed.directory.test", "connectiontyped", "");
				NetSeatLease wanted = directory.seat;
				wanted.directorySessionId.clear(); wanted.authority = {};
				wanted.issuedAt = directory.Now(); wanted.expiresAt = wanted.issuedAt + NetSeatLease::c_LifetimeSeconds;
				NetSeatLease issued;
				std::string issueError;
				const auto result = host.Issue(wanted, "Typed joiner", issued, issueError);
				uint64_t steady = NetDirectoryRetry::NowMs();
				for (int step = 0; step < 5; ++step) { host.Update(steady, directory.localUnix.load()); joiner.Update(steady, directory.localUnix.load()); steady += 1000; directory.localUnix += 1; }
				const bool adopted = joiner.AdoptLocalLease(issued);
				if (result != NetConnectionAuthority::Result::Ready || !issued.directorySessionId.empty() || !adopted || directory.Count("issue") != 0 || directory.Count("check-in") != 0) {
					*error = "a typed-address joiner of a listed host needed the directory (" + std::to_string(directory.paths.size()) + " requests): " + issueError; return false;
				}
				std::cout << "[net-directory-selftest] PASS listed_host_seats_a_typed_address_joiner_without_the_directory" << std::endl;
				return true;
			}

			uint64_t ClockMs(void* context) { return static_cast<std::atomic<uint64_t>*>(context)->load() * 1000; }

			NetH4TicketRecord RecordOf(const NetSeatLease& lease) {
				NetH4TicketRecord record;
				record.epoch = lease.epoch; record.stableSeat = lease.seat; record.holderGeneration = lease.generation;
				record.hostSessionId = lease.hostSessionId; record.credential = lease.credential;
				return record;
			}

			bool TestSeatLeasesAreJudgedOnTheirIssuersClock(std::string* error) {
				{
					// A host 200 s ahead of the directory keeps renewing the directory's seats.
					PlayerKey hostKey, joinerKey;
					FakeDirectory directory;
					if (!hostKey.Init("ahead-host", error) || !joinerKey.Init("ahead-joiner", error) || !directory.Init(joinerKey.store.PublicId(), error)) return false;
					directory.skewSeconds = -200;
					Responder responder(directory);
					NetConnectionAuthority authority;
					authority.Configure(&hostKey.store, "https://ahead.directory.test", "connectionahead", "");
					authority.SetDirectory(directory.sessionId, "ahead-host-token", 0);
					NetReconnectHost host;
					host.SetConnectionAuthority(&authority);
					host.SetUnixClock(&ClockMs, &directory.localUnix);
					NetSeatLease lease = directory.seat;
					lease.authority = {};
					std::string leaseError;
					bool renewed = false;
					uint64_t steady = NetDirectoryRetry::NowMs();
					for (int step = 0; step < 20 && !renewed; ++step) {
						renewed = NetConnectionSelfTestAccess::PrepareSeatLease(host, "Joiner", lease, leaseError);
						authority.Update(steady, directory.localUnix.load());
						steady += 1000; directory.localUnix += 1;
					}
					if (!renewed || lease.token.empty()) { *error = "a host 200 s ahead of the directory never held its joiner's seat lease after " + std::to_string(directory.Count("issue")) + " issues"; return false; }
				}
				{
					// A joiner two minutes behind the directory adopts the seat the directory renewed.
					Seated joiner;
					if (!joiner.Init("behind-joiner", 120, error)) return false;
					Responder responder(joiner.directory);
					(void)Drive(joiner.authority, joiner.directory, joiner.steady, 30, [&] { return joiner.authority.CheckIns() != 0 || joiner.authority.Refused(); });
					NetReconnectClient client;
					client.SetConnectionAuthority(&joiner.authority);
					client.SetUnixClock(&ClockMs, &joiner.directory.localUnix);
					client.SetDirectorySessionId(joiner.directory.sessionId);
					NetH4TicketRecord record = RecordOf(joiner.directory.seat);
					NetAuthBytes32 key{};
					std::copy(joiner.directory.pub.begin(), joiner.directory.pub.end(), key.begin());
					if (!NetConnectionSelfTestAccess::ReadSeatLease(client, joiner.directory.Token(joiner.directory.Now()), key, record)) {
						*error = "a joiner two minutes behind the directory refused the seat the directory had just renewed"; return false;
					}
				}
				{
					// A typed-address joiner two minutes behind its host adopts the host's own seat.
					PlayerKey hostKey, joinerKey;
					FakeDirectory clock;
					if (!hostKey.Init("typed-host", error) || !joinerKey.Init("typed-behind", error) || !clock.Init(joinerKey.store.PublicId(), error)) return false;
					NetConnectionAuthority hostAuthority, joinerAuthority;
					hostAuthority.Configure(&hostKey.store, "", "", "");
					joinerAuthority.Configure(&joinerKey.store, "", "", "");
					NetAuthBytes32 signing{};
					(void)GetNetAuthCrypto().RandomBytes(signing.data(), signing.size());
					hostAuthority.SetHostSigningKey(signing);
					NetSeatLease wanted = clock.seat;
					wanted.directorySessionId.clear(); wanted.authority = {};
					wanted.issuedAt = clock.localUnix.load(); wanted.expiresAt = wanted.issuedAt + NetSeatLease::c_LifetimeSeconds;
					NetSeatLease issued;
					std::string issueError;
					if (hostAuthority.Issue(wanted, "Typed joiner", issued, issueError) != NetConnectionAuthority::Result::Ready) { *error = "typed: the host did not sign its seat: " + issueError; return false; }
					std::atomic<uint64_t> joinerUnix{clock.localUnix.load() - 120};
					NetReconnectClient client;
					client.SetConnectionAuthority(&joinerAuthority);
					client.SetUnixClock(&ClockMs, &joinerUnix);
					NetH4TicketRecord record = RecordOf(issued);
					if (!NetConnectionSelfTestAccess::ReadSeatLease(client, issued.token, issued.authority, record)) {
						*error = "a typed-address joiner two minutes behind its host refused the host's seat lease"; return false;
					}
				}
				std::cout << "[net-directory-selftest] PASS seat_leases_are_judged_on_their_issuers_clock" << std::endl;
				return true;
			}

			bool TestAJoinerFetchWaitsForTheRelayAllowance(std::string* error) {
				FakeDirectory directory;
				NetParticipantId nobody{}; nobody.fill(1);
				if (!directory.Init(nobody, error)) return false;
				const std::string url = "https://fetch.directory.test", install = "connectionfetch";
				std::function<NetHttpClient::Response()> answer = [&] { return FakeDirectory::Reply(200, directory.Relay("lent-login")); };
				size_t fetches = 0;
				NetHttpClient::SetResponderForTest([&](const std::string& method, const std::string&, const std::string&) {
					if (method == "GET") ++fetches;
					return answer();
				});
				struct Restore { ~Restore() { NetHttpClient::SetResponderForTest(nullptr); } } restore;
				NetDirectoryClient browse;
				browse.Configure(url, install, "");
				const auto settle = [&] { for (int i = 0; i < 4 && browse.IceRequestPending(); ++i) browse.Update(NetDirectoryRetry::NowMs()); };
				if (!browse.FetchIceServers(directory.sessionId)) { *error = "a joiner's first relay fetch did not start"; return false; }
				settle();
				if (browse.IceServers().Empty() || browse.IceServers().iceServers.front().username != "lent-login") { *error = "a joiner's lent relay login was not kept"; return false; }
				// The joiner's mint shares the install's relay allowance; its refusal leaves every other request alone.
				const uint64_t now = NetDirectoryRetry::NowMs();
				answer = [&] { return FakeDirectory::Reply(429, json{{"error", "rate_limited_key"}, {"retry_after_s", 30}, {"budget", "relay"}}); };
				NetDirectoryBudget::Refused(url, install, "relay", now, now + 30000, "");
				const size_t before = fetches;
				const bool started = browse.FetchIceServers(directory.sessionId);
				settle();
				if (started || fetches != before || !NetDirectoryBudget::Ready(url, install, "general", NetDirectoryRetry::NowMs())) {
					*error = "a joiner's relay fetch went out while the install's relay allowance was spent"; return false;
				}
				// A directory with no relay of its own has none to lend; asking again cannot change that.
				FakeDirectory other;
				if (!other.Init(nobody, error)) return false;
				answer = [&] { return FakeDirectory::Reply(503, json{{"error", "relay_not_configured"}}); };
				NetDirectoryClient bare;
				bare.Configure("https://bare.directory.test", install, "");
				if (!bare.FetchIceServers(other.sessionId)) { *error = "a fetch from a directory with no relay did not start"; return false; }
				for (int i = 0; i < 4 && bare.IceRequestPending(); ++i) bare.Update(NetDirectoryRetry::NowMs());
				if (bare.IceRequestRetryable() || bare.IceError().find("no relay login to lend") == std::string::npos) {
					*error = "a directory with no relay to lend read as a login still coming: " + bare.IceError(); return false;
				}
				std::cout << "[net-directory-selftest] PASS joiner_fetch_spends_the_relay_allowance" << std::endl;
				return true;
			}
		}

		bool Run(std::string* error) {
			const char* selected = std::getenv("CCCP_TEST_CONNECTION_CASE");
			std::vector<std::string> failed;
			// Every row runs, so one launch names each red one.
			for (const auto& test: std::vector<std::pair<const char*, bool (*)(std::string*)>>{
			         {"clock", TestClockRefusalRetriesOnTheDirectoryClock}, {"mint", TestAutomaticRejoinDialsThroughAMintFailure},
			         {"rotation", TestARotatedRelayLoginReachesTheLiveTransport}, {"route", TestARouteGenerationReachesTheCheckIn},
			         {"refusals", TestDirectoryRefusalsReadAsTheirWords}, {"removal", TestARemovalWaitsForTheHostToken},
			         {"scan", TestATieQueryNeverWaitsForTheAdapterScan}, {"typed", TestATypedAddressJoinerNeedsNoDirectory},
			         {"leases", TestSeatLeasesAreJudgedOnTheirIssuersClock}, {"fetch", TestAJoinerFetchWaitsForTheRelayAllowance}}) {
				if (selected && std::string(selected) != test.first) continue;
				std::string why;
				if (test.second(&why)) continue;
				std::cerr << "[net-directory-selftest] FAIL: " << test.first << ": " << why << std::endl;
				failed.push_back(std::string(test.first) + ": " + why);
			}
			if (failed.empty()) return true;
			*error = std::to_string(failed.size()) + " connection rows failed, first " + failed.front();
			return false;
		}
	}
}
