#include "../Source/Renderer/FrameCaptureStream.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
	if (argc < 4) return 2;
	const int fps = std::atoi(argv[2]);
	std::ofstream output(argv[1], std::ios::binary);
	const auto header = RTE::FrameCaptureStream::Header(16, 16, fps);
	if (!output || header.empty()) return 3;
	output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
	for (int frame = 3; frame < argc; ++frame) {
		const auto slot = std::strtoull(argv[frame], nullptr, 10);
		std::vector<unsigned char> pixels(16 * 16 * 3);
		for (std::size_t byte = 0; byte < pixels.size(); ++byte) pixels[byte] = static_cast<unsigned char>((frame - 3) * 67 + byte * 13);
		const auto prefix = RTE::FrameCaptureStream::FrameHeader(slot, fps, pixels.size());
		if (prefix.empty()) return 4;
		output.write(reinterpret_cast<const char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
		output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
	}
	return output.good() ? 0 : 5;
}
