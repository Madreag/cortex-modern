#pragma once

#include "glad/gl.h"
#include <SDL3/SDL.h>

#include <array>
#include <limits>
#include <span>
#include <string>

namespace RTE {

	/// Reads the rendered texture as bottom-up, tightly packed RGB without changing the renderer's GL state.
	inline bool ReadTextureRGB(GLuint texture, int width, int height, std::span<unsigned char> pixels, std::string& error) {
		if (!SDL_GL_GetCurrentContext() || !glad_glGetError || !glad_glGetIntegerv || !glad_glBindBuffer ||
		    !glad_glPixelStorei || !glad_glBindTexture || !glad_glGetTexLevelParameteriv || !glad_glGetTexImage) {
			error = "no current desktop OpenGL context with texture readback functions";
			return false;
		}
		const std::size_t maxBytes = std::numeric_limits<std::size_t>::max();
		if (!texture || width <= 0 || height <= 0 || static_cast<std::size_t>(width) > maxBytes / 3 ||
		    static_cast<std::size_t>(height) > maxBytes / (static_cast<std::size_t>(width) * 3) ||
		    pixels.size() < static_cast<std::size_t>(width) * height * 3) {
			error = "invalid texture dimensions or RGB destination size";
			return false;
		}
		if (const GLenum previous = glad_glGetError(); previous != GL_NO_ERROR) {
			error = "GL error before texture readback: " + std::to_string(previous);
			return false;
		}
		struct PackState {
			static constexpr std::array<GLenum, 6> Parameters() {
				return {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES};
			}
			GLint texture = 0;
			GLint buffer = 0;
			std::array<GLint, 6> layout{};
			PackState() {
				glad_glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
				glad_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
				for (std::size_t i = 0; i < layout.size(); ++i) glad_glGetIntegerv(Parameters()[i], &layout[i]);
			}
			~PackState() {
				for (std::size_t i = 0; i < layout.size(); ++i) glad_glPixelStorei(Parameters()[i], layout[i]);
				glad_glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(buffer));
				glad_glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
			}
		};
		GLenum readError = GL_NO_ERROR;
		bool dimensionsMatch = false;
		{
			PackState restore;
			glad_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			for (GLenum parameter : PackState::Parameters()) glad_glPixelStorei(parameter, parameter == GL_PACK_ALIGNMENT ? 1 : 0);
			glad_glBindTexture(GL_TEXTURE_2D, texture);
			GLint actualWidth = 0, actualHeight = 0;
			glad_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actualWidth);
			glad_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actualHeight);
			dimensionsMatch = actualWidth == width && actualHeight == height;
			readError = glad_glGetError();
			if (dimensionsMatch && readError == GL_NO_ERROR) {
				glad_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
				readError = glad_glGetError();
			}
		}
		const GLenum restoreError = glad_glGetError();
		if (!dimensionsMatch || readError != GL_NO_ERROR || restoreError != GL_NO_ERROR) {
			error = !dimensionsMatch ? "rendered texture dimensions differ from the capture" :
			    "texture readback GL error " + std::to_string(readError) + ", restore error " + std::to_string(restoreError);
			return false;
		}
		return true;
	}

} // namespace RTE
