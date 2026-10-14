#include "GLResourceMan.h"

#include "ContentFile.h"
#include "RTEError.h"

#include "GLCheck.h"
#include "System.h"
#include "allegro.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#endif

#include "tracy/Tracy.hpp"
#include "tracy/TracyOpenGL.hpp"

#include "raylib/raylib.h"
#include "raylib/rlgl.h"

using namespace RTE;

namespace {
	/// Every this many new bitmap textures, the call stack that made one is printed (CC_TEST_TEXTURE_CREATORS); 0 = never.
	size_t TextureCreatorEvery() {
		static const size_t every = [] {
			const char* value = std::getenv("CC_TEST_TEXTURE_CREATORS");
			return value ? std::max<size_t>(1, std::strtoull(value, nullptr, 10)) : size_t{0};
		}();
		return every;
	}

	void PrintTextureCreator(size_t made, const BITMAP* bitmap) {
		// The stack is read where the platform's own header does not collide with the renderer's names.
		void* frames[12] = {};
		int depth = 0;
		uintptr_t base = 0;
#ifndef _WIN32
		depth = backtrace(frames, 12);
		Dl_info module{};
		if (dladdr(reinterpret_cast<void*>(&PrintTextureCreator), &module)) base = reinterpret_cast<uintptr_t>(module.dli_fbase);
#endif
		std::string line = "[gl-texture-creator] made=" + std::to_string(made) + " size=" + std::to_string(bitmap->w) + "x" + std::to_string(bitmap->h) + "x" +
		    std::to_string(bitmap_color_depth(bitmap)) + " offsets=";
		char text[24];
		for (int index = 0; index < depth; ++index) {
			std::snprintf(text, sizeof(text), "%s0x%llx", index ? "," : "", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[index]) - base));
			line += text;
		}
		System::PrintDiagnosticLine(line);
	}
}

GLResourceMan::GLResourceMan() = default;

GLResourceMan::~GLResourceMan() = default;

void GLResourceMan::Clear() {
	for (auto prog: m_Shaders) {
		GL_CHECK(glDeleteProgram(prog));
	}
	for (auto& texture: m_StaticTextures) {
		glDeleteTextures(1, &texture->m_Texture);
	}
	for (auto& shaderData: m_ShaderCache) {
		glDeleteShader(shaderData.second.second);
	}
}

void GLResourceMan::Destroy() { Clear(); }
void GLResourceMan::Initialize() { Clear(); }

GLuint GLResourceMan::CompileShader(const std::string& filename, ShaderType type) {
	return 0;
}

GLuint GLResourceMan::MakeGLProgram() {
	return glCreateProgram();
}

// std::shared_ptr<Shader> GLResourceMan::MakeShaderProgram(const std::string& name, const std::string& vertexShaderPath, const std::string& fragmentShaderPath) {
// 	if (m_Shaders.find(name) != m_Shaders.end()) {
// 		RTEAbort("Attempted to reregister Shader: " + name);
// 	} else {
// 		m_Shaders[name] = std::make_shared<Shader>(vertexShaderPath, fragmentShaderPath);
// 		return m_Shaders[name];
// 	}
// 	return nullptr;
// }

GLBitmapInfo* GLResourceMan::GetBitmapInfo(BITMAP* bitmap) {
	return reinterpret_cast<GLBitmapInfo*>(bitmap->extra);
}

Texture2D GLResourceMan::GetStaticTextureFromFile(const std::string& filename) {
	BITMAP* bitmap = ContentFile(filename.c_str()).GetAsBitmap();
	return GetStaticTextureFromBitmap(bitmap);
}

GLBitmapInfo* GLResourceMan::MakeBitmapInfo() {
	m_StaticTextures.emplace_back(new GLBitmapInfo);
	m_StaticTextures.back()->m_ID = m_StaticTextures.size();
	return m_StaticTextures.back().get();
}

Texture2D GLResourceMan::GetStaticTextureFromBitmap(BITMAP* bitmap) {
	if (!bitmap->extra) {
		m_StaticTextures.emplace_back(new GLBitmapInfo);
		m_StaticTextures.back()->m_ID = m_StaticTextures.size();
		bitmap->extra = reinterpret_cast<void*>(m_StaticTextures.back().get());
		GL_CHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, bitmap_color_depth(bitmap) == 8 ? 1 : 4));
		m_StaticTextures.back()->m_Texture = rlLoadTexture(bitmap->line[0], bitmap->w, bitmap->h, bitmap_color_depth(bitmap) == 8 ? PIXELFORMAT_UNCOMPRESSED_GRAYSCALE : PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
		m_StaticTextureBytes += static_cast<uint64_t>(bitmap->w) * bitmap->h * (bitmap_color_depth(bitmap) == 8 ? 1 : 4);
		++m_CreatedSinceCensus[std::to_string(bitmap->w) + "x" + std::to_string(bitmap->h) + "x" + std::to_string(bitmap_color_depth(bitmap))];
		if (const size_t every = TextureCreatorEvery(); every && m_StaticTextures.size() % every == 0) PrintTextureCreator(m_StaticTextures.size(), bitmap);

		return {
		    .id = m_StaticTextures.back()->m_Texture,
		    .width = bitmap->w,
		    .height = bitmap->h,
		    .mipmaps = 0,
		    .format = bitmap_color_depth(bitmap) == 8 ? PIXELFORMAT_UNCOMPRESSED_GRAYSCALE : PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
	} else {
		return {
		    .id = GetBitmapInfo(bitmap)->m_Texture,
		    .width = bitmap->w,
		    .height = bitmap->h,
		    .mipmaps = 0,
		    .format = 0};
	}
	return {0, 0, 0, 0, -1};
}


GLuint GLResourceMan::GetDynamicUploadBuffer(BITMAP* bitmap) {
	if (!bitmap->extra) {
		GetStaticTextureFromBitmap(bitmap);
	}
	GLBitmapInfo* info = GetBitmapInfo(bitmap);

	if (!info->m_UpdateBuffer) {
		GLuint updateBuffer;
		glGenBuffers(1, &updateBuffer);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, updateBuffer);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, bitmap->w * bitmap->h * bitmap_color_depth(bitmap) / 8, nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		info->m_UpdateBuffer = updateBuffer;
	}
	return info->m_UpdateBuffer;
}

GLuint GLResourceMan::UpdateDynamicBitmap(BITMAP* bitmap, bool updated, const std::vector<Box>& updateRegions) {
	ZoneScopedN("Bitmap Upload");
	GLuint texture = GetStaticTextureFromBitmap(bitmap).id;
	if (updated) {
		glBindTexture(GL_TEXTURE_2D, texture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, bitmap_color_depth(bitmap) == 8 ? 1 : 4);
		if (updateRegions.size() == 0) {
			GL_CHECK(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bitmap->w, bitmap->h, bitmap_color_depth(bitmap) == 8 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, bitmap->line[0]));
		} else {
			int bytesPerPixel = bitmap_color_depth(bitmap) / 8;
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GetDynamicUploadBuffer(bitmap));
			std::vector<size_t> offsets = {0};
			for (size_t i = 0; i < updateRegions.size(); ++i) {
				std::vector<unsigned char> pixels(updateRegions[i].m_Width * updateRegions[i].m_Height * bytesPerPixel);
				for (size_t y = 0; y < updateRegions[i].m_Height; y++) {
					memcpy(
						pixels.data() + y * static_cast<int>(updateRegions[i].m_Width) * bytesPerPixel,
						bitmap->line[y + updateRegions[i].m_Corner.GetFloorIntY()] + updateRegions[i].m_Corner.GetFloorIntX(),
						updateRegions[i].m_Width * bytesPerPixel);
				}
				glBufferSubData(GL_PIXEL_UNPACK_BUFFER, offsets[i], updateRegions[i].m_Width * updateRegions[i].m_Height * bytesPerPixel, pixels.data());
				offsets.emplace_back(updateRegions[i].m_Width * updateRegions[i].m_Height * bytesPerPixel);
			}
			for (size_t i = 0; i < updateRegions.size(); ++i) {
				GL_CHECK(glTexSubImage2D(
					GL_TEXTURE_2D,
					0,
					updateRegions[i].m_Corner.GetFloorIntX(),
					updateRegions[i].m_Corner.GetFloorIntY(),
					updateRegions[i].m_Width,
					updateRegions[i].m_Height,
					bytesPerPixel == 1 ? GL_RED : GL_RGBA,
					GL_UNSIGNED_BYTE,
					(void*)offsets[i]
				));
			}
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		}
	}
	return texture;
}

std::string GLResourceMan::Census() {
	std::vector<std::pair<size_t, std::string>> sizes;
	for (const auto& [size, count]: m_CreatedSinceCensus) sizes.emplace_back(count, size);
	std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
	std::string created;
	for (size_t index = 0; index < std::min<size_t>(sizes.size(), 4); ++index) created += (index ? "," : "") + sizes[index].second + ":" + std::to_string(sizes[index].first);
	m_CreatedSinceCensus.clear();
	return "gl_bitmap_textures=" + std::to_string(m_StaticTextures.size()) + " gl_bitmap_texture_mb=" + std::to_string(m_StaticTextureBytes >> 20) +
	    " gl_bitmap_releases=" + std::to_string(m_TextureReleases) + " gl_bitmap_new_sizes=" + (created.empty() ? std::string("none") : created);
}

void GLResourceMan::DestroyBitmapInfo(BITMAP* bitmap) {
	GLBitmapInfo* info = GetBitmapInfo(bitmap);
	if (info) {
		rlUnloadTexture(info->m_Texture);
		++m_TextureReleases;
	}
}
