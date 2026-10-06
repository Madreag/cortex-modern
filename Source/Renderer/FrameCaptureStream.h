#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace RTE::FrameCaptureStream {

	using Bytes = std::vector<unsigned char>;

	inline void Number(Bytes& output, std::uint64_t value, unsigned int width = 0) {
		if (!width) {
			width = 1;
			while (width < 8 && (value >> (width * 8))) ++width;
		}
		for (unsigned int byte = width; byte; --byte) output.push_back(static_cast<unsigned char>(value >> ((byte - 1) * 8)));
	}

	inline void Identifier(Bytes& output, std::uint32_t value) {
		unsigned int width = 1;
		while (width < 4 && (value >> (width * 8))) ++width;
		Number(output, value, width);
	}

	inline bool Size(Bytes& output, std::uint64_t value) {
		for (unsigned int width = 1; width <= 8; ++width) {
			const std::uint64_t flag = std::uint64_t{1} << (width * 7);
			if (value < flag - 1) {
				Number(output, value | flag, width);
				return true;
			}
		}
		return false;
	}

	inline void Element(Bytes& output, std::uint32_t identifier, std::span<const unsigned char> payload) {
		Identifier(output, identifier);
		Size(output, payload.size());
		output.insert(output.end(), payload.begin(), payload.end());
	}

	inline void Unsigned(Bytes& output, std::uint32_t identifier, std::uint64_t value) {
		Bytes payload;
		Number(payload, value);
		Element(output, identifier, payload);
	}

	inline void Text(Bytes& output, std::uint32_t identifier, std::string_view value) {
		Element(output, identifier, {reinterpret_cast<const unsigned char*>(value.data()), value.size()});
	}

	/// A timestamped RGB stream lets the encoder hold missing slots without resending their pixels through the pipe.
	inline Bytes Header(int width, int height, int fps) {
		if (width <= 0 || height <= 0 || fps < 1 || fps > 60) return {};
		Bytes ebml, info, video, track, tracks, output;
		Unsigned(ebml, 0x4286, 1);
		Unsigned(ebml, 0x42F7, 1);
		Unsigned(ebml, 0x42F2, 4);
		Unsigned(ebml, 0x42F3, 8);
		Text(ebml, 0x4282, "matroska");
		Unsigned(ebml, 0x4287, 4);
		Unsigned(ebml, 0x4285, 2);
		Element(output, 0x1A45DFA3, ebml);
		Identifier(output, 0x18538067);
		// A pipe has no final segment size.
		Number(output, 0x01FFFFFFFFFFFFFF, 8);
		Unsigned(info, 0x2AD7B1, 1);
		Text(info, 0x4D80, "frame capture");
		Text(info, 0x5741, "frame capture");
		Element(output, 0x1549A966, info);
		Unsigned(video, 0xB0, static_cast<std::uint64_t>(width));
		Unsigned(video, 0xBA, static_cast<std::uint64_t>(height));
		const unsigned char rgb24[] = {'R', 'G', 'B', 24};
		Element(video, 0x2EB524, rgb24);
		Unsigned(track, 0xD7, 1);
		Unsigned(track, 0x73C5, 1);
		Unsigned(track, 0x83, 1);
		Unsigned(track, 0x9C, 0);
		Unsigned(track, 0x23E383, 1000000000 / static_cast<std::uint64_t>(fps));
		Text(track, 0x86, "V_UNCOMPRESSED");
		Element(track, 0xE0, video);
		Element(tracks, 0xAE, track);
		Element(output, 0x1654AE6B, tracks);
		return output;
	}

	/// The caller writes this prefix followed by the frame's unchanged RGB bytes.
	inline Bytes FrameHeader(std::uint64_t slot, int fps, std::size_t pixelBytes) {
		if (fps < 1 || fps > 60 || pixelBytes > (std::uint64_t{1} << 56) - 128) return {};
		const std::uint64_t seconds = slot / static_cast<std::uint64_t>(fps);
		const std::uint64_t fraction = (slot % static_cast<std::uint64_t>(fps)) * 1000000000 / static_cast<std::uint64_t>(fps);
		if (seconds > (std::numeric_limits<std::uint64_t>::max() - fraction) / 1000000000) return {};
		Bytes payload, block, output;
		Unsigned(payload, 0xE7, seconds * 1000000000 + fraction);
		Identifier(block, 0xA3);
		if (!Size(block, static_cast<std::uint64_t>(pixelBytes) + 4)) return {};
		block.insert(block.end(), {0x81, 0x00, 0x00, 0x80});
		payload.insert(payload.end(), block.begin(), block.end());
		Identifier(output, 0x1F43B675);
		if (!Size(output, payload.size() + pixelBytes)) return {};
		output.insert(output.end(), payload.begin(), payload.end());
		return output;
	}

} // namespace RTE::FrameCaptureStream
