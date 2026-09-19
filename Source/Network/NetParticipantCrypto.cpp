#include "NetParticipantCrypto.h"

#include "NetAuthCrypto.h"
#include "System/System.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef CCCP_WITH_GNS
#include <openssl/evp.h>
#endif

namespace RTE {

	namespace {
		constexpr char c_ProofDomain[] = "CCCP-PID-PROOF-v1";
		constexpr char c_Magic[8] = {'C', 'C', 'C', 'P', 'P', 'I', 'D', 'K'};

#ifdef CCCP_WITH_GNS
		class OpenSslParticipantCrypto : public NetParticipantCrypto {
		public:
			bool IsRealCrypto() const override { return true; }

			bool GenerateKey(uint8_t (&priv)[32], uint8_t (&pub)[32]) override {
				EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
				if (key == nullptr) {
					return false;
				}
				size_t privCount = 32;
				size_t pubCount = 32;
				const bool ok = EVP_PKEY_get_raw_private_key(key, priv, &privCount) == 1 && privCount == 32 &&
				                EVP_PKEY_get_raw_public_key(key, pub, &pubCount) == 1 && pubCount == 32;
				EVP_PKEY_free(key);
				return ok;
			}

			bool PublicFromPrivate(const uint8_t (&priv)[32], uint8_t (&pub)[32]) override {
				EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv, 32);
				if (key == nullptr) {
					return false;
				}
				size_t pubCount = 32;
				const bool ok = EVP_PKEY_get_raw_public_key(key, pub, &pubCount) == 1 && pubCount == 32;
				EVP_PKEY_free(key);
				return ok;
			}

			bool Sign(const uint8_t (&priv)[32], const uint8_t* message, size_t messageCount, uint8_t (&signature)[64]) override {
				if (message == nullptr && messageCount > 0) {
					return false;
				}
				EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv, 32);
				EVP_MD_CTX* context = key != nullptr ? EVP_MD_CTX_new() : nullptr;
				size_t signatureCount = 64;
				const bool ok = context != nullptr && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
				                EVP_DigestSign(context, signature, &signatureCount, message, messageCount) == 1 && signatureCount == 64;
				EVP_MD_CTX_free(context);
				EVP_PKEY_free(key);
				return ok;
			}

			bool Verify(const uint8_t (&pub)[32], const uint8_t* message, size_t messageCount, const uint8_t (&signature)[64]) override {
				if (message == nullptr && messageCount > 0) {
					return false;
				}
				EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, 32);
				EVP_MD_CTX* context = key != nullptr ? EVP_MD_CTX_new() : nullptr;
				const bool ok = context != nullptr && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
				                EVP_DigestVerify(context, signature, 64, message, messageCount) == 1;
				EVP_MD_CTX_free(context);
				EVP_PKEY_free(key);
				return ok;
			}
		};
		using DefaultParticipantCrypto = OpenSslParticipantCrypto;
#else
		class FailClosedParticipantCrypto : public NetParticipantCrypto {
		public:
			bool IsRealCrypto() const override { return false; }
			bool GenerateKey(uint8_t (&)[32], uint8_t (&)[32]) override { return false; }
			bool PublicFromPrivate(const uint8_t (&)[32], uint8_t (&)[32]) override { return false; }
			bool Sign(const uint8_t (&)[32], const uint8_t*, size_t, uint8_t (&)[64]) override { return false; }
			bool Verify(const uint8_t (&)[32], const uint8_t*, size_t, const uint8_t (&)[64]) override { return false; }
		};
		using DefaultParticipantCrypto = FailClosedParticipantCrypto;
#endif

		NetParticipantCrypto* s_TestOverride = nullptr;

		void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		bool WriteDurably(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
#ifdef _WIN32
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.wstring().c_str(), L"wb") != 0 || file == nullptr) {
				return false;
			}
#else
			FILE* file = std::fopen(path.string().c_str(), "wb");
			if (file == nullptr) {
				return false;
			}
#endif
			const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
			const bool flushed = wrote && std::fflush(file) == 0;
#ifdef _WIN32
			const bool synced = flushed && _commit(_fileno(file)) == 0;
#else
			const bool synced = flushed && fsync(fileno(file)) == 0;
#endif
			std::fclose(file);
			if (!synced) {
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
				return false;
			}
			return true;
		}
	} // namespace

	const char* NetParticipantProofVerdictName(NetParticipantProofVerdict verdict) {
		switch (verdict) {
			case NetParticipantProofVerdict::Accept: return "Accept";
			case NetParticipantProofVerdict::RejectReplay: return "RejectReplay";
			case NetParticipantProofVerdict::RejectForgery: return "RejectForgery";
			case NetParticipantProofVerdict::RejectCrossHost: return "RejectCrossHost";
			case NetParticipantProofVerdict::RejectOldVersion: return "RejectOldVersion";
			case NetParticipantProofVerdict::RejectUnproven: return "RejectUnproven";
		}
		return "Unknown";
	}

	bool NetParticipantProofBytes(const NetParticipantProofTranscript& transcript, std::vector<uint8_t>& out) {
		out.clear();
		out.insert(out.end(), std::begin(c_ProofDomain), std::end(c_ProofDomain) - 1);
		AppendU16(out, transcript.protocolVersion);
		out.insert(out.end(), transcript.hostBinding.begin(), transcript.hostBinding.end());
		AppendU64(out, transcript.sessionId);
		out.insert(out.end(), transcript.connectionBinding.begin(), transcript.connectionBinding.end());
		out.insert(out.end(), transcript.challenge.begin(), transcript.challenge.end());
		return true;
	}

	NetParticipantProofVerdict NetAcceptParticipantProof(const NetParticipantChallenge& challenge, const NetParticipantProof& proof, const NetAuthBytes32& expectedHostBinding, uint64_t expectedSessionId, bool challengeSpent) {
		if (challenge.version != c_NetParticipantIdentityVersion || proof.version != c_NetParticipantIdentityVersion) {
			return NetParticipantProofVerdict::RejectOldVersion;
		}
		if (!(challenge.hostBinding == expectedHostBinding) || challenge.sessionId != expectedSessionId) {
			return NetParticipantProofVerdict::RejectCrossHost;
		}
		if (!(proof.challenge == challenge.challenge) || !(proof.connectionBinding == challenge.connectionBinding)) {
			return NetParticipantProofVerdict::RejectCrossHost;
		}
		if (challengeSpent) {
			return NetParticipantProofVerdict::RejectReplay;
		}
		NetParticipantProofTranscript transcript;
		transcript.protocolVersion = NetProtocol::c_Version;
		transcript.hostBinding = challenge.hostBinding;
		transcript.sessionId = challenge.sessionId;
		transcript.connectionBinding = challenge.connectionBinding;
		transcript.challenge = challenge.challenge;
		std::vector<uint8_t> message;
		if (!NetParticipantProofBytes(transcript, message)) {
			return NetParticipantProofVerdict::RejectUnproven;
		}
		uint8_t pub[32];
		uint8_t signature[64];
		std::memcpy(pub, proof.publicId.data(), 32);
		std::memcpy(signature, proof.signature.data(), 64);
		if (!GetNetParticipantCrypto().Verify(pub, message.data(), message.size(), signature)) {
			return NetParticipantProofVerdict::RejectForgery;
		}
		return NetParticipantProofVerdict::Accept;
	}

	NetParticipantCrypto& GetNetParticipantCrypto() {
		static DefaultParticipantCrypto defaultProvider;
		return s_TestOverride != nullptr ? *s_TestOverride : defaultProvider;
	}

	void SetNetParticipantCryptoForTest(NetParticipantCrypto* provider) {
		s_TestOverride = provider;
	}

	std::string NetParticipantIdentityStore::DefaultPath() {
		return System::GetWorkingDirectory() + System::GetUserdataDirectory() + "NetworkIdentity.key";
	}

	void NetParticipantIdentityStore::SetPath(std::string path) {
		m_Path = std::move(path);
	}

	bool NetParticipantIdentityStore::LoadOrCreate(std::string* error) {
		m_HasKey = false;
		m_Private.fill(0);
		m_Public = {};
		const std::filesystem::path path(m_Path);
		std::error_code code;
		const bool present = std::filesystem::exists(path, code);
		if (code) {
			if (error) *error = "could not stat the participant identity";
			return false;
		}
		if (present) {
			const uintmax_t size = std::filesystem::file_size(path, code);
			constexpr uintmax_t kIdentityBytes = 8 + 2 + 32 + 32 + 32;
			if (code || size != kIdentityBytes) {
				if (error) *error = code ? "could not size the participant identity" : "the participant identity is corrupt";
				return false;
			}
			std::vector<uint8_t> bytes(static_cast<size_t>(size));
			FILE* file = nullptr;
#ifdef _WIN32
			if (_wfopen_s(&file, path.wstring().c_str(), L"rb") != 0 || file == nullptr) {
#else
			file = std::fopen(path.string().c_str(), "rb");
			if (file == nullptr) {
#endif
				if (error) *error = "could not read the participant identity";
				return false;
			}
			const bool read = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
			std::fclose(file);
			if (!read || bytes.size() != 8 + 2 + 32 + 32 + 32 || std::memcmp(bytes.data(), c_Magic, 8) != 0) {
				if (error) *error = "the participant identity is corrupt";
				return false;
			}
			const uint16_t version = static_cast<uint16_t>(bytes[8] | (static_cast<uint16_t>(bytes[9]) << 8));
			if (version != c_RecordVersion) {
				if (error) *error = "unsupported participant identity version";
				return false;
			}
			std::memcpy(m_Public.data(), bytes.data() + 10, 32);
			std::memcpy(m_Private.data(), bytes.data() + 42, 32);
			uint8_t mac[32];
			if (!GetNetAuthCrypto().HmacSha256(reinterpret_cast<const uint8_t*>(c_ProofDomain), sizeof(c_ProofDomain) - 1, bytes.data(), 74, mac) ||
			    !NetAuthConstantTimeEquals(mac, bytes.data() + 74, 32)) {
				m_Private.fill(0);
				if (error) *error = "the participant identity failed its integrity check";
				return false;
			}
			uint8_t priv[32];
			uint8_t derived[32];
			std::memcpy(priv, m_Private.data(), 32);
			if (!GetNetParticipantCrypto().PublicFromPrivate(priv, derived)) {
				m_Private.fill(0);
				if (error) *error = "the stored participant key cannot be opened";
				return false;
			}
			if (!NetAuthConstantTimeEquals(derived, m_Public.data(), 32)) {
				m_Private.fill(0);
				if (error) *error = "the stored participant key does not match its public identity";
				return false;
			}
			m_HasKey = true;
			return true;
		}
		uint8_t priv[32];
		uint8_t pub[32];
		if (!GetNetParticipantCrypto().GenerateKey(priv, pub)) {
			if (error) *error = "no crypto provider to create a participant identity";
			return false;
		}
		std::vector<uint8_t> bytes;
		bytes.insert(bytes.end(), std::begin(c_Magic), std::end(c_Magic));
		AppendU16(bytes, c_RecordVersion);
		bytes.insert(bytes.end(), pub, pub + 32);
		bytes.insert(bytes.end(), priv, priv + 32);
		uint8_t mac[32];
		if (!GetNetAuthCrypto().HmacSha256(reinterpret_cast<const uint8_t*>(c_ProofDomain), sizeof(c_ProofDomain) - 1, bytes.data(), bytes.size(), mac)) {
			if (error) *error = "no crypto provider to seal the participant identity";
			return false;
		}
		bytes.insert(bytes.end(), mac, mac + 32);
		if (path.has_parent_path() && !std::filesystem::exists(path.parent_path(), code)) {
			std::filesystem::create_directories(path.parent_path(), code);
		}
		std::filesystem::path temporary = path;
		temporary += ".tmp";
		if (!WriteDurably(temporary, bytes)) {
			if (error) *error = "could not write the participant identity";
			return false;
		}
		std::filesystem::rename(temporary, path, code);
		if (code) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			if (error) *error = "could not publish the participant identity";
			return false;
		}
		std::memcpy(m_Public.data(), pub, 32);
		std::memcpy(m_Private.data(), priv, 32);
		m_HasKey = true;
		return true;
	}

	bool NetParticipantIdentityStore::Sign(const std::vector<uint8_t>& message, NetParticipantSignature& signature) const {
		if (!m_HasKey) {
			return false;
		}
		uint8_t priv[32];
		uint8_t raw[64];
		std::memcpy(priv, m_Private.data(), 32);
		if (!GetNetParticipantCrypto().Sign(priv, message.data(), message.size(), raw)) {
			return false;
		}
		std::memcpy(signature.data(), raw, 64);
		return true;
	}

	bool NetParticipantIdentityStore::DeriveLocalKey(const std::string& label, std::array<uint8_t, 32>& out) const {
		if (!m_HasKey || label.empty()) {
			return false;
		}
		uint8_t mac[32];
		if (!GetNetAuthCrypto().HmacSha256(m_Private.data(), m_Private.size(), reinterpret_cast<const uint8_t*>(label.data()), label.size(), mac)) {
			return false;
		}
		std::memcpy(out.data(), mac, out.size());
		return true;
	}

} // namespace RTE
