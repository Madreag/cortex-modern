#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RTE {

	// Field limits mirrored from tools/session_directory/session_directory.py (the API of record).
	namespace NetDirectoryLimits {
		inline constexpr size_t c_MaxStringChars = 64;        // MAX_STR
		inline constexpr size_t c_MaxPeerChars = 64;          // peer string cap (not 7+MAX_STR)
		inline constexpr size_t c_MaxListRows = 4096;         // MAX_ROWS
		inline constexpr size_t c_MaxSignalRows = 256;        // MAX_QUEUE
		inline constexpr size_t c_MaxListenAddrs = 8;         // MAX_ARR
		inline constexpr size_t c_MaxBodyBytes = 128 * 1024;  // MAX_BODY
		inline constexpr size_t c_MaxPayloadB64Chars = 87384; // 64 KiB decoded payload, base64 on the wire
		inline constexpr int64_t c_MaxIntField = 1000000000;  // server bound on count/version fields
		inline constexpr int64_t c_MinListenPort = 1;
		inline constexpr int64_t c_MaxListenPort = 65535;
	}

	// POST /v1/sessions request body.
	struct NetDirectoryRegisterRequest {
		std::string name;
		std::string activity;
		std::string scene;
		std::string mode;
		int64_t peerCount = 0;
		int64_t seatsFree = 0;
		std::string gameVersion;
		std::string buildId;
		int64_t networkProtocolVersion = 0;
		int64_t lockstepCodecVersion = 0;
		int64_t controllerFrameVersion = 0;
		std::string matchConfigHash;
		std::string sessionIdentityHash;
		std::string moduleManifestHash;
		int64_t listenPort = 0;
		std::vector<std::string> listenAddrs;
		std::string joinMode; //!< "ip" | "ice" | "either"

		bool operator==(const NetDirectoryRegisterRequest&) const = default;
	};

	// POST /v1/sessions 200 response body.
	struct NetDirectoryRegisterResponse {
		std::string sessionId;
		std::string token;
		int64_t expiresInS = 0;
		int64_t heartbeatS = 0;
		std::string observedIp;

		bool operator==(const NetDirectoryRegisterResponse&) const = default;
	};

	// POST /v1/sessions/{id}/heartbeat request body. listen_addrs and state are optional.
	struct NetDirectoryHeartbeatRequest {
		std::string token;
		int64_t peerCount = 0;
		int64_t seatsFree = 0;
		std::optional<std::vector<std::string>> listenAddrs;
		std::optional<std::string> state; //!< "lobby" | "running"

		bool operator==(const NetDirectoryHeartbeatRequest&) const = default;
	};

	// Heartbeat 200 response body.
	struct NetDirectoryHeartbeatResponse {
		int64_t expiresInS = 0;
		int64_t heartbeatS = 0;

		bool operator==(const NetDirectoryHeartbeatResponse&) const = default;
	};

	// DELETE /v1/sessions/{id} request body.
	struct NetDirectoryDeleteRequest {
		std::string token;

		bool operator==(const NetDirectoryDeleteRequest&) const = default;
	};

	// DELETE 200 response body.
	struct NetDirectoryDeleteResponse {
		bool ok = false;

		bool operator==(const NetDirectoryDeleteResponse&) const = default;
	};

	// One row of GET /v1/sessions: the register fields plus session_id, age_s, observed_ip, state.
	struct NetDirectorySessionRow {
		std::string name;
		std::string activity;
		std::string scene;
		std::string mode;
		int64_t peerCount = 0;
		int64_t seatsFree = 0;
		std::string gameVersion;
		std::string buildId;
		int64_t networkProtocolVersion = 0;
		int64_t lockstepCodecVersion = 0;
		int64_t controllerFrameVersion = 0;
		std::string matchConfigHash;
		std::string sessionIdentityHash;
		std::string moduleManifestHash;
		int64_t listenPort = 0;
		std::vector<std::string> listenAddrs;
		std::string joinMode;
		std::string sessionId;
		int64_t ageS = 0;
		std::string observedIp;
		std::string state; //!< "lobby" | "running"

		bool operator==(const NetDirectorySessionRow&) const = default;
	};

	// GET /v1/sessions 200 response body.
	struct NetDirectoryListResponse {
		std::vector<NetDirectorySessionRow> sessions;

		bool operator==(const NetDirectoryListResponse&) const = default;
	};

	// POST /v1/sessions/{id}/signal request body.
	struct NetDirectorySignalPost {
		std::string tokenOrJoinNonce;
		std::string from; //!< "host" | "client:<nonce>"
		std::string to;
		std::string payloadB64;

		bool operator==(const NetDirectorySignalPost&) const = default;
	};

	// Signal POST 200 response body.
	struct NetDirectorySignalPostResponse {
		bool ok = false;
		int64_t seq = 0;

		bool operator==(const NetDirectorySignalPostResponse&) const = default;
	};

	// One item of the GET /v1/sessions/{id}/signals response.
	struct NetDirectorySignal {
		int64_t seq = 0;
		std::string from;
		std::string to;
		std::string payloadB64;

		bool operator==(const NetDirectorySignal&) const = default;
	};

	// GET /v1/sessions/{id}/signals 200 response body.
	struct NetDirectorySignalList {
		std::vector<NetDirectorySignal> signals;

		bool operator==(const NetDirectorySignalList&) const = default;
	};

	// The local build's compatibility identity: what a listed row must equal to be joinable.
	struct NetDirectoryLocalIdentity {
		int64_t networkProtocolVersion = 0;
		int64_t lockstepCodecVersion = 0;
		int64_t controllerFrameVersion = 0;
		std::string sessionIdentityHash;
		std::string moduleManifestHash;

		bool operator==(const NetDirectoryLocalIdentity&) const = default;
	};

	// JSON codec for the session-directory API. Decode is fail-closed and never throws: a missing
	// required field, a wrong type, a string over the cap, more than c_MaxListenAddrs listen_addrs,
	// or a body over c_MaxBodyBytes returns false with a reason string.
	class NetDirectoryCodec {
	public:
		static std::string EncodeRegisterRequest(const NetDirectoryRegisterRequest& request);
		static bool DecodeRegisterRequest(const std::string& body, NetDirectoryRegisterRequest& out, std::string& reason);

		static std::string EncodeRegisterResponse(const NetDirectoryRegisterResponse& response);
		static bool DecodeRegisterResponse(const std::string& body, NetDirectoryRegisterResponse& out, std::string& reason);

		static std::string EncodeHeartbeatRequest(const NetDirectoryHeartbeatRequest& request);
		static bool DecodeHeartbeatRequest(const std::string& body, NetDirectoryHeartbeatRequest& out, std::string& reason);

		static std::string EncodeHeartbeatResponse(const NetDirectoryHeartbeatResponse& response);
		static bool DecodeHeartbeatResponse(const std::string& body, NetDirectoryHeartbeatResponse& out, std::string& reason);

		static std::string EncodeDeleteRequest(const NetDirectoryDeleteRequest& request);
		static bool DecodeDeleteRequest(const std::string& body, NetDirectoryDeleteRequest& out, std::string& reason);

		static std::string EncodeDeleteResponse(const NetDirectoryDeleteResponse& response);
		static bool DecodeDeleteResponse(const std::string& body, NetDirectoryDeleteResponse& out, std::string& reason);

		static std::string EncodeSessionRow(const NetDirectorySessionRow& row);
		static bool DecodeSessionRow(const std::string& body, NetDirectorySessionRow& out, std::string& reason);

		static std::string EncodeListResponse(const NetDirectoryListResponse& response);
		static bool DecodeListResponse(const std::string& body, NetDirectoryListResponse& out, std::string& reason);

		static std::string EncodeSignalPost(const NetDirectorySignalPost& post);
		static bool DecodeSignalPost(const std::string& body, NetDirectorySignalPost& out, std::string& reason);

		static std::string EncodeSignalPostResponse(const NetDirectorySignalPostResponse& response);
		static bool DecodeSignalPostResponse(const std::string& body, NetDirectorySignalPostResponse& out, std::string& reason);

		static std::string EncodeSignalList(const NetDirectorySignalList& list);
		static bool DecodeSignalList(const std::string& body, NetDirectorySignalList& out, std::string& reason);

		// True only when the row's protocol/codec/frame versions and identity/module hashes all
		// equal the local values. build_id and match_config_hash are informational.
		static bool IsJoinable(const NetDirectorySessionRow& row, const NetDirectoryLocalIdentity& local, std::string* reason = nullptr);
	};

	namespace NetDirectorySelfTest { int Run(); }

} // namespace RTE
