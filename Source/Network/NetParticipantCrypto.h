#pragma once

#include "NetProtocol.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	using NetParticipantId = NetAuthBytes32;
	using NetParticipantSignature = std::array<uint8_t, 64>;

	/// Signed connection proof: host, session and this transport, never a seat number.
	struct NetParticipantProofTranscript {
		uint16_t protocolVersion = NetProtocol::c_Version;
		NetAuthBytes32 hostBinding{};
		uint64_t sessionId = 0;
		NetAuthBytes16 connectionBinding{};
		NetAuthBytes16 challenge{};
	};

	enum class NetParticipantProofVerdict : uint8_t {
		Accept = 0,
		RejectReplay = 1,
		RejectForgery = 2,
		RejectCrossHost = 3,
		RejectOldVersion = 4,
		RejectUnproven = 5,
	};

	const char* NetParticipantProofVerdictName(NetParticipantProofVerdict verdict);
	bool NetParticipantProofBytes(const NetParticipantProofTranscript& transcript, std::vector<uint8_t>& out);
	NetParticipantProofVerdict NetAcceptParticipantProof(const NetParticipantChallenge& challenge, const NetParticipantProof& proof, const NetAuthBytes32& expectedHostBinding, uint64_t expectedSessionId, bool challengeSpent);

	/// Ed25519 signer/verifier. Separate from NetAuthCrypto (HMAC + CSPRNG only).
	class NetParticipantCrypto {
	public:
		virtual ~NetParticipantCrypto() = default;
		virtual bool IsRealCrypto() const = 0;
		virtual bool GenerateKey(uint8_t (&priv)[32], uint8_t (&pub)[32]) = 0;
		virtual bool PublicFromPrivate(const uint8_t (&priv)[32], uint8_t (&pub)[32]) = 0;
		virtual bool Sign(const uint8_t (&priv)[32], const uint8_t* message, size_t messageCount, uint8_t (&signature)[64]) = 0;
		virtual bool Verify(const uint8_t (&pub)[32], const uint8_t* message, size_t messageCount, const uint8_t (&signature)[64]) = 0;
	};

	NetParticipantCrypto& GetNetParticipantCrypto();
	void SetNetParticipantCryptoForTest(NetParticipantCrypto* provider);

	/// One local participant key. The public half is the identity; the private half never leaves this store.
	class NetParticipantIdentityStore {
	public:
		static constexpr uint16_t c_RecordVersion = 1;
		static std::string DefaultPath();

		void SetPath(std::string path);
		const std::string& GetPath() const { return m_Path; }
		bool LoadOrCreate(std::string* error = nullptr);
		bool HasKey() const { return m_HasKey; }
		const NetParticipantId& PublicId() const { return m_Public; }
		bool Sign(const std::vector<uint8_t>& message, NetParticipantSignature& signature) const;
		/// A symmetric key for sealing this install's own files, derived from the private half under a
		/// purpose label: HMAC-SHA-256(private key, label). The private half still never leaves the store,
		/// and two purposes never share a key.
		/// @return Whether a key was derived; false without a key or without real crypto.
		bool DeriveLocalKey(const std::string& label, std::array<uint8_t, 32>& out) const;

	private:
		std::string m_Path = DefaultPath();
		NetParticipantId m_Public{};
		std::array<uint8_t, 32> m_Private{};
		bool m_HasKey = false;
	};

} // namespace RTE
