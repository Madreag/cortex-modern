#pragma once

#include "Singleton.h"

#include "glad/gl.h"
#include <unordered_map>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <optional>
#include "Shader.h"
#include "Box.h"

#include "raylib/raylib.h"
namespace RTE {
#define g_GLResourceMan GLResourceMan::Instance()
	enum class ShaderType {
		Fragment,
		Vertex
	};
	struct GLBitmapInfo {
		GLuint m_Texture{0};
		GLuint* m_Textures{nullptr};
		size_t m_ID{0};
		GLuint m_UpdateBuffer{0};
	};
	class GLResourceMan : public Singleton<GLResourceMan> {
	public:
		GLResourceMan();
		~GLResourceMan();

		void Initialize();
		void Destroy();
		void Clear();

		GLuint CompileShader(const std::string& filename, ShaderType type);
		GLuint MakeGLProgram();

		// std::shared_ptr<Shader> MakeShaderProgram(const std::string& name, const std::string& vertexShaderPath, const std::string& fragmentShaderPath);
		// std::shared_ptr<Shader> GetShaderByName(const std::string& name);
		Texture2D GetStaticTextureFromFile(const std::string& filename);
		Texture2D GetStaticTextureFromBitmap(BITMAP* bitmap);

		GLBitmapInfo* GetBitmapInfo(BITMAP* bitmap);
		GLuint GetDynamicUploadBuffer(BITMAP* bitmap);
		GLuint UpdateDynamicBitmap(BITMAP* bitmap, bool updated, const std::vector<Box>& = {});

		/// Releases the texture and upload buffer made for a bitmap, before the bitmap itself is destroyed or redrawn from new pixels.
		void DestroyBitmapInfo(BITMAP* bitmap);

		GLBitmapInfo* MakeBitmapInfo();

		/// The bitmaps that have been given a texture so far and their pixel bytes, for the memory census.
		std::string Census();


	private:
		std::vector<std::unique_ptr<GLBitmapInfo>> m_StaticTextures;
		uint64_t m_StaticTextureBytes = 0; //!< Pixel bytes of every texture made from a bitmap, released or not.
		uint64_t m_TextureReleases = 0; //!< Textures released through DestroyBitmapInfo.
		std::thread::id m_RenderThread; //!< The thread that makes the textures; their names are released there.
		std::mutex m_ReleaseMutex;
		std::vector<GLBitmapInfo*> m_PendingReleases; //!< Infos of bitmaps freed on another thread, released at the next draw.
		std::atomic<bool> m_ReleasesPending{false};

		void ReleaseBitmapInfo(GLBitmapInfo* info);
		void ReleasePendingBitmapInfos();
		std::map<std::string, size_t> m_CreatedSinceCensus; //!< New bitmap textures by size and depth since the last census line.
		std::vector<GLuint> m_DynamicBitmapUploadBuffers;
		std::unordered_map<std::string, std::pair<ShaderType, GLuint>> m_ShaderCache;
		std::vector<GLuint> m_Shaders;
	};

} // namespace RTE