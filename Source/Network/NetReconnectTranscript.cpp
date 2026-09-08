#include "NetReconnectTranscript.h"

#include "NetAuthCrypto.h"

#include <cstring>

namespace RTE {

	namespace {
		// Fixed width, so the tag needs no length prefix under the transcript's all-fixed-width rule.
		constexpr char c_ReclaimTag[c_NetH4DomainTagBytes + 1] = "CCCP.H4.RECLAIM";
		constexpr char c_SubstitutionTag[c_NetH4DomainTagBytes + 1] = "CCCP.H4.SUBST\0\0";
		constexpr char c_TicketTag[c_NetH4DomainTagBytes + 1] = "CCCP.H4.TICKET\0";

		void WriteU16LE(uint8_t* out, uint16_t value) {
			out[0] = static_cast<uint8_t>(value & 0xFFU);
			out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
		}

		void WriteU32LE(uint8_t* out, uint32_t value) {
			for (int i = 0; i < 4; ++i) {
				out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFU);
			}
		}
	} // namespace

	const char* NetH4DomainTag(NetH4ProofDomain domain) {
		switch (domain) {
			case NetH4ProofDomain::Reclaim: return c_ReclaimTag;
			case NetH4ProofDomain::Substitution: return c_SubstitutionTag;
			case NetH4ProofDomain::Ticket: return c_TicketTag;
		}
		return nullptr;
	}

	bool NetH4BuildTranscript(const NetH4Transcript& transcript, NetH4TranscriptBytes& out) {
		const char* tag = NetH4DomainTag(transcript.domain);
		if (tag == nullptr || transcript.holderGeneration == 0) {
			return false;
		}
		out.fill(0);
		size_t offset = 0;
		std::memcpy(out.data() + offset, tag, c_NetH4DomainTagBytes);
		offset += c_NetH4DomainTagBytes;
		WriteU16LE(out.data() + offset, transcript.protocolVersion);
		offset += 2;
		std::memcpy(out.data() + offset, transcript.epoch.data(), transcript.epoch.size());
		offset += transcript.epoch.size();
		WriteU16LE(out.data() + offset, transcript.stableSeat);
		offset += 2;
		WriteU32LE(out.data() + offset, transcript.holderGeneration);
		offset += 4;
		std::memcpy(out.data() + offset, transcript.challenge.data(), transcript.challenge.size());
		offset += transcript.challenge.size();
		std::memcpy(out.data() + offset, transcript.clientNonce.data(), transcript.clientNonce.size());
		offset += transcript.clientNonce.size();
		return offset == out.size();
	}

	bool NetH4ComputeProof(const NetAuthBytes32& credential, const NetH4Transcript& transcript, NetAuthBytes32& mac) {
		NetH4TranscriptBytes bytes{};
		if (!NetH4BuildTranscript(transcript, bytes)) {
			return false;
		}
		uint8_t raw[32] = {};
		if (!GetNetAuthCrypto().HmacSha256(credential.data(), credential.size(), bytes.data(), bytes.size(), raw)) {
			return false;
		}
		std::memcpy(mac.data(), raw, sizeof(raw));
		return true;
	}

	bool NetH4VerifyProof(const NetAuthBytes32& credential, const NetH4Transcript& transcript, const NetAuthBytes32& mac) {
		NetAuthBytes32 expected{};
		if (!NetH4ComputeProof(credential, transcript, expected)) {
			return false;
		}
		return NetAuthConstantTimeEquals(expected.data(), mac.data(), expected.size());
	}

	bool NetH4DrawChallenge(NetAuthBytes32& challenge) {
		return GetNetAuthCrypto().RandomBytes(challenge.data(), challenge.size());
	}

	bool NetH4DrawNonce(NetAuthBytes16& nonce) {
		return GetNetAuthCrypto().RandomBytes(nonce.data(), nonce.size());
	}

	bool NetH4DrawTxId(NetAuthBytes16& txId) {
		return GetNetAuthCrypto().RandomBytes(txId.data(), txId.size());
	}

} // namespace RTE
