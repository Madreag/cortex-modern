#pragma once

#include "glad/gl.h"
#include <SDL3/SDL.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

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

	/// The GPU owns this immutable copy until the writer has read it; the texture may be rendered again meanwhile.
	class QueuedTextureReadback {
	public:
		GLuint buffer = 0;
		GLsync ready = nullptr;
		int width = 0, height = 0;
	};

	/// A private shared context completes capture transfers on a writer, leaving the render context's state intact.
	class FrameReadbackContext {
	public:
		static std::unique_ptr<FrameReadbackContext> Create(std::string& error) {
			if (!SDL_GL_GetCurrentContext() || !SDL_GL_GetCurrentWindow() || !glad_glGetError || !glad_glGetIntegerv ||
			    !glad_glBindBuffer || !glad_glGenBuffers || !glad_glDeleteBuffers || !glad_glBufferData || !glad_glGetBufferSubData ||
			    !glad_glPixelStorei || !glad_glBindTexture || !glad_glGetTexLevelParameteriv || !glad_glGetTexImage ||
			    !glad_glFenceSync || !glad_glClientWaitSync || !glad_glDeleteSync || !glad_glFlush) {
				error = "no current context with shared pixel-buffer readback functions";
				return nullptr;
			}
			auto result = std::make_unique<FrameReadbackContext>();
			SDL_Window* originalWindow = SDL_GL_GetCurrentWindow();
			SDL_GLContext originalContext = SDL_GL_GetCurrentContext();
			int originalShare = 0;
			if (!SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, &originalShare) ||
			    !SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1)) {
				error = "capture context sharing: " + std::string(SDL_GetError());
				return nullptr;
			}
			result->m_Window = SDL_CreateWindow("Frame readback", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
			if (result->m_Window) result->m_Context = SDL_GL_CreateContext(result->m_Window);
			if (!result->m_Context) error = "private capture context: " + std::string(SDL_GetError());
			const bool restored = SDL_GL_MakeCurrent(originalWindow, originalContext);
			const bool attributeRestored = SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, originalShare);
			if (!restored || !attributeRestored) error = "restore render context after capture setup: " + std::string(SDL_GetError());
			if (!result->m_Context || !restored || !attributeRestored) return nullptr;
			return result;
		}

		~FrameReadbackContext() {
			if (m_Context) SDL_GL_DestroyContext(m_Context);
			if (m_Window) SDL_DestroyWindow(m_Window);
		}

		static void Discard(QueuedTextureReadback& frame) {
			if (frame.ready) glad_glDeleteSync(frame.ready);
			if (frame.buffer) glad_glDeleteBuffers(1, &frame.buffer);
			frame.ready = nullptr;
			frame.buffer = 0;
		}

		std::unique_ptr<QueuedTextureReadback> Submit(GLuint texture, int width, int height, std::string& error) {
			const std::size_t max = std::min<std::size_t>(std::numeric_limits<std::size_t>::max(), std::numeric_limits<GLsizeiptr>::max());
			if (!SDL_GL_GetCurrentContext() || !texture || width <= 0 || height <= 0 ||
			    static_cast<std::size_t>(width) > max / 4 || static_cast<std::size_t>(height) > max / (static_cast<std::size_t>(width) * 4)) {
				error = "invalid queued texture dimensions or context";
				return nullptr;
			}
			if (const GLenum previous = glad_glGetError(); previous != GL_NO_ERROR) {
				error = "GL error before queued readback: " + std::to_string(previous);
				return nullptr;
			}
			GLint oldBuffer = 0, oldTexture = 0;
			constexpr std::array<GLenum, 6> parameters{GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES};
			std::array<GLint, 6> oldPack{};
			glad_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldBuffer);
			glad_glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
			for (std::size_t i = 0; i < parameters.size(); ++i) glad_glGetIntegerv(parameters[i], &oldPack[i]);
			auto result = std::make_unique<QueuedTextureReadback>();
			result->width = width; result->height = height;
			glad_glBindTexture(GL_TEXTURE_2D, texture);
			GLint actualWidth = 0, actualHeight = 0;
			GLenum allocationError = GL_NO_ERROR;
			glad_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actualWidth);
			glad_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actualHeight);
			if (actualWidth == width && actualHeight == height) {
				glad_glGenBuffers(1, &result->buffer);
				if (result->buffer) {
					glad_glBindBuffer(GL_PIXEL_PACK_BUFFER, result->buffer);
					glad_glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(static_cast<std::size_t>(width) * height * 4), nullptr, GL_STREAM_READ);
					allocationError = glad_glGetError();
					if (allocationError == GL_NO_ERROR) {
						for (GLenum parameter : parameters) glad_glPixelStorei(parameter, parameter == GL_PACK_ALIGNMENT ? 1 : 0);
						// RGBA avoids a driver's synchronous three-channel conversion; alpha is discarded on the writer.
						glad_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
						result->ready = glad_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
						glad_glFlush();
					}
				}
			}
			const GLenum transferError = glad_glGetError();
			for (std::size_t i = 0; i < parameters.size(); ++i) glad_glPixelStorei(parameters[i], oldPack[i]);
			glad_glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(oldBuffer));
			glad_glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
			const GLenum restoreError = glad_glGetError();
			if (!result->buffer || !result->ready || allocationError != GL_NO_ERROR || transferError != GL_NO_ERROR || restoreError != GL_NO_ERROR) {
				if (result->ready) glad_glDeleteSync(result->ready);
				if (result->buffer) glad_glDeleteBuffers(1, &result->buffer);
				error = actualWidth != width || actualHeight != height ? "queued texture dimensions differ from capture" :
				    "queued readback allocation error " + std::to_string(allocationError) + ", transfer error " + std::to_string(transferError) + ", restore error " + std::to_string(restoreError);
				return nullptr;
			}
			return result;
		}

		bool Complete(QueuedTextureReadback& frame, std::span<unsigned char> rgb, std::string& error) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!SDL_GL_MakeCurrent(m_Window, m_Context)) {
				error = "writer capture context: " + std::string(SDL_GetError());
				return false;
			}
			bool done = false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
			while (std::chrono::steady_clock::now() < deadline) {
				const GLenum state = glad_glClientWaitSync(frame.ready, 0, 1000000);
				if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED) { done = true; break; }
				if (state == GL_WAIT_FAILED) { error = "queued pixel transfer fence failed"; break; }
			}
			if (!done && error.empty()) error = "queued pixel transfer did not finish within 90 seconds";
			if (done) {
				m_RGBA.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
				glad_glBindBuffer(GL_PIXEL_PACK_BUFFER, frame.buffer);
				glad_glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(m_RGBA.size()), m_RGBA.data());
				const GLenum readError = glad_glGetError();
				done = readError == GL_NO_ERROR && rgb.size() == static_cast<std::size_t>(frame.width) * frame.height * 3;
				if (!done) error = "queued pixel copy GL error " + std::to_string(readError) + " or RGB size mismatch";
				if (done) {
					for (int y = 0; y < frame.height; ++y) {
						const auto* source = m_RGBA.data() + static_cast<std::size_t>(frame.height - y - 1) * frame.width * 4;
						auto* destination = rgb.data() + static_cast<std::size_t>(y) * frame.width * 3;
						for (int x = 0; x < frame.width; ++x) {
							for (int channel = 0; channel < 3; ++channel) destination[x * 3 + channel] = source[x * 4 + channel];
						}
					}
				}
			}
			glad_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			Discard(frame);
			if (const GLenum cleanupError = glad_glGetError(); cleanupError != GL_NO_ERROR) {
				error += " queued readback cleanup GL error " + std::to_string(cleanupError);
				done = false;
			}
			if (!SDL_GL_MakeCurrent(nullptr, nullptr) && done) { error = "writer capture context release: " + std::string(SDL_GetError()); done = false; }
			return done;
		}

	private:
		SDL_Window* m_Window = nullptr;
		SDL_GLContext m_Context = nullptr;
		std::mutex m_Mutex;
		std::vector<unsigned char> m_RGBA;
	};

} // namespace RTE
