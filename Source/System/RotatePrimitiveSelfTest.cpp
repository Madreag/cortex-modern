#include "RotatePrimitiveSelfTest.h"

#include "allegro.h"
#include "allegro/internal/aintern.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace RTE::RotatePrimitiveSelfTest {

	namespace {

		struct RotationCase {
			int degrees;
			fixed angle;
			int size;
			std::array<fixed, 4> xs;
			std::array<fixed, 4> ys;
			int sourceX;
			int sourceY;
			int targetX;
			int targetY;
			int pixel;
		};

		constexpr std::array<RotationCase, 8> c_Cases = {{
			{-162, -7549747, 79, {4250674, -673238, 926670, 5850582}, {5850582, 4250674, -673238, 926670}, 36, 72, 52, 8, 245},
			{-34, -1584515, 23, {-292595, 957041, 1799922, 550286}, {550287, -292594, 957042, 1799923}, 12, 7, 9, 7, 245},
			{93, 4334114, 25, {1680150, 1594400, -41750, 44000}, {44000, 1680150, 1594400, -41750}, 7, 13, 11, 7, 245},
			{78, 3635064, 72, {4176504, 5157576, 542088, -438984}, {-438984, 4176504, 5157576, 542088}, 7, 55, 11, 12, 122},
			{-119, -5545802, 25, {499862, -294438, 1138537, 1932837}, {1932838, 499863, -294437, 1138538}, 16, 14, 12, 7, 10},
			{172, 8015781, 23, {1604882, 112228, -97555, 1395099}, {1395100, 1604883, 112229, -97554}, 13, 15, 8, 7, 178},
			{178, 8295401, 97, {6465971, 112859, -108980, 6244132}, {6244133, 6465972, 112860, -108979}, 32, 63, 63, 32, 117},
			{89, 4147701, 47, {3053073, 3106841, 27119, -26649}, {-26649, 3053073, 3106841, 27119}, 9, 22, 24, 9, 117},
		}};

	}

	int Run() {
		if (install_allegro(SYSTEM_NONE, &errno, std::atexit) != 0) {
			std::cerr << "[rotate-primitive-selftest] FAIL init " << allegro_error << std::endl;
			return 1;
		}

		volatile fixed x = 65535;
		volatile fixed y = 32768;
		const fixed integerProduct = fixmul(x, y);
		const fixed floatingProduct = ftofix(fixtof(x) * fixtof(y));
		bool passed = integerProduct == 32767 && floatingProduct == 32768 && integerProduct != floatingProduct;
		if (!passed) {
			std::cerr << "[rotate-primitive-selftest] FAIL fixmul integer=" << integerProduct << " floating=" << floatingProduct << " expected=32767/32768" << std::endl;
		}

		for (const RotationCase& test : c_Cases) {
			fixed xs[4];
			fixed ys[4];
			const fixed size = itofix(test.size);
			_rotate_scale_flip_coordinates(size, size, size / 2, size / 2, size / 2, size / 2, test.angle, itofix(1), itofix(1), 0, 0, xs, ys);
			for (int corner = 0; corner < 4; ++corner) {
				if (xs[corner] != test.xs[corner] || ys[corner] != test.ys[corner]) {
					std::cerr << "[rotate-primitive-selftest] FAIL " << test.degrees << " corner " << corner << " actual=" << xs[corner] << '/' << ys[corner] << " expected=" << test.xs[corner] << '/' << test.ys[corner] << std::endl;
					passed = false;
				}
			}

			using Bitmap = std::unique_ptr<BITMAP, decltype(&destroy_bitmap)>;
			Bitmap source(create_bitmap_ex(8, test.size, test.size), destroy_bitmap);
			Bitmap target(create_bitmap_ex(8, test.size, test.size), destroy_bitmap);
			if (!source || !target) {
				std::cerr << "[rotate-primitive-selftest] FAIL " << test.degrees << " bitmap allocation" << std::endl;
				passed = false;
				continue;
			}
			clear_to_color(source.get(), 0);
			clear_to_color(target.get(), 0);
			putpixel(source.get(), test.sourceX, test.sourceY, test.pixel);
			rotate_sprite(target.get(), source.get(), 0, 0, test.angle);
			const int pixel = getpixel(target.get(), test.targetX, test.targetY);
			if (pixel != test.pixel) {
				std::cerr << "[rotate-primitive-selftest] FAIL " << test.degrees << " pixel " << test.targetX << '/' << test.targetY << " actual=" << pixel << " expected=" << test.pixel << std::endl;
				passed = false;
			}
		}

		allegro_exit();
		if (passed) {
			std::cout << "[rotate-primitive-selftest] PASS" << std::endl;
		}
		return passed ? 0 : 1;
	}

}
