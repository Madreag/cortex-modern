#pragma once

#include <memory>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct BITMAP;

namespace RTE {

	class Entity;
	class MovableObject;
	class Scene;
	class GUIBitmap;
	class GUIControlManager;
	class GUIPanel;
	class GUISkin;
	class GUIFont;
	class GUIManager;

	/// Portable GUI state and references at a completed activity checkpoint.
	class GUICheckpoint {
		friend class FrameMan;
	public:
		static std::string SaveBitmap(const BITMAP* bitmap);
		static BITMAP* LoadBitmap(std::string_view text, bool validateOnly = false);
		static std::string SaveSharedBitmap(const BITMAP* bitmap);
		static std::shared_ptr<BITMAP> LoadSharedBitmap(std::string_view text, bool validateOnly = false);
		static std::vector<std::shared_ptr<BITMAP>> LoadSharedBitmapPool(const std::vector<std::string>& images, bool validateOnly = false);
		struct PreparedBitmapPool {
			std::vector<std::shared_ptr<BITMAP>> images;
			std::function<void()> commit;
		};
		static PreparedBitmapPool PrepareSharedBitmapPool(const std::vector<std::string>& images);
		static std::string SaveImage(const GUIBitmap* bitmap);
		static std::unique_ptr<GUIBitmap> LoadImage(std::string_view text, bool validateOnly = false);
		static std::string SaveOwnedEntity(const Entity* entity);
		static std::unique_ptr<Entity> LoadOwnedEntity(std::string_view text, bool validateOnly = false);
		static std::string SaveEntityReference(const Entity* entity, const Scene* scene = nullptr);
		/// A menu's non-owning object pointer, or nullptr once the object has left the world. Menus
		/// refresh these every Update, so one MovableMan no longer knows is already gone.
		static const MovableObject* LiveObject(const MovableObject* object);
		static const Entity* LoadEntityReference(std::string_view text, bool validateOnly = false, const Scene* scene = nullptr);
		static std::string Save(const GUIControlManager& manager);
		static bool Load(GUIControlManager& manager, std::string_view text, bool validateOnly = false);
		static bool Validate(std::string_view text);
		// Per-module menu flags. A module ID belongs to the installation that saved them, so they
		// travel keyed by module and come back sized to the modules this installation has.
		static std::map<std::string, bool> SaveModuleFlags(const std::vector<bool>& flags);
		static std::vector<bool> LoadModuleFlags(const std::map<std::string, bool>& saved);
		static std::vector<bool> LoadModuleFlags(const std::vector<bool>& savedByModuleID);
		static bool RunSelfTest();

	private:
		static thread_local int s_BitmapPoolAllocationFailureAfter;
		static std::map<std::string, GUIPanel*> Panels(const GUIControlManager& manager);
		static void PrepareOwnedPanels(GUIControlManager& manager, const std::unordered_set<std::string>& panelKeys);
		template <class Archive> static void VisitManagerFields(Archive& archive, GUIManager& manager);
		template <class Archive> static void VisitPanelFields(Archive& archive, GUIPanel& panel);
		template <class Archive> static void VisitPanelImages(Archive& archive, GUIPanel& panel);
		static std::string SavePanel(const GUIPanel& panel);
		static bool LoadPanel(GUIPanel& panel, std::string_view text, bool validateOnly, GUISkin* skin = nullptr, const std::unordered_set<std::string>* fontNames = nullptr, const std::unordered_set<std::string>* imagePaths = nullptr);
		static std::string SaveSkin(const GUISkin& skin);
		static bool LoadSkin(GUISkin& skin, std::string_view text, bool validateOnly);
		static std::string SaveFont(const GUIFont& font);
		static bool LoadFont(GUIFont& font, std::string_view text, bool validateOnly);
	};
}
