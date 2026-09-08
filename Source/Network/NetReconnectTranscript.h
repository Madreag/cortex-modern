#pragma once

#include "NetProtocol.h"

#include <array>
#include <cstdint>
#include <vector>

namespace RTE {

	/// What a proof authorizes. The tag is part of the signed bytes, so a reclaim proof can never be
	/// replayed as a substitution proof or as the mac over a stored ticket.
	enum class NetH4ProofDomain : uint8_t {
		Reclaim = 0,
		Substitution = 1,
		Ticket = 2,
	};

	constexpr size_t c_NetH4DomainTagBytes = 16;
	constexpr size_t c_NetH4TranscriptBytes = 88;

	using NetH4TranscriptBytes = std::array<uint8_t, c_NetH4TranscriptBytes>;

	/// The fields a proof binds itself to: which protocol carried it, which hosted session, which
	/// seat under which holder, and the one challenge/nonce pair it answers.
	struct NetH4Transcript {
		NetH4ProofDomain domain = NetH4ProofDomain::Reclaim;
		uint16_t protocolVersion = NetProtocol::c_Version;
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		NetAuthBytes32 challenge{};
		NetAuthBytes16 clientNonce{};

		bool operator==(const NetH4Transcript&) const = default;
	};

	/// The domain's 16 B zero-padded tag.
	const char* NetH4DomainTag(NetH4ProofDomain domain);

	/// Serializes the transcript: tag 16 B, protocol version u16, epoch 16 B, stable seat u16,
	/// holder generation u32, challenge 32 B, client nonce 16 B, little-endian in that order.
	/// @return Whether the transcript is well formed; generation 0 holds no credential to prove.
	bool NetH4BuildTranscript(const NetH4Transcript& transcript, NetH4TranscriptBytes& out);

	/// Computes the proof mac over the transcript. Fails closed on a malformed transcript or a
	/// provider that cannot mac.
	bool NetH4ComputeProof(const NetAuthBytes32& credential, const NetH4Transcript& transcript, NetAuthBytes32& mac);

	/// Verifies a proof in constant time. A provider failure is a verification failure, never a pass.
	bool NetH4VerifyProof(const NetAuthBytes32& credential, const NetH4Transcript& transcript, const NetAuthBytes32& mac);

	/// Macs a stored ticket record: the ticket domain tag followed by the record's canonical bytes.
	/// The tag is what keeps a stored record from ever verifying as a reclaim or substitution proof.
	bool NetH4MacTicketRecord(const NetAuthBytes32& credential, const std::vector<uint8_t>& recordBytes, NetAuthBytes32& mac);

	/// Verifies a stored ticket record's mac in constant time.
	bool NetH4VerifyTicketRecord(const NetAuthBytes32& credential, const std::vector<uint8_t>& recordBytes, const NetAuthBytes32& mac);

	/// Draws a server challenge / client nonce / transaction id from the CSPRNG.
	/// @return Whether the draw succeeded; a refusal means no auth material may be issued.
	bool NetH4DrawChallenge(NetAuthBytes32& challenge);
	bool NetH4DrawNonce(NetAuthBytes16& nonce);
	bool NetH4DrawTxId(NetAuthBytes16& txId);

} // namespace RTE
