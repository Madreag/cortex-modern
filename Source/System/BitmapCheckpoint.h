#pragma once
#include "CheckpointArchive.h"
#include "allegro.h"
#include <cstring>
#include <iostream>
#include <memory>

namespace RTE {
	struct BitmapCheckpoint {
        int width = 0, height = 0, depth = 0;
        int clip = TRUE, clipLeft = 0, clipTop = 0, clipRight = 0, clipBottom = 0;
		std::string pixels;
		void Capture(BITMAP* bitmap) {
            if (!bitmap) { *this = BitmapCheckpoint{}; return; }
            width = bitmap->w; height = bitmap->h; depth = bitmap_color_depth(bitmap);
            clip = bitmap->clip; clipLeft = bitmap->cl; clipTop = bitmap->ct;
            clipRight = bitmap->cr; clipBottom = bitmap->cb;
			const size_t stride = static_cast<size_t>(width) * ((depth + 7) / 8);
			pixels.resize(stride * height);
			for (int y = 0; y < height; ++y) std::memcpy(pixels.data() + y * stride, bitmap->line[y], stride);
		}
		std::string SaveCheckpoint() const {
            CheckpointWriter writer("Bitmap2");
            writer(width, height, depth, pixels, clip, clipLeft, clipTop, clipRight, clipBottom);
            return writer.Text();
		}
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
			try {
                const bool legacy = text.starts_with("7 Bitmap1 ");
                CheckpointReader reader(text, legacy ? "Bitmap1" : "Bitmap2");
                BitmapCheckpoint value;
                reader.Value(value.width); reader.Value(value.height); reader.Value(value.depth); reader.Value(value.pixels);
                value.clipRight = value.width; value.clipBottom = value.height;
                if (!legacy) {
                    reader.Value(value.clip); reader.Value(value.clipLeft); reader.Value(value.clipTop);
                    reader.Value(value.clipRight); reader.Value(value.clipBottom);
                }
                reader.Finish();
				if (value.width < 0 || value.height < 0 || ((value.width == 0) != (value.height == 0))) return false;
				if (value.width && value.depth != 8 && value.depth != 15 && value.depth != 16 && value.depth != 24 && value.depth != 32) return false;
                if (static_cast<uint64_t>(value.width) * value.height * ((value.depth + 7) / 8) != value.pixels.size()) return false;
                if (value.clipLeft < 0 || value.clipTop < 0 ||
                    value.clipRight < value.clipLeft || value.clipBottom < value.clipTop ||
                    value.clipRight > value.width || value.clipBottom > value.height) return false;
				if (!validateOnly) *this = std::move(value);
				return true;
			} catch (const std::exception&) { return false; }
		}
		BITMAP* Create() const {
			if (!width) return nullptr;
			BITMAP* bitmap = create_bitmap_ex(depth, width, height);
			if (!bitmap) throw std::runtime_error("could not allocate checkpoint bitmap");
			const size_t stride = static_cast<size_t>(width) * ((depth + 7) / 8);
            for (int y = 0; y < height; ++y) std::memcpy(bitmap->line[y], pixels.data() + y * stride, stride);
            bitmap->clip = clip; bitmap->cl = clipLeft; bitmap->ct = clipTop;
            bitmap->cr = clipRight; bitmap->cb = clipBottom;
            return bitmap;
        }
        static bool RunSelfTest() {
            bool passed = true;
            size_t checked = 0;
            const auto check = [&](const char* name, bool result) {
                passed = result && passed; ++checked;
                std::cout << "[bitmap-checkpoint-selftest] " << (result ? "PASS " : "FAIL ") << name << std::endl;
            };
            try {
                using Image = std::unique_ptr<BITMAP, decltype(&destroy_bitmap)>;
                for (const int colorDepth: {8, 15, 16, 24, 32}) {
                    Image original(create_bitmap_ex(colorDepth, 9, 7), &destroy_bitmap);
                    if (!original) throw std::runtime_error("bitmap fixture allocation failed");
                    clear_to_color(original.get(), 0);
                    putpixel(original.get(), 3, 2, 31);
                    set_clip_rect(original.get(), 2, 1, 5, 4);
                    BitmapCheckpoint saved; saved.Capture(original.get());
                    const auto text = saved.SaveCheckpoint();
                    BitmapCheckpoint loaded;
                    check("decode_valid_clipped_bitmap", loaded.LoadCheckpoint(text));
                    Image restored(loaded.Create(), &destroy_bitmap);
                    check("pixels_and_clip_values", getpixel(restored.get(), 3, 2) == 31 &&
                        restored->clip == original->clip && restored->cl == 2 && restored->ct == 1 &&
                        restored->cr == 6 && restored->cb == 5);
                    putpixel(restored.get(), 0, 0, 7); putpixel(restored.get(), 4, 3, 13);
                    check("clipped_draw_continuation", getpixel(restored.get(), 0, 0) == 0 && getpixel(restored.get(), 4, 3) == 13);
                    auto invalid = saved; invalid.clipLeft = 10;
                    check("invalid_clip_atomic", !loaded.LoadCheckpoint(invalid.SaveCheckpoint()) && loaded.SaveCheckpoint() == text);
                    check("truncated_atomic", !loaded.LoadCheckpoint(text.substr(0, text.size() - 2)) && loaded.SaveCheckpoint() == text);
                    CheckpointWriter legacy("Bitmap1"); legacy(saved.width, saved.height, saved.depth, saved.pixels);
                    check("legacy_full_clip", loaded.LoadCheckpoint(legacy.Text()) && loaded.clip == TRUE &&
                        loaded.clipLeft == 0 && loaded.clipTop == 0 && loaded.clipRight == 9 && loaded.clipBottom == 7);
                    saved.Capture(nullptr);
                    check("empty_replaces_previous_image", saved.width == 0 && saved.pixels.empty() && saved.Create() == nullptr);
                }
            } catch (const std::exception& error) {
                passed = false; std::cout << "[bitmap-checkpoint-selftest] " << error.what() << std::endl;
            }
            std::cout << "[bitmap-checkpoint-selftest] " << (passed ? "PASS " : "FAIL ") << "complete checked=" << checked << std::endl;
            return passed;
        }
	};
}
