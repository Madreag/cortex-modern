#include "GUI.h"
#include "GUICheckpoint.h"
#include "AllegroScreen.h"
#include <iostream>
#include "Scene.h"
#include "SceneMan.h"
#include "TerrainObject.h"
#include "GameActivity.h"
#include "GAScripted.h"
#include "BuyMenuGUI.h"
#include "InventoryMenuGUI.h"
#include "ObjectPickerGUI.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "GLResourceMan.h"
#include "SDL3/SDL.h"
#include "Reader.h"
#include "Writer.h"
#include <sstream>
#include "CheckpointArchive.h"
#include "AllegroBitmap.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "Actor.h"
#include "MOPixel.h"
#include "PresetMan.h"
#include "GUIButton.h"
#include "GUICollectionBox.h"
#include "GUILabel.h"
#include "GUIListBox.h"
#include "GUITextBox.h"
#include "GUIScrollbar.h"
#include "GUITab.h"
#include "GUIRadioButton.h"
#include "GUICheckbox.h"
#include "GUIComboBox.h"
#include "GUISlider.h"
#include "GUIProgressBar.h"
#include "GUIPropertyPage.h"
#include "GUIInputWrapper.h"
#include "GUIBanner.h"
#include <cstring>
#include <unordered_set>
#include "PresetMan.h"

#include <cassert>

using namespace RTE;

std::string GUICheckpoint::SaveBitmap(const BITMAP* bitmap) {
	CheckpointWriter writer("GUIBitmap1");
	writer(bitmap != nullptr);
	if (bitmap) {
		const int depth = bitmap_color_depth(const_cast<BITMAP*>(bitmap));
		const size_t stride = static_cast<size_t>(bitmap->w) * ((depth + 7) / 8);
		std::string pixels;
		pixels.reserve(stride * bitmap->h);
		for (int y = 0; y < bitmap->h; ++y) pixels.append(reinterpret_cast<const char*>(bitmap->line[y]), stride);
		writer(depth, bitmap->w, bitmap->h, bitmap->clip, bitmap->cl, bitmap->cr, bitmap->ct, bitmap->cb, pixels);
	}
	return writer.Text();
}

BITMAP* GUICheckpoint::LoadBitmap(std::string_view text, bool validateOnly) {
	CheckpointReader reader(text, "GUIBitmap1");
	bool present;
	reader.Value(present);
	if (!present) { reader.Finish(); return nullptr; }
	int depth, width, height, clip, left, right, top, bottom;
	std::string pixels;
	reader.Value(depth); reader.Value(width); reader.Value(height); reader.Value(clip);
	reader.Value(left); reader.Value(right); reader.Value(top); reader.Value(bottom); reader.Value(pixels);
	reader.Finish();
	if ((depth != 8 && depth != 15 && depth != 16 && depth != 24 && depth != 32) || width <= 0 || height <= 0 || width > 32768 || height > 32768)
		throw std::runtime_error("invalid GUI bitmap dimensions");
	const size_t stride = static_cast<size_t>(width) * ((depth + 7) / 8);
	if (pixels.size() != stride * height || left < 0 || left > right || right > width || top < 0 || top > bottom || bottom > height)
		throw std::runtime_error("invalid GUI bitmap payload");
	if (validateOnly) return nullptr;
	BITMAP* bitmap = create_bitmap_ex(depth, width, height);
	if (!bitmap) throw std::bad_alloc();
	for (int y = 0; y < height; ++y) std::memcpy(bitmap->line[y], pixels.data() + stride * y, stride);
	bitmap->clip = clip; bitmap->cl = left; bitmap->cr = right; bitmap->ct = top; bitmap->cb = bottom;
	return bitmap;
}

std::string GUICheckpoint::SaveImage(const GUIBitmap* bitmap) {
	CheckpointWriter writer("GUIImage2");
	writer(bitmap != nullptr);
	if (bitmap) {
		const auto* image = dynamic_cast<const AllegroBitmap*>(bitmap);
		if (!image) throw std::runtime_error("unsupported GUI bitmap checkpoint backend");
		writer(image->m_BitmapFile, SaveBitmap(bitmap->GetBitmap()));
	}
	return writer.Text();
}

std::unique_ptr<GUIBitmap> GUICheckpoint::LoadImage(std::string_view text, bool validateOnly) {
	const bool legacy = text.starts_with("9 GUIImage1 ");
	CheckpointReader reader(text, legacy ? "GUIImage1" : "GUIImage2");
	bool present;
	reader.Value(present);
	std::string file, pixels;
	if (present) {
		reader.Value(file); reader.Value(pixels); LoadBitmap(pixels, true);
		if (!legacy) { ContentFile validator; if (!validator.LoadCheckpoint(file, true)) throw std::runtime_error("invalid GUI image content checkpoint"); }
	}
	reader.Finish();
	if (!present || validateOnly) return nullptr;
	auto image = std::make_unique<AllegroBitmap>();
	if (legacy) { if (!file.empty()) image->m_BitmapFile.SetDataPath(file); }
	else if (!image->m_BitmapFile.LoadCheckpoint(file)) throw std::runtime_error("invalid GUI image content checkpoint");
	image->m_Bitmap = LoadBitmap(pixels);
	image->m_SelfCreated = true;
	return image;
}

std::string GUICheckpoint::SaveOwnedEntity(const Entity* entity) {
	CheckpointWriter checkpoint("GUIOwnedEntity1");
	checkpoint(entity != nullptr);
	if (entity) {
		auto stream = std::make_unique<std::stringstream>();
		auto* raw = stream.get();
		Writer writer(std::move(stream));
		Writer::SnapshotScope snapshot(writer);
		writer.NewProperty("GUIOwnedEntity");
		if (const auto* movable = dynamic_cast<const MovableObject*>(entity)) Scene::SaveSceneObject(writer, movable, false, true);
		else { entity->Save(writer); writer.ObjectEnd(); }
		checkpoint(entity->GetClassName(), raw->str());
	}
	return checkpoint.Text();
}

std::unique_ptr<Entity> GUICheckpoint::LoadOwnedEntity(std::string_view text, bool validateOnly) {
	CheckpointReader checkpoint(text, "GUIOwnedEntity1");
	bool present;
	std::string type, native;
	checkpoint.Value(present);
	if (present) { checkpoint.Value(type); checkpoint.Value(native); }
	checkpoint.Finish();
	if (!present) return {};
	Reader reader(std::make_unique<std::stringstream>(native), "Base.rte/GUIOwnedCheckpoint.ini", false, nullptr, true);
	reader.SetCheckpoint(true);
	reader.SetThrowOnError(true);
	reader.SetSkipIncludes(true);
	if (!reader.NextProperty() || reader.ReadPropName() != "GUIOwnedEntity") throw std::runtime_error("invalid GUI owned entity");
	if (validateOnly) {
		if (reader.ReadPropValue() != type) throw std::runtime_error("invalid GUI owned class");
		return {};
	}
	Entity::CheckpointCloneScope checkpointClones(true);
	std::unique_ptr<Entity> entity(g_PresetMan.ReadReflectedPreset(reader));
	if (!entity || entity->GetClassName() != type || reader.NextProperty()) throw std::runtime_error("invalid GUI owned entity body");
	if (auto* movable = dynamic_cast<MovableObject*>(entity.get())) {
		movable->AdoptPersistedUniqueID();
		if (auto* actor = dynamic_cast<Actor*>(movable)) actor->ApplyPersistedControllerMode();
		movable->ResolveFaithfulLinks();
	}
	return entity;
}

std::string GUICheckpoint::SaveEntityReference(const Entity* entity, const Scene* scene) {
	CheckpointWriter writer("GUIEntity2");
	writer(entity != nullptr);
	if (entity) {
		const auto* mo = dynamic_cast<const MovableObject*>(entity);
		const long uid = mo && !mo->IsOriginalPreset() ? mo->GetUniqueID() : 0;
		int kind = uid ? 1 : 0, placedSet = -1;
		size_t placedIndex = 0;
		if (!uid && !entity->IsOriginalPreset()) {
			if (!scene) scene = g_SceneMan.GetScene();
			if (scene) {
				for (int set = 0; set < Scene::PLACEDSETSCOUNT && placedSet < 0; ++set) {
					const auto* objects = scene->GetPlacedObjects(set);
					const auto found = std::find(objects->begin(), objects->end(), entity);
					if (found != objects->end()) { placedSet = set; placedIndex = std::distance(objects->begin(), found); kind = 2; }
				}
			}
			if (placedSet < 0) throw std::runtime_error("a GUI entity reference has no persistent owner: " + entity->GetClassName() + " \"" + entity->GetPresetName() + "\"");
		}
		writer(kind, uid, placedSet, placedIndex, entity->GetClassName(), entity->GetPresetName(), entity->GetModuleName());
	}
	return writer.Text();
}

const MovableObject* GUICheckpoint::LiveObject(const MovableObject* object) {
	// Only the pointer value is read, so this is safe on one whose object is already gone.
	return object && !g_MovableMan.IsKnownObject(object) ? nullptr : object;
}

std::string GUICheckpoint::SaveSharedBitmap(const BITMAP* bitmap) {
	std::string path;
	int cacheSlot = -1;
	if (bitmap) for (size_t slot = 0; slot < ContentFile::s_LoadedBitmaps.size(); ++slot) {
		for (const auto& [candidate, image]: ContentFile::s_LoadedBitmaps[slot]) if (image == bitmap && (cacheSlot < 0 || std::make_pair(static_cast<int>(slot), candidate) < std::make_pair(cacheSlot, path))) {
			cacheSlot = slot; path = candidate;
		}
	}
	CheckpointWriter writer("SharedBitmap1");
	writer(path, cacheSlot, SaveBitmap(bitmap));
	return writer.Text();
}

std::shared_ptr<BITMAP> GUICheckpoint::LoadSharedBitmap(std::string_view text, bool validateOnly) {
	CheckpointReader reader(text, "SharedBitmap1", true);
	std::string path, pixels;
	int cacheSlot;
	reader.Value(path); reader.Value(cacheSlot); reader.Value(pixels);
	LoadBitmap(pixels, true);
	if (path.empty() ? cacheSlot != -1 : cacheSlot < 0 || cacheSlot >= ContentFile::BitDepths::BitDepthCount) throw std::runtime_error("invalid shared bitmap cache reference");
	CheckpointReader image(pixels, "GUIBitmap1", true);
	bool present; image.Value(present);
	if (!present && cacheSlot >= 0) throw std::runtime_error("null cached bitmap owner");
	reader.Finish();
	if (validateOnly) return {};
	if (cacheSlot >= 0) {
		auto& cache = ContentFile::s_LoadedBitmaps[cacheSlot];
		const auto found = cache.find(path);
		if (found != cache.end()) {
			if (SaveBitmap(found->second) != pixels) throw std::runtime_error("shared bitmap cache pixels differ: " + path);
			return std::shared_ptr<BITMAP>(found->second, [](BITMAP*) {});
		}
		auto loaded = std::unique_ptr<BITMAP, void(*)(BITMAP*)>(LoadBitmap(pixels), destroy_bitmap);
		BITMAP* bitmap = loaded.get();
		cache.emplace(path, bitmap);
		loaded.release();
		return std::shared_ptr<BITMAP>(bitmap, [](BITMAP*) {});
	}
	return std::shared_ptr<BITMAP>(LoadBitmap(pixels), [](BITMAP* bitmap) {
		if (bitmap) { g_GLResourceMan.DestroyBitmapInfo(bitmap); destroy_bitmap(bitmap); }
	});
}

const Entity* GUICheckpoint::LoadEntityReference(std::string_view text, bool validateOnly, const Scene* scene) {
	const bool legacy = text.starts_with("10 GUIEntity1 ");
	CheckpointReader reader(text, legacy ? "GUIEntity1" : "GUIEntity2");
	bool present;
	reader.Value(present);
	long uid = 0;
	int kind = 0, placedSet = -1;
	size_t placedIndex = 0;
	std::string type, preset, module;
	if (present) {
		if (!legacy) reader.Value(kind);
		reader.Value(uid);
		if (legacy) kind = uid ? 1 : 0;
		else { reader.Value(placedSet); reader.Value(placedIndex); }
		reader.Value(type); reader.Value(preset); reader.Value(module);
		if (type.empty() || kind < 0 || kind > 2 || (kind == 1) != (uid > 0) ||
			(kind == 2 ? (placedSet < 0 || placedSet >= Scene::PLACEDSETSCOUNT) : (placedSet != -1 || placedIndex != 0)))
			throw std::runtime_error("invalid GUI entity owner");
	}
	reader.Finish();
	if (!present || validateOnly) return nullptr;
	const Entity* entity = nullptr;
	if (kind == 0) entity = g_PresetMan.GetEntityPreset(type, preset, module);
	else if (kind == 1) entity = g_MovableMan.FindObjectByUniqueID(uid);
	else {
		if (!scene) scene = g_SceneMan.GetScene();
		if (scene) {
			const auto* objects = scene->GetPlacedObjects(placedSet);
			if (placedIndex < objects->size()) entity = *std::next(objects->begin(), placedIndex);
		}
	}
	if (!entity || entity->GetClassName() != type) throw std::runtime_error("a GUI entity reference is missing");
	return entity;
}

namespace {
	template <class Archive> void GUIRectFields(Archive& archive, GUIRect& rect) { archive(rect.left, rect.top, rect.right, rect.bottom); }
	template <class Archive> void GUIImageField(Archive& archive, GUIBitmap*& image) {
		if constexpr (std::is_same_v<Archive, CheckpointWriter>) archive(GUICheckpoint::SaveImage(image));
		else {
			std::string saved;
			archive.Value(saved);
			GUICheckpoint::LoadImage(saved, true);
			archive.OnCommit([&image, saved = std::move(saved)] { auto replacement = GUICheckpoint::LoadImage(saved); delete image; image = replacement.release(); });
		}
	}
	template <class Archive> void GUIImageField(Archive& archive, std::unique_ptr<GUIBitmap>& image) {
		if constexpr (std::is_same_v<Archive, CheckpointWriter>) archive(GUICheckpoint::SaveImage(image.get()));
		else {
			std::string saved;
			archive.Value(saved);
			GUICheckpoint::LoadImage(saved, true);
			archive.OnCommit([&image, saved = std::move(saved)] { image = GUICheckpoint::LoadImage(saved); });
		}
	}
}

GUIControlManager::GUIControlManager() {
	m_Screen = nullptr;
	m_Input = nullptr;
	m_Skin = nullptr;
	m_GUIManager = nullptr;
	m_ControlList.clear();
	m_EventQueue.clear();

	m_CursorType = Pointer;
}

GUIControlManager::~GUIControlManager() {
	Destroy();
}

bool GUIControlManager::Create(GUIScreen* Screen, GUIInput* Input, const std::string& SkinDir, const std::string& SkinFilename) {
	assert(Screen && Input);

	m_Screen = Screen;
	m_Input = Input;

	// Create the skin
	m_Skin = new GUISkin(Screen);
	if (!m_Skin) {
		return false;
	}

	// Load the skin
	if (!m_Skin->Load(SkinDir, SkinFilename)) {
		delete m_Skin;
		m_Skin = 0;
		return false;
	}

	// Create the GUI manager
	m_GUIManager = new GUIManager(Input);
	if (!m_GUIManager) {
		return false;
	}
	return true;
}

void GUIControlManager::Destroy() {
	// Free the skin
	if (m_Skin) {
		m_Skin->Destroy();
		delete m_Skin;
		m_Skin = nullptr;
	}

	// Destroy the controls & event queue
	Clear();

	// Free the GUI manager
	if (m_GUIManager) {
		delete m_GUIManager;
		m_GUIManager = nullptr;
	}
}

void GUIControlManager::Clear() {
	std::vector<GUIControl*>::iterator it;

	// Destroy every control
	for (it = m_ControlList.begin(); it != m_ControlList.end(); it++) {
		GUIControl* C = *it;

		C->Destroy();
		delete C;
	}

	m_ControlList.clear();

	if (m_GUIManager) m_GUIManager->Clear();

	// Destroy the event queue
	std::vector<GUIEvent*>::iterator ite;
	for (ite = m_EventQueue.begin(); ite != m_EventQueue.end(); ite++) {
		GUIEvent* E = *ite;
		if (E) {
			delete E;
		}
	}
	m_EventQueue.clear();
}

void GUIControlManager::ChangeSkin(const std::string& SkinDir, const std::string& SkinFilename) {
	std::vector<GUIControl*>::iterator it;

	m_Skin->Destroy();
	m_Skin->Load(SkinDir, SkinFilename);

	// Go through every control and change its skin
	for (it = m_ControlList.begin(); it != m_ControlList.end(); it++) {
		GUIControl* C = *it;

		C->ChangeSkin(m_Skin);
	}
}

GUIControl* GUIControlManager::AddControl(const std::string& Name, const std::string& Type, GUIControl* Parent, int X, int Y, int Width, int Height) {
	// Skip if we already have a control of this name
	if (GetControl(Name)) {
		return nullptr;
	}

	// Create the control
	GUIControl* Control = GUIControlFactory::CreateControl(m_GUIManager, this, Type);
	if (!Control) {
		return nullptr;
	}

	Control->Create(Name, X, Y, Width, Height);
	Control->ChangeSkin(m_Skin);

	GUIPanel* Pan = nullptr;
	if (Parent) {
		Pan = Parent->GetPanel();
		Parent->AddChild(Control);
	}
	if (Pan) {
		Pan->AddChild(Control->GetPanel());
	} else {
		m_GUIManager->AddPanel(Control->GetPanel());
	}
	// Add the control to the list
	m_ControlList.push_back(Control);

	// Ready
	Control->Activate();

	return Control;
}

GUIControl* GUIControlManager::AddControl(GUIProperties* Property) {
	assert(Property);

	// Get the control type and name
	std::string Type;
	Property->GetValue("ControlType", &Type);
	std::string Name;
	Property->GetValue("Name", &Name);

	// Skip if we already have a control of this name
	if (GetControl(Name)) {
		return nullptr;
	}
	// Create the control
	GUIControl* Control = GUIControlFactory::CreateControl(m_GUIManager, this, Type);
	if (!Control) {
		return nullptr;
	}

	Control->Create(Property);
	Control->ChangeSkin(m_Skin);

	// Get the parent control
	std::string Parent;
	Property->GetValue("Parent", &Parent);

	GUIControl* Par = GetControl(Parent);
	GUIPanel* Pan = nullptr;
	if (Par && Parent.compare("None") != 0) {
		Pan = Par->GetPanel();
		Par->AddChild(Control);
	}

	if (Pan) {
		Pan->AddChild(Control->GetPanel());
	} else {
		m_GUIManager->AddPanel(Control->GetPanel());
	}

	// Add the control to the list
	m_ControlList.push_back(Control);

	// Ready
	Control->Activate();

	return Control;
}

GUIControl* GUIControlManager::GetControl(const std::string& Name) {
	std::vector<GUIControl*>::iterator it;

	for (it = m_ControlList.begin(); it != m_ControlList.end(); it++) {
		GUIControl* C = *it;
		if (C->GetName().compare(Name) == 0) {
			return C;
		}
	}

	// Not found
	return nullptr;
}

std::vector<GUIControl*>* GUIControlManager::GetControlList() {
	return &m_ControlList;
}

GUIControl* GUIControlManager::GetControlUnderPoint(int pointX, int pointY, GUIControl* pParent, int depth) {
	// Default to the root object if no parent specified
	if (!pParent) {
		pParent = m_ControlList.front();
	}
	if (!pParent) {
		return nullptr;
	}

	// Clicked on the parent?
	int X;
	int Y;
	int Width;
	int Height;
	pParent->GetControlRect(&X, &Y, &Width, &Height);

	if (pointX < X || pointX > X + Width) {
		return nullptr;
	}

	if (pointY < Y || pointY > Y + Height) {
		return nullptr;
	}

	// Check children
	std::vector<GUIControl*>* List = pParent->GetChildren();
	std::vector<GUIControl*>::reverse_iterator it;

	assert(List);

	// Control the depth. If negative, it'll go forever
	if (depth != 0) {
		for (it = List->rbegin(); it != List->rend(); it++) {
			// Only check visible controls
			if ((*it)->GetVisible()) {
				GUIControl* C = GetControlUnderPoint(pointX, pointY, *it, depth - 1);
				if (C) {
					return C;
				}
			}
		}
	}

	// If not asked to search for the root object, return the parent if point is on it
	return pParent == m_ControlList.front() ? nullptr : pParent;
}

void GUIControlManager::RemoveControl(const std::string& Name, bool RemoveFromParent) {
	// NOTE: We can't simply remove it because some controls need to remove extra panels and it's silly to add 'remove' to every control to remove their extra panels (ie. Combobox).
	// Signals and stuff are also linked in so we just remove the controls from the list and not from memory.
	std::vector<GUIControl*>::iterator it;

	for (it = m_ControlList.begin(); it != m_ControlList.end(); it++) {
		GUIControl* C = *it;
		if (C->GetName().compare(Name) == 0) {

			// Just remove it from the list
			C->SetVisible(false);
			m_ControlList.erase(it);

			// Remove all my children
			C->RemoveChildren();

			// Remove me from my parent
			if (C->GetParent() && RemoveFromParent) {
				C->GetParent()->RemoveChild(Name);
			}

			break;
		}
	}
}

void GUIControlManager::Update(bool ignoreKeyboardEvents) {
	// Clear the event queue
	m_EventQueue.clear();

	// Process the manager
	m_GUIManager->Update(ignoreKeyboardEvents);
}

void GUIControlManager::Draw() {
	m_GUIManager->Draw(m_Screen);
}

void GUIControlManager::Draw(GUIScreen* pScreen) {
	m_GUIManager->Draw(pScreen);
}

void GUIControlManager::DrawMouse(GUIScreen* guiScreen) {
	int MouseX;
	int MouseY;
	m_Input->GetMousePosition(&MouseX, &MouseY);
	switch (m_CursorType) {
		// Pointer
		case Pointer:
			m_Skin->DrawMouse(0, MouseX, MouseY, guiScreen);
			break;

			// Text
		case Text:
			m_Skin->DrawMouse(1, MouseX, MouseY, guiScreen);
			break;

			// Horizontal Resize
		case HorSize:
			m_Skin->DrawMouse(2, MouseX, MouseY, guiScreen);
			break;
		default:
			break;
	}
}

bool GUIControlManager::GetEvent(GUIEvent* Event) {
	if (Event && !m_EventQueue.empty()) {

		// Copy the event
		*Event = *m_EventQueue.back();

		// Free the event
		if (GUIEvent* ptr = m_EventQueue.at(m_EventQueue.size() - 1)) {
			delete ptr;
		}

		m_EventQueue.pop_back();
		return true;
	}
	// Empty queue OR null Event pointer
	return false;
}

void GUIControlManager::AddEvent(GUIEvent* Event) {
	// Add the event to the queue
	if (Event) {
		m_EventQueue.push_back(Event);
	}
}

void GUIControlManager::SetCursor(int CursorType) {
	m_CursorType = CursorType;
}

bool GUIControlManager::Save(const std::string& Filename) {
	GUIWriter W;
	if (W.Create(Filename) != 0) {
		return false;
	}
	bool Result = Save(&W);

	W.EndWrite();

	return Result;
}

bool GUIControlManager::Save(GUIWriter* W) {
	assert(W);

	// Go through each control
	std::vector<GUIControl*>::iterator it;

	for (it = m_ControlList.begin(); it != m_ControlList.end(); it++) {
		GUIControl* C = *it;
		C->Save(W);
		// Separate controls by one line
		W->NewLine();
	}

	return true;
}

bool GUIControlManager::Load(const std::string& Filename, bool keepOld) {
	GUIReader reader;
	const std::string pathFile = g_PresetMan.GetFullModulePath(Filename);
	if (reader.Create(pathFile) != 0) {
		return false;
	}

	// Clear the current layout, IF directed to
	if (!keepOld) {
		Clear();
	}

	std::vector<GUIProperties*> ControlList;
	ControlList.clear();

	GUIProperties* CurProp = nullptr;

	while (!reader.GetStream()->eof()) {
		std::string line = reader.ReadLine();

		if (line.empty()) {
			continue;
		}

		// Is the line a section?
		if (line.front() == '[' && line.back() == ']') {
			GUIProperties* p = new GUIProperties(line.substr(1, line.size() - 2));
			CurProp = p;
			ControlList.push_back(p);
			continue;
		}

		// Is the line a valid property?
		size_t Position = line.find_first_of('=');
		if (Position != std::string::npos) {
			// Break the line into variable & value, but only add a property if it belongs to a section
			if (CurProp) {
				// Grab the variable & value strings and trim them
				std::string Name = reader.TrimString(line.substr(0, Position));
				std::string Value = reader.TrimString(line.substr(Position + 1, std::string::npos));

				// Add it to the current property
				CurProp->AddVariable(Name, Value);
			}
			continue;
		}
	}

	// Go through each control item and create it
	std::vector<GUIProperties*>::iterator it;
	for (it = ControlList.begin(); it != ControlList.end(); it++) {
		GUIProperties* Prop = *it;
		AddControl(Prop);
		// Free the property class
		delete Prop;
	}

	return true;
}

template <class Archive> void GUICheckpoint::VisitPanelFields(Archive& archive, GUIPanel& panel) {
	archive(panel.m_X, panel.m_Y, panel.m_Width, panel.m_Height, panel.m_Visible, panel.m_Enabled, panel.m_GotFocus, panel.m_Captured,
		panel.m_FontColor, panel.m_FontShadow, panel.m_FontKerning, panel.m_ID, panel.m_ValidRegion, panel.m_ZPos);
	GUIRectFields(archive, panel.m_Rect);
	if (auto* value = dynamic_cast<GUIButton*>(&panel)) {
		archive(value->m_Pushed, value->m_Over);
	}
	if (auto* value = dynamic_cast<GUICollectionBox*>(&panel)) {
		archive(value->m_DrawBackground, value->m_DrawType, value->m_DrawColor);
	}
	if (auto* value = dynamic_cast<GUILabel*>(&panel)) {
		archive(value->m_Text, value->m_HAlignment, value->m_VAlignment, value->m_HorizontalOverflowScroll, value->m_VerticalOverflowScroll, value->m_OverflowScrollState, value->m_OverflowScrollTimer);
	}
	if (auto* value = dynamic_cast<GUIListPanel*>(&panel)) {
		archive(value->m_FontSelectColor, value->m_UpdateLocked, value->m_HorzScrollEnabled, value->m_VertScrollEnabled, value->m_ScrollBarThickness, value->m_ScrollBarPadding, value->m_CapturedHorz, value->m_CapturedVert, value->m_ExternalCapture, value->m_HighlightAsIfAlwaysFocused, value->m_LargestWidth, value->m_MultiSelect, value->m_HotTracking, value->m_LastSelected, value->m_LoopSelectionScroll, value->m_MouseScroll, value->m_AlternateDrawMode, value->m_SelectedColorIndex, value->m_UnselectedColorIndex);
	}
	if (auto* value = dynamic_cast<GUITextPanel*>(&panel)) {
		archive(value->m_FontSelectColor, value->m_Text, value->m_RightText, value->m_Focus, value->m_Locked, value->m_WidthMargin, value->m_HeightMargin, value->m_CursorX, value->m_CursorY, value->m_CursorIndex, value->m_CursorColor, value->m_BlinkTimer, value->m_StartIndex, value->m_GotSelection, value->m_StartSelection, value->m_EndSelection, value->m_SelectedColorIndex, value->m_SelectionX, value->m_SelectionWidth, value->m_MaxTextLength, value->m_NumericOnly, value->m_MaxNumericValue);
	}
	if (auto* value = dynamic_cast<GUITextBox*>(&panel)) {
		archive(value->m_HAlignment, value->m_VAlignment);
	}
	if (auto* value = dynamic_cast<GUIScrollPanel*>(&panel)) {
		archive(value->m_Orientation, value->m_Minimum, value->m_Maximum, value->m_Value, value->m_PageSize, value->m_SmallChange, value->m_RebuildSize, value->m_RebuildKnob, value->m_ButtonSize, value->m_MinimumKnobSize, value->m_KnobPosition, value->m_KnobLength, value->m_ButtonPushed, value->m_GrabbedKnob, value->m_GrabbedBackg, value->m_GrabbedPos, value->m_GrabbedSide, value->m_ValueResolution);
	}
	if (auto* value = dynamic_cast<GUITab*>(&panel)) {
		archive(value->m_Selected, value->m_Mouseover, value->m_Text);
		for (auto& rect: value->m_ImageRects) GUIRectFields(archive, rect);
	}
	if (auto* value = dynamic_cast<GUIRadioButton*>(&panel)) {
		archive(value->m_Checked, value->m_Mouseover, value->m_Text);
		for (auto& rect: value->m_ImageRects) GUIRectFields(archive, rect);
	}
	if (auto* value = dynamic_cast<GUICheckbox*>(&panel)) {
		archive(value->m_Check, value->m_Text, value->m_Mouseover);
		for (auto& rect: value->m_ImageRects) GUIRectFields(archive, rect);
	}
	if (auto* value = dynamic_cast<GUIComboBox*>(&panel)) {
		archive(value->m_OldSelection, value->m_CreatedList, value->m_DropHeight, value->m_DropDownStyle);
	}
	if (auto* value = dynamic_cast<GUIComboBoxButton*>(&panel)) {
		archive(value->m_Pushed);
	}
	if (auto* value = dynamic_cast<GUISlider*>(&panel)) {
		archive(value->m_Orientation, value->m_TickDirection, value->m_Minimum, value->m_Maximum, value->m_Value, value->m_ValueResolution, value->m_KnobPosition, value->m_KnobSize, value->m_KnobGrabbed, value->m_KnobGrabPos, value->m_EndThickness, value->m_OldValue);
	}
	if (auto* value = dynamic_cast<GUIProgressBar*>(&panel)) {
		archive(value->m_Minimum, value->m_Maximum, value->m_Value, value->m_Spacing);
	}
	if (auto* value = dynamic_cast<GUIPropertyPage*>(&panel)) {
		archive(value->m_LineColor);
	}
}

template <class Archive> void GUICheckpoint::VisitPanelImages(Archive& archive, GUIPanel& panel) {
	if (auto* value = dynamic_cast<GUIButton*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap);
		GUIImageField(archive, value->m_Icon);
	}
	if (auto* value = dynamic_cast<GUICollectionBox*>(&panel)) {
		GUIImageField(archive, value->m_Background);
		GUIImageField(archive, value->m_DrawBitmap);
	}
	if (auto* value = dynamic_cast<GUIListPanel*>(&panel)) {
		GUIImageField(archive, value->m_BaseBitmap);
		GUIImageField(archive, value->m_DrawBitmap);
		GUIImageField(archive, value->m_FrameBitmap);
	}
	if (auto* value = dynamic_cast<GUITextBox*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap);
	}
	if (auto* value = dynamic_cast<GUIScrollPanel*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap[0]);
		GUIImageField(archive, value->m_DrawBitmap[1]);
		GUIImageField(archive, value->m_DrawBitmap[2]);
	}
	if (auto* value = dynamic_cast<GUIComboBox*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap);
	}
	if (auto* value = dynamic_cast<GUIComboBoxButton*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap);
	}
	if (auto* value = dynamic_cast<GUISlider*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap);
		GUIImageField(archive, value->m_KnobImage);
	}
	if (auto* value = dynamic_cast<GUIProgressBar*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap);
		GUIImageField(archive, value->m_IndicatorImage);
	}
	if (auto* value = dynamic_cast<GUIPropertyPage*>(&panel)) {
		GUIImageField(archive, value->m_DrawBitmap);
	}
}

namespace {
	std::string GUIPanelType(const GUIPanel& panel) {
		if (const auto* control = dynamic_cast<const GUIControl*>(&panel)) return control->GetID();
		if (dynamic_cast<const GUIScrollPanel*>(&panel)) return "SCROLLPANEL";
		if (dynamic_cast<const GUITextPanel*>(&panel)) return "TEXTPANEL";
		if (dynamic_cast<const GUIListPanel*>(&panel)) return "LISTPANEL";
		if (dynamic_cast<const GUIComboBoxButton*>(&panel)) return "COMBOBOXBUTTON";
		throw std::runtime_error("unknown checkpoint GUI panel type");
	}

	struct GUIPanelDeleter {
		void operator()(GUIPanel* panel) const {
			if (auto* control = dynamic_cast<GUIControl*>(panel)) { control->Destroy(); delete control; }
			else if (auto* scroll = dynamic_cast<GUIScrollPanel*>(panel)) { scroll->Destroy(); delete scroll; }
			else if (auto* text = dynamic_cast<GUITextPanel*>(panel)) delete text;
			else if (auto* list = dynamic_cast<GUIListPanel*>(panel)) { list->Destroy(); delete list; }
			else if (auto* button = dynamic_cast<GUIComboBoxButton*>(panel)) { button->Destroy(); delete button; }
		}
	};

	std::unique_ptr<GUIPanel, GUIPanelDeleter> GUICheckpointPanel(const std::string& type, GUIManager& manager) {
		GUIPanel* panel = nullptr;
		if (type == "SCROLLPANEL") panel = new GUIScrollPanel(&manager);
		else if (type == "TEXTPANEL") panel = new GUITextPanel(&manager);
		else if (type == "LISTPANEL") panel = new GUIListPanel(&manager);
		else if (type == "COMBOBOXBUTTON") panel = new GUIComboBoxButton(&manager);
		else if (GUIControl* control = GUIControlFactory::CreateControl(&manager, nullptr, type)) panel = control->GetPanel();
		if (!panel) throw std::runtime_error("unknown checkpoint GUI panel type");
		return std::unique_ptr<GUIPanel, GUIPanelDeleter>(panel);
	}

	std::vector<std::pair<std::string, std::string>> GUIPropertyValues(GUIProperties& properties) {
		std::vector<std::pair<std::string, std::string>> values;
		for (int index = 0; index < properties.GetCount(); ++index) {
			std::string name, value;
			properties.GetVariable(index, &name, &value);
			values.emplace_back(std::move(name), std::move(value));
		}
		return values;
	}
}

std::string GUICheckpoint::SavePanel(const GUIPanel& source) {
	auto& panel = const_cast<GUIPanel&>(source);
	CheckpointWriter writer("GUIPanel2");
	writer(GUIPanelType(panel), panel.m_Font ? panel.m_Font->m_Name : std::string{}, dynamic_cast<GUIControl*>(&panel) ? dynamic_cast<GUIControl*>(&panel)->GetName() : std::string{});
	VisitPanelFields(writer, panel);
	VisitPanelImages(writer, panel);
	const GUIBitmap* sharedImage = nullptr;
	bool hasSharedImage = false;
	if (auto* value = dynamic_cast<GUITab*>(&panel)) { sharedImage = value->m_Image; hasSharedImage = true; }
	if (auto* value = dynamic_cast<GUIRadioButton*>(&panel)) { sharedImage = value->m_Image; hasSharedImage = true; }
	if (auto* value = dynamic_cast<GUICheckbox*>(&panel)) { sharedImage = value->m_Image; hasSharedImage = true; }
	if (hasSharedImage) writer(sharedImage != nullptr, sharedImage ? sharedImage->GetDataPath() : std::string{});
	if (auto* control = dynamic_cast<GUIControl*>(&panel)) {
		writer(control->m_SkinPreset, control->m_IsContainer, control->m_MinWidth, control->m_MinHeight, control->m_DefWidth, control->m_DefHeight,
			control->m_Properties.m_Name, GUIPropertyValues(control->m_Properties));
	}
	if (auto* button = dynamic_cast<GUIButton*>(&panel)) {
		writer(button->m_BorderSizes != nullptr);
		if (button->m_BorderSizes) GUIRectFields(writer, *button->m_BorderSizes);
	}
	if (auto* list = dynamic_cast<GUIListPanel*>(&panel)) {
		writer(list->m_Items.size());
		for (const auto* item: list->m_Items) {
			if (!item) throw std::runtime_error("null checkpoint list item");
			writer(item->m_ID, item->m_Name, item->m_RightText, item->m_ExtraIndex, item->m_Selected, item->m_Height, item->m_OffsetX,
				SaveImage(item->m_pBitmap), SaveEntityReference(item->m_pEntity));
		}
		std::vector<size_t> selected;
		for (const auto* item: list->m_SelectedList) {
			const auto found = std::find(list->m_Items.begin(), list->m_Items.end(), item);
			if (found == list->m_Items.end()) throw std::runtime_error("a selected GUI item has no list owner");
			selected.push_back(std::distance(list->m_Items.begin(), found));
		}
		writer(selected);
	}
	if (auto* page = dynamic_cast<GUIPropertyPage*>(&panel)) writer(page->m_PageValues.m_Name, GUIPropertyValues(page->m_PageValues));
	return writer.Text();
}

bool GUICheckpoint::LoadPanel(GUIPanel& panel, std::string_view text, bool validateOnly, GUISkin* skin, const std::unordered_set<std::string>* fontNames, const std::unordered_set<std::string>* imagePaths) {
	try {
		const bool legacy = text.starts_with("9 GUIPanel1 ");
		CheckpointReader reader(text, legacy ? "GUIPanel1" : "GUIPanel2", validateOnly);
		std::string type, font, controlName;
		reader.Value(type); reader.Value(font);
		if (!legacy) reader.Value(controlName);
		else if (auto* control = dynamic_cast<GUIControl*>(&panel)) controlName = control->GetName();
		if (fontNames && !font.empty() && !fontNames->contains(font)) return false;
		if (type != GUIPanelType(panel)) return false;
		GUIFont* fontReference = nullptr;
		if (skin && !font.empty()) {
			for (auto* candidate: skin->m_FontCache) if (candidate->m_Name == font) { fontReference = candidate; break; }
			if (!fontReference) return false;
		}
		reader.OnCommit([&panel, fontReference, skin] { panel.m_Font = fontReference; if (auto* list = dynamic_cast<GUIListPanel*>(&panel)) list->m_Skin = skin; if (auto* control = dynamic_cast<GUIControl*>(&panel)) control->m_Skin = skin; });
		VisitPanelFields(reader, panel);
		VisitPanelImages(reader, panel);
		GUIBitmap** sharedImage = nullptr;
		if (auto* value = dynamic_cast<GUITab*>(&panel)) sharedImage = &value->m_Image;
		if (auto* value = dynamic_cast<GUIRadioButton*>(&panel)) sharedImage = &value->m_Image;
		if (auto* value = dynamic_cast<GUICheckbox*>(&panel)) sharedImage = &value->m_Image;
		if (sharedImage) {
			bool present;
			std::string path;
			reader.Value(present); reader.Value(path);
			if (imagePaths && present && !imagePaths->contains(path)) return false;
			GUIBitmap* reference = nullptr;
			if (skin && present) {
				for (auto* candidate: skin->m_ImageCache) if (candidate->GetDataPath() == path) { reference = candidate; break; }
				if (!reference) return false;
			}
			reader.OnCommit([sharedImage, reference] { *sharedImage = reference; });
		}
		if (auto* control = dynamic_cast<GUIControl*>(&panel)) {
			reader(control->m_SkinPreset, control->m_IsContainer, control->m_MinWidth, control->m_MinHeight, control->m_DefWidth, control->m_DefHeight);
			std::string name;
			std::vector<std::pair<std::string, std::string>> values;
			reader.Value(name); reader.Value(values);
			GUIProperties properties(name);
			for (const auto& [key, value]: values) properties.AddVariable(key, value);
			std::string foundName;
			const bool named = properties.GetValue("Name", &foundName);
			if ((!controlName.empty() && !named) || (named && foundName != controlName)) throw std::runtime_error("GUI control property name differs from its checkpoint identity");
			reader.OnCommit([control, name = std::move(name), values = std::move(values)] {
				control->m_Properties.Clear();
				control->m_Properties.m_Name = name;
				for (const auto& [key, value]: values) control->m_Properties.AddVariable(key, value);
			});
		}
		if (auto* button = dynamic_cast<GUIButton*>(&panel)) {
			bool present;
			GUIRect rect{};
			reader.Value(present);
			if (present) { reader.Value(rect.left); reader.Value(rect.top); reader.Value(rect.right); reader.Value(rect.bottom); }
			reader.OnCommit([button, present, rect] { button->m_BorderSizes = present ? std::make_unique<GUIRect>(rect) : nullptr; });
		}
		if (auto* list = dynamic_cast<GUIListPanel*>(&panel)) {
			struct ItemState { int id, extra, height, offset; bool selected; std::string name, right, image, entity; };
			size_t count;
			reader.Value(count);
			if (count > text.size()) return false;
			auto items = std::make_shared<std::vector<ItemState>>(count);
			for (auto& item: *items) {
				reader.Value(item.id); reader.Value(item.name); reader.Value(item.right); reader.Value(item.extra); reader.Value(item.selected);
				reader.Value(item.height); reader.Value(item.offset); reader.Value(item.image); reader.Value(item.entity);
				LoadImage(item.image, true); LoadEntityReference(item.entity, true);
			}
			std::vector<size_t> selected;
			reader.Value(selected);
			std::unordered_set<size_t> unique;
			for (size_t index: selected) if (index >= count || !unique.insert(index).second) return false;
			reader.OnCommit([list, items, selected = std::move(selected)] {
				std::vector<std::unique_ptr<GUIListPanel::Item>> replacements;
				for (const auto& saved: *items) {
					auto item = std::make_unique<GUIListPanel::Item>();
					item->m_ID = saved.id; item->m_Name = saved.name; item->m_RightText = saved.right; item->m_ExtraIndex = saved.extra;
					item->m_Selected = saved.selected; item->m_Height = saved.height; item->m_OffsetX = saved.offset;
					item->m_pEntity = LoadEntityReference(saved.entity);
					item->m_pBitmap = LoadImage(saved.image).release();
					replacements.push_back(std::move(item));
				}
				for (auto* item: list->m_Items) delete item;
				list->m_Items.clear(); list->m_SelectedList.clear();
				for (auto& item: replacements) list->m_Items.push_back(item.release());
				for (size_t index: selected) list->m_SelectedList.push_back(list->m_Items[index]);
			});
		}
		if (auto* page = dynamic_cast<GUIPropertyPage*>(&panel)) {
			std::string name;
			std::vector<std::pair<std::string, std::string>> values;
			reader.Value(name); reader.Value(values);
			reader.OnCommit([page, name = std::move(name), values = std::move(values)] {
				page->m_PageValues.Clear(); page->m_PageValues.m_Name = name;
				for (const auto& [key, value]: values) page->m_PageValues.AddVariable(key, value);
			});
		}
		reader.Finish();
		return true;
	} catch (const std::exception& exception) { std::cout << "[gui-checkpoint] panel validation=" << validateOnly << " error=" << exception.what() << std::endl; return false; }
}

std::string GUICheckpoint::SaveFont(const GUIFont& font) {
	CheckpointWriter writer("GUIFont1");
	writer(font.m_Name, font.m_FontHeight, font.m_MainColor, font.m_CurrentColor, font.m_CharIndexCap, font.m_Kerning, font.m_Leading, SaveImage(font.m_Font));
	for (const auto& character: font.m_Characters) writer(character.m_Width, character.m_Height, character.m_Offset);
	writer(font.m_ColorCache.size());
	int current = font.m_CurrentBitmap == font.m_Font ? -1 : -2;
	for (size_t index = 0; index < font.m_ColorCache.size(); ++index) {
		const auto& color = font.m_ColorCache[index];
		writer(color.m_Color, SaveImage(color.m_Bitmap));
		if (font.m_CurrentBitmap == color.m_Bitmap) current = static_cast<int>(index);
	}
	if (current == -2 && font.m_CurrentBitmap) throw std::runtime_error("GUI font bitmap has no cache owner");
	writer(current);
	return writer.Text();
}

bool GUICheckpoint::LoadFont(GUIFont& font, std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "GUIFont1", validateOnly);
		reader(font.m_Name, font.m_FontHeight, font.m_MainColor, font.m_CurrentColor, font.m_CharIndexCap, font.m_Kerning, font.m_Leading);
		GUIImageField(reader, font.m_Font);
		for (auto& character: font.m_Characters) reader(character.m_Width, character.m_Height, character.m_Offset);
		size_t count;
		reader.Value(count);
		if (count > text.size()) return false;
		std::vector<std::pair<unsigned long, std::string>> colors(count);
		for (auto& [color, image]: colors) { reader.Value(color); reader.Value(image); LoadImage(image, true); }
		int current;
		reader.Value(current);
		if (current < -2 || current >= static_cast<int64_t>(count)) return false;
		reader.OnCommit([&font, colors = std::move(colors), current] {
			std::vector<std::unique_ptr<GUIBitmap>> images;
			for (const auto& [color, image]: colors) images.push_back(LoadImage(image));
			for (auto& color: font.m_ColorCache) delete color.m_Bitmap;
			font.m_ColorCache.clear();
			for (size_t index = 0; index < colors.size(); ++index) font.m_ColorCache.push_back({colors[index].first, images[index].release()});
			font.m_CurrentBitmap = current == -1 ? font.m_Font : current == -2 ? nullptr : font.m_ColorCache[current].m_Bitmap;
		});
		reader.Finish();
		return true;
	} catch (const std::exception& exception) { std::cout << "[gui-checkpoint] font validation=" << validateOnly << " error=" << exception.what() << std::endl; return false; }
}

std::string GUICheckpoint::SaveSkin(const GUISkin& skin) {
	CheckpointWriter writer("GUISkin1");
	writer(skin.m_Directory, skin.m_PropList.size());
	for (auto* properties: skin.m_PropList) writer(properties->m_Name, GUIPropertyValues(*properties));
	std::vector<std::string> images, fonts;
	for (const auto* image: skin.m_ImageCache) images.push_back(SaveImage(image));
	for (const auto* font: skin.m_FontCache) fonts.push_back(SaveFont(*font));
	writer(images, fonts);
	for (const auto* cursor: skin.m_MousePointers) {
		const auto found = std::find(skin.m_ImageCache.begin(), skin.m_ImageCache.end(), cursor);
		if (cursor && found == skin.m_ImageCache.end()) throw std::runtime_error("GUI cursor has no skin owner");
		writer(cursor ? static_cast<int>(std::distance(skin.m_ImageCache.begin(), found)) : -1);
	}
	return writer.Text();
}

bool GUICheckpoint::LoadSkin(GUISkin& skin, std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "GUISkin1", validateOnly);
		std::string directory;
		size_t count;
		reader.Value(directory); reader.Value(count);
		if (count > text.size()) return false;
		using Properties = std::pair<std::string, std::vector<std::pair<std::string, std::string>>>;
		std::vector<Properties> properties(count);
		for (auto& [name, values]: properties) { reader.Value(name); reader.Value(values); }
		std::vector<std::string> images, fonts;
		reader.Value(images); reader.Value(fonts);
		for (const auto& image: images) LoadImage(image, true);
		std::unordered_set<std::string> fontNames;
		for (const auto& saved: fonts) {
			GUIFont font("");
			if (!LoadFont(font, saved, true)) throw std::runtime_error("invalid GUI skin font");
			CheckpointReader nameReader(saved, "GUIFont1");
			std::string name; nameReader.Value(name);
			if (!fontNames.insert(name).second) return false;
		}
		std::array<int, 3> cursors;
		reader.Value(cursors);
		for (int cursor: cursors) if (cursor < -1 || cursor >= static_cast<int64_t>(images.size())) return false;
		reader.Finish();
		if (validateOnly) return true;
		auto candidate = std::unique_ptr<GUISkin, void(*)(GUISkin*)>(new GUISkin(skin.m_Screen), [](GUISkin* value) { value->Destroy(); delete value; });
		candidate->m_Directory = directory;
		for (const auto& [name, values]: properties) {
			auto record = std::make_unique<GUIProperties>(name);
			for (const auto& [key, value]: values) record->AddVariable(key, value);
			candidate->m_PropList.push_back(record.release());
		}
		for (const auto& image: images) candidate->m_ImageCache.push_back(LoadImage(image).release());
		for (const auto& saved: fonts) {
			auto font = std::unique_ptr<GUIFont, void(*)(GUIFont*)>(new GUIFont(""), [](GUIFont* value) { value->Destroy(); delete value; });
			font->m_Screen = skin.m_Screen;
			if (!LoadFont(*font, saved, false)) return false;
			candidate->m_FontCache.push_back(font.release());
		}
		for (size_t index = 0; index < cursors.size(); ++index) candidate->m_MousePointers[index] = cursors[index] < 0 ? nullptr : candidate->m_ImageCache[cursors[index]];
		std::swap(skin.m_Directory, candidate->m_Directory);
		std::swap(skin.m_PropList, candidate->m_PropList);
		std::swap(skin.m_ImageCache, candidate->m_ImageCache);
		std::swap(skin.m_FontCache, candidate->m_FontCache);
		std::swap(skin.m_MousePointers, candidate->m_MousePointers);
		return true;
	} catch (const std::exception& exception) { std::cout << "[gui-checkpoint] skin validation=" << validateOnly << " error=" << exception.what() << std::endl; return false; }
}

std::map<std::string, GUIPanel*> GUICheckpoint::Panels(const GUIControlManager& manager) {
	std::map<std::string, GUIPanel*> panels;
	std::unordered_set<GUIPanel*> visited;
	std::function<void(GUIPanel*, const std::string&)> add = [&](GUIPanel* panel, const std::string& key) {
		if (!panel || !visited.insert(panel).second) return;
		if (!panels.emplace(key, panel).second) throw std::runtime_error("duplicate GUI panel identity");
		if (auto* button = dynamic_cast<GUIButton*>(panel)) add(button->m_Text.get(), key + "/text");
		if (auto* list = dynamic_cast<GUIListPanel*>(panel)) { add(list->m_HorzScroll, key + "/horizontal"); add(list->m_VertScroll, key + "/vertical"); }
		if (auto* combo = dynamic_cast<GUIComboBox*>(panel)) {
			add(combo->m_TextPanel, key + "/text"); add(combo->m_ListPanel, key + "/list"); add(combo->m_Button, key + "/button");
		}
		if (auto* page = dynamic_cast<GUIPropertyPage*>(panel)) {
			add(page->m_VertScroll, key + "/vertical");
			for (size_t index = 0; index < page->m_TextPanelList.size(); ++index) add(page->m_TextPanelList[index], key + "/text" + std::to_string(index));
		}
	};
	for (auto* control: manager.m_ControlList) {
		const std::string name = control->GetName();
		add(control->GetPanel(), std::to_string(name.size()) + ":" + name);
	}
	for (const auto& [key, panel]: panels) {
		for (auto* child: panel->m_Children) if (!visited.contains(child)) throw std::runtime_error("a GUI child has no control owner");
	}
	return panels;
}

void GUICheckpoint::PrepareOwnedPanels(GUIControlManager& manager, const std::unordered_set<std::string>& panelKeys) {
	// Restore the recorded ownership graph without invoking Create or ChangeSkin,
	// which would rebuild controls from the receiving game's current skin.
	for (const auto& [key, panel]: Panels(manager)) { panel->m_Children.clear(); panel->m_Parent = nullptr; }
	const auto rawPanel = [&](auto*& panel, const std::string& key) {
		using Panel = std::remove_pointer_t<std::remove_reference_t<decltype(panel)>>;
		if (panelKeys.contains(key)) {
			if (!panel) panel = new Panel(manager.m_GUIManager);
		} else if (panel) { GUIPanelDeleter{}(panel); panel = nullptr; }
	};
	std::function<void(GUIPanel*, const std::string&)> prepare = [&](GUIPanel* panel, const std::string& key) {
		if (!panel) return;
		if (auto* button = dynamic_cast<GUIButton*>(panel)) {
			if (panelKeys.contains(key + "/text")) {
				if (!button->m_Text) button->m_Text = std::make_unique<GUILabel>(manager.m_GUIManager, &manager);
			} else button->m_Text.reset();
		}
		if (auto* list = dynamic_cast<GUIListPanel*>(panel)) {
			rawPanel(list->m_HorzScroll, key + "/horizontal"); rawPanel(list->m_VertScroll, key + "/vertical");
		}
		if (auto* combo = dynamic_cast<GUIComboBox*>(panel)) {
			rawPanel(combo->m_TextPanel, key + "/text"); rawPanel(combo->m_ListPanel, key + "/list"); rawPanel(combo->m_Button, key + "/button");
			prepare(combo->m_ListPanel, key + "/list");
		}
		if (auto* page = dynamic_cast<GUIPropertyPage*>(panel)) {
			rawPanel(page->m_VertScroll, key + "/vertical");
			size_t count = 0;
			while (panelKeys.contains(key + "/text" + std::to_string(count))) ++count;
			while (page->m_TextPanelList.size() > count) { delete page->m_TextPanelList.back(); page->m_TextPanelList.pop_back(); }
			while (page->m_TextPanelList.size() < count) page->m_TextPanelList.push_back(new GUITextPanel(manager.m_GUIManager));
		}
	};
	for (auto* control: manager.m_ControlList) {
		const std::string name = control->GetName();
		prepare(control->GetPanel(), std::to_string(name.size()) + ":" + name);
	}
}

template <class Archive> void GUICheckpoint::VisitManagerFields(Archive& archive, GUIManager& manager) {
	archive(manager.m_MouseEnabled, manager.m_OldMouseX, manager.m_OldMouseY, manager.m_DoubleClickTime, manager.m_DoubleClickSize,
		manager.m_DoubleClickButtons, manager.m_LastMouseDown, manager.m_HoverTrack, manager.m_HoverTime, manager.m_UseValidation, manager.m_UniqueIDCount, *manager.m_pTimer);
	GUIRectFields(archive, manager.m_DoubleClickRect);
}

std::string GUICheckpoint::Save(const GUIControlManager& manager) {
	CheckpointWriter writer("GUIControls1");
	writer(manager.m_GUIManager != nullptr);
	if (!manager.m_GUIManager) return writer.Text();
	const auto panels = Panels(manager);
	std::unordered_map<const GUIPanel*, std::string> keys;
	for (const auto& [key, panel]: panels) keys.emplace(panel, key);
	const auto keyOf = [&](const GUIPanel* panel) {
		if (!panel) return std::string{};
		const auto found = keys.find(panel);
		if (found == keys.end()) throw std::runtime_error("a GUI panel reference has no owner");
		return found->second;
	};
	CheckpointWriter managerValues("GUIManager1");
	VisitManagerFields(managerValues, *manager.m_GUIManager);
	writer(manager.m_CursorType, manager.m_Input->SaveCheckpoint(), SaveSkin(*manager.m_Skin), managerValues.Text(), manager.m_ControlList.size());
	for (auto* control: manager.m_ControlList) {
		std::vector<std::string> children;
		for (auto* child: control->m_ControlChildren) children.push_back(child->GetName());
		const auto* panel = control->GetPanel();
		writer(control->GetName(), control->GetID(), control->m_ControlParent ? control->m_ControlParent->GetName() : std::string{}, children,
			panel->m_X, panel->m_Y, panel->m_Width, panel->m_Height);
	}
	writer(panels.size());
	for (const auto& [key, panel]: panels) {
		std::vector<std::string> children;
		for (auto* child: panel->m_Children) children.push_back(keyOf(child));
		writer(key, keyOf(panel->m_Parent), keyOf(panel->m_SignalTarget), children, SavePanel(*panel));
	}
	std::vector<std::string> roots;
	for (auto* panel: manager.m_GUIManager->m_PanelList) roots.push_back(keyOf(panel));
	writer(roots, keyOf(manager.m_GUIManager->m_CapturedPanel), keyOf(manager.m_GUIManager->m_FocusPanel),
		keyOf(manager.m_GUIManager->m_MouseOverPanel), keyOf(manager.m_GUIManager->m_HoverPanel), manager.m_EventQueue.size());
	for (const auto* event: manager.m_EventQueue) writer(event->m_Control ? event->m_Control->GetName() : std::string{}, event->m_Type, event->m_Msg, event->m_Data);
	return writer.Text();
}

bool GUICheckpoint::Load(GUIControlManager& manager, std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "GUIControls1");
		bool present;
		reader.Value(present);
		if (!present) { reader.Finish(); return validateOnly || !manager.m_GUIManager; }
		int cursor;
		std::string input, skin, managerValues;
		reader.Value(cursor); reader.Value(input); reader.Value(skin); reader.Value(managerValues);
		GUIInputWrapper inputValidator(-1);
		GUISkin skinValidator(nullptr);
		GUIManager managerValidator(nullptr);
		if (!inputValidator.LoadCheckpoint(input, true)) throw std::runtime_error("invalid GUI input checkpoint");
		if (!LoadSkin(skinValidator, skin, true)) throw std::runtime_error("invalid GUI skin checkpoint");
		std::unordered_set<std::string> fontNames, imagePaths;
		{
			CheckpointReader skinReader(skin, "GUISkin1");
			std::string directory;
			size_t propertiesCount;
			skinReader.Value(directory); skinReader.Value(propertiesCount);
			for (size_t index = 0; index < propertiesCount; ++index) { std::string name; std::vector<std::pair<std::string, std::string>> values; skinReader.Value(name); skinReader.Value(values); }
			std::vector<std::string> images, fonts;
			skinReader.Value(images); skinReader.Value(fonts);
			for (const auto& image: images) {
				const bool legacyImage = image.starts_with("9 GUIImage1 ");
				CheckpointReader imageReader(image, legacyImage ? "GUIImage1" : "GUIImage2");
				bool present; std::string path; imageReader.Value(present);
				if (present) {
					imageReader.Value(path);
					if (!legacyImage) { CheckpointReader fileReader(path, "ContentFile1"); std::string dataPath; fileReader.Value(dataPath); path = std::move(dataPath); }
					imagePaths.insert(path);
				}
			}
			for (const auto& font: fonts) { CheckpointReader fontReader(font, "GUIFont1"); std::string name; fontReader.Value(name); fontNames.insert(name); }
		}
		CheckpointReader managerReader(managerValues, "GUIManager1", true);
		VisitManagerFields(managerReader, managerValidator);
		managerReader.Finish();
		struct Control { std::string name, type, parent; std::vector<std::string> children; int x, y, width, height; };
		struct Panel { std::string key, parent, signal; std::vector<std::string> children; std::string state; };
		struct Event { std::string control; int type, message, data; };
		size_t count;
		reader.Value(count);
		if (count > text.size()) return false;
		std::vector<Control> controls(count);
		std::unordered_set<std::string> controlNames;
		std::unordered_map<std::string, std::string> controlPanelNames;
		for (auto& control: controls) {
			reader.Value(control.name); reader.Value(control.type); reader.Value(control.parent); reader.Value(control.children);
			reader.Value(control.x); reader.Value(control.y); reader.Value(control.width); reader.Value(control.height);
			if (control.name.empty() || !controlNames.insert(control.name).second || control.width < 0 || control.height < 0) return false;
			controlPanelNames.emplace(std::to_string(control.name.size()) + ":" + control.name, control.name);
		}
		reader.Value(count);
		if (count > text.size()) return false;
		std::vector<Panel> panels(count);
		std::map<std::string, const Panel*> panelKeys;
		for (auto& panel: panels) {
			reader.Value(panel.key); reader.Value(panel.parent); reader.Value(panel.signal); reader.Value(panel.children); reader.Value(panel.state);
			if (panel.key.empty() || !panelKeys.emplace(panel.key, &panel).second) return false;
			CheckpointReader typeReader(panel.state, panel.state.starts_with("9 GUIPanel1 ") ? "GUIPanel1" : "GUIPanel2");
			std::string type; typeReader.Value(type);
			auto validator = GUICheckpointPanel(type, managerValidator);
			if (auto* control = dynamic_cast<GUIControl*>(validator.get())) {
				const auto expected = controlPanelNames.find(panel.key);
				if (expected != controlPanelNames.end()) control->m_Properties.AddVariable("Name", expected->second);
			}
			if (!LoadPanel(*validator, panel.state, true, nullptr, &fontNames, &imagePaths)) throw std::runtime_error("invalid GUI panel " + panel.key);
		}
		std::vector<std::string> roots;
		std::string captured, focus, over, hover;
		reader.Value(roots); reader.Value(captured); reader.Value(focus); reader.Value(over); reader.Value(hover); reader.Value(count);
		if (count > text.size()) return false;
		std::vector<Event> events(count);
		for (auto& event: events) { reader.Value(event.control); reader.Value(event.type); reader.Value(event.message); reader.Value(event.data); }
		reader.Finish();
		std::unordered_map<std::string, const Control*> controlByName;
		for (const auto& control: controls) controlByName.emplace(control.name, &control);
		std::unordered_set<std::string> controlChildren;
		for (const auto& control: controls) {
			if (!control.parent.empty() && !controlNames.contains(control.parent)) return false;
			std::unordered_set<std::string> seen;
			for (const auto& child: control.children) if (!controlNames.contains(child) || !seen.insert(child).second || controlByName.at(child)->parent != control.name || !controlChildren.insert(child).second) return false;
			const auto root = panelKeys.find(std::to_string(control.name.size()) + ":" + control.name);
			if (root == panelKeys.end()) return false;
			const bool legacyPanel = root->second->state.starts_with("9 GUIPanel1 ");
			CheckpointReader typeReader(root->second->state, legacyPanel ? "GUIPanel1" : "GUIPanel2");
			std::string type, font, name; typeReader.Value(type); typeReader.Value(font); if (!legacyPanel) typeReader.Value(name);
			if (type != control.type || (!legacyPanel && name != control.name)) return false;
		}
		for (const auto& control: controls) {
			std::unordered_set<std::string> seen;
			for (std::string name = control.name; !name.empty(); name = controlByName.at(name)->parent) if (!seen.insert(name).second) return false;
			if (!control.parent.empty() && !controlChildren.contains(control.name)) return false;
		}
		const auto knownPanel = [&](const std::string& key) { return key.empty() || panelKeys.contains(key); };
		std::unordered_map<std::string, std::string> parents;
		for (const auto& panel: panels) {
			if (!knownPanel(panel.parent) || !knownPanel(panel.signal)) return false;
			for (const auto& child: panel.children) {
				const auto found = panelKeys.find(child);
				if (found == panelKeys.end() || found->second->parent != panel.key || !parents.emplace(child, panel.key).second) return false;
			}
		}
		for (const auto& panel: panels) {
			std::unordered_set<std::string> seen;
			for (std::string key = panel.key; !key.empty(); key = panelKeys.at(key)->parent) if (!seen.insert(key).second) return false;
			if (!panel.parent.empty() && !parents.contains(panel.key)) return false;
		}
		std::unordered_set<std::string> rootKeys;
		for (const auto& root: roots) if (root.empty() || !knownPanel(root) || !rootKeys.insert(root).second || !panelKeys.at(root)->parent.empty()) return false;
		if (!knownPanel(captured) || !knownPanel(focus) || !knownPanel(over) || !knownPanel(hover)) return false;
		for (const auto& event: events) if (!event.control.empty() && !controlNames.contains(event.control)) return false;
		std::unordered_set<std::string> ownedPanelKeys;
		for (const auto& panel: panels) ownedPanelKeys.insert(panel.key);
		const auto createControl = [](GUIControlManager& target, const Control& saved) {
			auto* control = GUIControlFactory::CreateControl(target.m_GUIManager, &target, saved.type);
			if (!control) throw std::runtime_error("unknown checkpoint GUI control type");
			control->m_Properties.AddVariable("Name", saved.name);
			target.m_ControlList.push_back(control);
		};
		{
			GUIControlManager topologyValidator;
			topologyValidator.m_GUIManager = new GUIManager(nullptr);
			for (const auto& control: controls) createControl(topologyValidator, control);
			PrepareOwnedPanels(topologyValidator, ownedPanelKeys);
			const auto expected = Panels(topologyValidator);
			if (expected.size() != panels.size()) return false;
			for (const auto& panel: panels) {
				const auto found = expected.find(panel.key);
				if (found == expected.end() || !LoadPanel(*found->second, panel.state, true)) return false;
			}
		}
		if (validateOnly) return true;
		if (!manager.m_GUIManager || !manager.m_Skin || !manager.m_Input) return false;
		for (const auto& control: controls) if (auto* existing = manager.GetControl(control.name); existing && existing->GetID() != control.type) return false;
		for (const auto& control: controls) if (!manager.GetControl(control.name)) createControl(manager, control);
		PrepareOwnedPanels(manager, ownedPanelKeys);
		auto targets = Panels(manager);
		for (const auto& panel: panels) {
			const auto found = targets.find(panel.key);
			if (found == targets.end() || !LoadPanel(*found->second, panel.state, true)) throw std::runtime_error("invalid target GUI panel " + panel.key);
		}
		if (!LoadSkin(*manager.m_Skin, skin, false)) throw std::runtime_error("could not restore GUI skin");
		for (const auto& panel: panels) if (!LoadPanel(*targets.at(panel.key), panel.state, false, manager.m_Skin)) throw std::runtime_error("could not restore GUI panel " + panel.key);
		for (auto* control: manager.m_ControlList) {
			if (!controlNames.contains(control->GetName())) { control->Destroy(); delete control; }
		}
		std::map<std::string, GUIControl*> controlTargets;
		for (const auto& control: controls) controlTargets.emplace(control.name, dynamic_cast<GUIControl*>(targets.at(std::to_string(control.name.size()) + ":" + control.name)));
		manager.m_ControlList.clear();
		for (const auto& control: controls) {
			auto* target = controlTargets.at(control.name);
			manager.m_ControlList.push_back(target);
			target->m_ControlParent = control.parent.empty() ? nullptr : controlTargets.at(control.parent);
			target->m_ControlChildren.clear();
			for (const auto& child: control.children) target->m_ControlChildren.push_back(controlTargets.at(child));
		}
		const auto panelAt = [&](const std::string& key) { return key.empty() ? nullptr : targets.at(key); };
		for (const auto& panel: panels) {
			auto* target = targets.at(panel.key);
			target->m_Parent = panelAt(panel.parent); target->m_SignalTarget = panelAt(panel.signal);
			target->m_Children.clear();
			for (const auto& child: panel.children) target->m_Children.push_back(targets.at(child));
		}
		auto& gui = *manager.m_GUIManager;
		gui.m_PanelList.clear();
		for (const auto& root: roots) gui.m_PanelList.push_back(targets.at(root));
		gui.m_CapturedPanel = panelAt(captured); gui.m_FocusPanel = panelAt(focus); gui.m_MouseOverPanel = panelAt(over); gui.m_HoverPanel = panelAt(hover);
		for (auto* event: manager.m_EventQueue) delete event;
		manager.m_EventQueue.clear();
		for (const auto& event: events) manager.m_EventQueue.push_back(new GUIEvent(event.control.empty() ? nullptr : controlTargets.at(event.control), event.type, event.message, event.data));
		CheckpointReader stateReader(managerValues, "GUIManager1");
		VisitManagerFields(stateReader, gui); stateReader.Finish();
		manager.m_CursorType = cursor;
		return manager.m_Input->LoadCheckpoint(input);
	} catch (const std::exception& exception) { std::cout << "[gui-checkpoint] controls validation=" << validateOnly << " error=" << exception.what() << std::endl; return false; }
}

bool GUICheckpoint::Validate(std::string_view text) {
	GUIControlManager manager;
	return Load(manager, text, true);
}

std::map<std::string, bool> GUICheckpoint::SaveModuleFlags(const std::vector<bool>& flags) {
	std::map<std::string, bool> saved;
	for (int module = 0; module < g_PresetMan.GetTotalModuleCount() && module < static_cast<int>(flags.size()); ++module) {
		saved.emplace(g_PresetMan.GetDataModuleName(module), flags[module]);
	}
	return saved;
}

std::vector<bool> GUICheckpoint::LoadModuleFlags(const std::map<std::string, bool>& saved) {
	std::vector<bool> flags(g_PresetMan.GetTotalModuleCount());
	for (int module = 0; module < static_cast<int>(flags.size()); ++module) {
		const auto found = saved.find(g_PresetMan.GetDataModuleName(module));
		// A module the save did not know gets what a fresh menu would give it.
		flags[module] = found != saved.end() ? found->second : module == 0;
	}
	return flags;
}

std::vector<bool> GUICheckpoint::LoadModuleFlags(const std::vector<bool>& savedByModuleID) {
	std::vector<bool> flags(g_PresetMan.GetTotalModuleCount());
	for (int module = 0; module < static_cast<int>(flags.size()); ++module) {
		flags[module] = module < static_cast<int>(savedByModuleID.size()) ? savedByModuleID[module] : module == 0;
	}
	return flags;
}

std::string GUIControlManager::SaveCheckpoint() const { return GUICheckpoint::Save(*this); }
bool GUIControlManager::LoadCheckpoint(std::string_view text, bool validateOnly) { return GUICheckpoint::Load(*this, text, validateOnly); }

bool GUICheckpoint::RunSelfTest() {
	bool passed = true;
	int checked = 0;
	const auto check = [&](const char* label, bool value) {
		++checked; passed = passed && value;
		std::cout << "[gui-checkpoint-selftest] " << (value ? "PASS " : "FAIL ") << label << std::endl;
	};
	const std::string oldSharedInput = GUIInput::SaveSharedCheckpoint();
	try {
		std::unique_ptr<BITMAP, void(*)(BITMAP*)> bitmap(create_bitmap_ex(8, 640, 480), destroy_bitmap);
		std::unique_ptr<BITMAP, void(*)(BITMAP*)> icon(create_bitmap_ex(8, 16, 16), destroy_bitmap);
		clear_to_color(bitmap.get(), 0); clear_to_color(icon.get(), 37);
		AllegroScreen screen(bitmap.get());
		GUIInputWrapper input(0);
		GUIControlManager controls;
		if (!controls.Create(&screen, &input, "Base.rte/GUIs/Skins", "DefaultSkin.ini")) throw std::runtime_error("the fixture skin is unavailable");
		// Property pages accept mod-supplied skin sections; the stock skin does not define one.
		auto pageSkin = std::make_unique<GUIProperties>("PropertyPage");
		for (auto* section: controls.m_Skin->m_PropList) if (section->GetName() == "Listbox") pageSkin->Update(section, true);
		pageSkin->AddVariable("FontShadow", 0); pageSkin->AddVariable("LineColor", 12);
		controls.m_Skin->m_PropList.push_back(pageSkin.release());
		auto* root = dynamic_cast<GUICollectionBox*>(controls.AddControl("root", "COLLECTIONBOX", nullptr, 3, 4, 620, 460));
		auto* button = dynamic_cast<GUIButton*>(controls.AddControl("button", "BUTTON", root, 12, 13, 110, 32));
		auto* list = dynamic_cast<GUIListBox*>(controls.AddControl("list", "LISTBOX", root, 12, 58, 210, 95));
		auto* text = dynamic_cast<GUITextBox*>(controls.AddControl("text", "TEXTBOX", root, 12, 170, 200, 27));
		auto* checkbox = dynamic_cast<GUICheckbox*>(controls.AddControl("check", "CHECKBOX", root, 245, 15, 135, 25));
		auto* radio = dynamic_cast<GUIRadioButton*>(controls.AddControl("radio", "RADIOBUTTON", root, 245, 50, 135, 25));
		auto* tab = dynamic_cast<GUITab*>(controls.AddControl("tab", "TAB", root, 245, 85, 135, 25));
		auto* slider = dynamic_cast<GUISlider*>(controls.AddControl("slider", "SLIDER", root, 245, 125, 135, 25));
		auto* progress = dynamic_cast<GUIProgressBar*>(controls.AddControl("progress", "PROGRESSBAR", root, 245, 170, 135, 25));
		auto* combo = dynamic_cast<GUIComboBox*>(controls.AddControl("combo", "COMBOBOX", root, 12, 215, 200, 25));
		auto* page = dynamic_cast<GUIPropertyPage*>(controls.AddControl("page", "PROPERTYPAGE", root, 395, 15, 160, 160));
		if (!root || !button || !list || !text || !checkbox || !radio || !tab || !slider || !progress || !combo || !page) throw std::runtime_error("a GUI fixture control is unavailable");
		button->SetIconAndText(icon.get(), "checkpoint button");
		button->OnMouseEnter(button->GetXPos() + 1, button->GetYPos() + 1, 0, 0);
		button->OnMouseDown(button->GetXPos() + 1, button->GetYPos() + 1, GUIPanel::MOUSE_LEFT, 0);
		list->AddItem("first", "12 oz", new AllegroBitmap(icon.get()), nullptr, 27, 3);
		list->AddItem("second", "19 oz", new AllegroBitmap(icon.get()), nullptr, 39, 7);
		list->SetSelectedIndex(1);
		text->SetText("abcdef 012345"); text->SetSelection(2, 5); text->SetCursorPos(7);
		checkbox->SetCheck(1); radio->SetCheck(true); tab->SetCheck(true);
		slider->SetValue(37); progress->SetValue(61);
		GUIProperties values("page-values"); values.AddVariable("Rate", "1.375"); values.AddVariable("Mode", "second"); page->SetPropertyValues(&values);
		input.SetMouseOffset(13, -19);
		controls.Draw();
		const std::string pixels = SaveBitmap(bitmap.get());
		const std::string checkpoint = controls.SaveCheckpoint();
		const int cursor = text->m_CursorIndex, selectionStart = text->m_StartSelection, selectionEnd = text->m_EndSelection;
		const int sliderValue = slider->GetValue(), progressValue = progress->GetValue();
		std::vector<std::array<int, 3>> events;
		for (const auto* event: controls.m_EventQueue) events.push_back({event->m_Type, event->m_Msg, event->m_Data});
		check("fixture_has_event_and_selection", !events.empty() && list->GetSelectedIndex() == 1 && button->IsPushed());
		button->SetText("advanced"); button->SetPushed(false); list->ClearList(); list->AddItem("changed");
		text->SetText("changed"); checkbox->SetCheck(0); radio->SetCheck(false); tab->SetCheck(false); slider->SetValue(0); progress->SetValue(0); input.SetMouseOffset(0, 0);
		check("restore_live_controls", controls.LoadCheckpoint(checkpoint));
		check("control_pointer_identity", controls.GetControl("button") == button && controls.GetControl("list") == list);
		check("button_state", button->GetText() == "checkpoint button" && button->IsPushed() && button->IsMousedOver());
		check("list_content", list->GetItemList()->size() == 2 && list->GetItem(0)->m_Name == "first" && list->GetItem(1)->m_RightText == "19 oz" && list->GetItem(1)->m_ExtraIndex == 39 && list->GetItem(1)->m_OffsetX == 7);
		check("selected_item_identity", list->GetSelected() == list->GetItem(1));
		check("text_cursor_and_selection", text->GetText() == "abcdef 012345" && text->m_CursorIndex == cursor && text->m_StartSelection == selectionStart && text->m_EndSelection == selectionEnd);
		check("selection_controls", checkbox->GetCheck() == 1 && radio->GetCheck() && tab->GetCheck());
		check("range_controls", slider->GetValue() == sliderValue && progress->GetValue() == progressValue);
		int mouseX, mouseY; input.GetMouseOffset(mouseX, mouseY); check("input_offset", mouseX == 13 && mouseY == -19);
		bool sameEvents = events.size() == controls.m_EventQueue.size();
		for (size_t index = 0; sameEvents && index < events.size(); ++index) { const auto* event = controls.m_EventQueue[index]; sameEvents = events[index] == std::array<int, 3>{event->m_Type, event->m_Msg, event->m_Data}; }
		check("queued_events", sameEvents);
		check("canonical_checkpoint", controls.SaveCheckpoint() == checkpoint);
		clear_to_color(bitmap.get(), 0); controls.Draw(); check("rendered_pixels", SaveBitmap(bitmap.get()) == pixels);
		const std::string steady = controls.SaveCheckpoint();
		{
			GUIInputWrapper freshInput(0);
			GUIControlManager fresh;
			if (!fresh.Create(&screen, &freshInput, "Base.rte/GUIs/Skins", "DefaultSkin.ini")) throw std::runtime_error("fresh fixture skin unavailable");
			check("fresh_custom_skin_and_controls", fresh.LoadCheckpoint(checkpoint) && fresh.m_ControlList.size() == 11);
			check("fresh_canonical_checkpoint", fresh.SaveCheckpoint() == checkpoint);
			clear_to_color(bitmap.get(), 0); fresh.Draw(); check("fresh_rendered_pixels", SaveBitmap(bitmap.get()) == pixels);
			auto* freshButton = dynamic_cast<GUIButton*>(fresh.GetControl("button"));
			freshButton->OnMouseUp(freshButton->GetXPos() + 1, freshButton->GetYPos() + 1, GUIPanel::MOUSE_LEFT, 0);
			check("fresh_button_click", !freshButton->IsPushed() && std::any_of(fresh.m_EventQueue.begin(), fresh.m_EventQueue.end(), [&](auto* event) { return event->m_Control == freshButton && event->m_Msg == GUIButton::Clicked; }));
		}
		{
			auto* savedFont = button->m_Font;
			GUIFont missingFont("checkpoint-font-not-in-skin");
			button->m_Font = &missingFont;
			const std::string missingFontState = controls.SaveCheckpoint();
			button->m_Font = savedFont;
			check("missing_font_atomic", !controls.LoadCheckpoint(missingFontState) && controls.SaveCheckpoint() == steady);
			auto* savedImage = checkbox->m_Image;
			AllegroBitmap missingImage;
			missingImage.m_BitmapFile.SetDataPath("Base.rte/GUIs/Fonts/BannerFontYellowReg.png");
			checkbox->m_Image = &missingImage;
			const std::string missingImageState = controls.SaveCheckpoint();
			checkbox->m_Image = savedImage;
			check("missing_skin_image_atomic", !controls.LoadCheckpoint(missingImageState) && controls.SaveCheckpoint() == steady);
			std::string missingPanel = steady;
			for (size_t offset = 0; (offset = missingPanel.find("4:page/text0", offset)) != std::string::npos; offset += 12) missingPanel.replace(offset, 12, "4:page/textx");
			check("missing_owned_panel_atomic", !controls.LoadCheckpoint(missingPanel) && controls.SaveCheckpoint() == steady);
			auto* removed = page->m_TextPanelList.back();
			page->m_Children.erase(std::remove(page->m_Children.begin(), page->m_Children.end(), removed), page->m_Children.end());
			page->m_TextPanelList.pop_back(); delete removed;
			const std::string fewerPanels = controls.SaveCheckpoint();
			const size_t fewerCount = page->m_TextPanelList.size();
			check("owned_panel_growth", controls.LoadCheckpoint(steady) && page->m_TextPanelList.size() == fewerCount + 1 && controls.SaveCheckpoint() == steady);
			check("owned_panel_shrink", controls.LoadCheckpoint(fewerPanels) && page->m_TextPanelList.size() == fewerCount && controls.SaveCheckpoint() == fewerPanels);
			if (!controls.LoadCheckpoint(steady)) throw std::runtime_error("fixture topology restore failed");
		}
		check("truncated_checkpoint_atomic", !controls.LoadCheckpoint(steady.substr(0, steady.size() - 1)) && controls.SaveCheckpoint() == steady);
		check("trailing_checkpoint_atomic", !controls.LoadCheckpoint(steady + "invalid") && controls.SaveCheckpoint() == steady);
		button->OnMouseUp(button->GetXPos() + 1, button->GetYPos() + 1, GUIPanel::MOUSE_LEFT, 0);
		bool clicked = false;
		for (const auto* event: controls.m_EventQueue) clicked = clicked || (event->m_Control == button && event->m_Msg == GUIButton::Clicked);
		check("restored_button_click", clicked && !button->IsPushed());
		GUIInput::SetNetworkMouseMovement(0, 91, -72); GUIInput::SetNetworkMouseButton(0, GUIInput::Down, GUIInput::Up, GUIInput::Down);
		int expectedX, expectedY, expectedEvents[3], expectedStates[3]; input.GetMousePosition(&expectedX, &expectedY); input.GetMouseButtons(expectedEvents, expectedStates);
		const std::string shared = GUIInput::SaveSharedCheckpoint();
		GUIInput::SetNetworkMouseMovement(0, 200, 300); GUIInput::SetNetworkMouseButton(0, GUIInput::Up, GUIInput::Down, GUIInput::Up);
		check("shared_input_restore", GUIInput::LoadSharedCheckpoint(shared));
		int actualX, actualY, actualEvents[3], actualStates[3]; input.GetMousePosition(&actualX, &actualY); input.GetMouseButtons(actualEvents, actualStates);
		check("shared_input_observation", actualX == expectedX && actualY == expectedY && std::equal(std::begin(expectedStates), std::end(expectedStates), std::begin(actualStates)) && std::equal(std::begin(expectedEvents), std::end(expectedEvents), std::begin(actualEvents)));
		check("shared_input_atomic", !GUIInput::LoadSharedCheckpoint(shared + "bad") && GUIInput::SaveSharedCheckpoint() == shared);
		if (auto* window = g_WindowMan.GetWindow()) {
			const SDL_Rect area{31, 47, 151, 23};
			if (!SDL_SetTextInputArea(window, &area, 19) || !SDL_StartTextInput(window)) throw std::runtime_error("text input fixture unavailable");
			const std::string activeText = GUIInput::SaveSharedCheckpoint();
			SDL_StopTextInput(window); SDL_SetTextInputArea(window, nullptr, 0);
			check("shared_text_input_activation", GUIInput::LoadSharedCheckpoint(activeText) && SDL_TextInputActive(window));
			SDL_Rect restored{}; int cursorOffset = 0;
			check("shared_text_input_area", SDL_GetTextInputArea(window, &restored, &cursorOffset) && restored.x == 31 && restored.y == 47 && restored.w == 151 && restored.h == 23 && cursorOffset == 19);
			check("shared_text_input_canonical", GUIInput::SaveSharedCheckpoint() == activeText);
			if (!GUIInput::LoadSharedCheckpoint(shared)) throw std::runtime_error("text input fixture cleanup failed");
		}
		GUIBanner banner;
		banner.Create("Base.rte/GUIs/Fonts/BannerFontYellowReg.png", "Base.rte/GUIs/Fonts/BannerFontYellowBlur.png", 8);
		banner.SetKerning(3); banner.ShowText("CHECKPOINT", GUIBanner::BLINKING, -1, Vector(640, 480), 0.5F); banner.Update();
		const std::string bannerState = banner.SaveCheckpoint();
		clear_to_color(bitmap.get(), 0); banner.Draw(bitmap.get()); const std::string bannerPixels = SaveBitmap(bitmap.get());
		banner.Create("Base.rte/GUIs/Fonts/BannerFontRedReg.png", "Base.rte/GUIs/Fonts/BannerFontRedBlur.png", 8);
		banner.SetKerning(11); banner.ClearText();
		check("banner_restore_and_font_pixels", banner.LoadCheckpoint(bannerState) && banner.SaveCheckpoint() == bannerState && banner.GetBannerText() == "CHECKPOINT" && banner.GetKerning() == 3);
		clear_to_color(bitmap.get(), 0); banner.Draw(bitmap.get()); check("banner_rendered_pixels", SaveBitmap(bitmap.get()) == bannerPixels);
		GUIBanner bareBanner;
		check("banner_bare_restore", bareBanner.LoadCheckpoint(bannerState) && bareBanner.SaveCheckpoint() == bannerState);
		clear_to_color(bitmap.get(), 0); bareBanner.Draw(bitmap.get()); check("banner_bare_rendered_pixels", SaveBitmap(bitmap.get()) == bannerPixels);
		check("banner_truncated_atomic", !banner.LoadCheckpoint(bannerState.substr(0, bannerState.size() - 1)) && banner.SaveCheckpoint() == bannerState);
		{
			struct RestoreUIDCounter { long value = MovableObject::GetUniqueIDCounter(); ~RestoreUIDCounter() { MovableObject::PinUniqueIDCounter(value); } } counter;
			Actor inventoryActor;
			MOPixel inventoryItem;
			if (inventoryActor.MovableObject::Create() < 0 || inventoryItem.MovableObject::Create() < 0) throw std::runtime_error("inventory reference fixture unavailable");
			Controller controller(Controller::CIM_DISABLED, 0);
			InventoryMenuGUI inventory;
			if (inventory.Create(&controller) < 0) throw std::runtime_error("inventory carousel fixture unavailable");
			check("inventory_initial_pixels", getpixel(inventory.m_CarouselBitmap.get(), 0, 0) == g_MaskColor && getpixel(inventory.m_CarouselBGBitmap.get(), 0, 0) == g_MaskColor);
			inventory.m_InventoryActor = &inventoryActor;
			inventory.m_InventoryActorEquippedItems = {{&inventoryItem, &inventoryItem}};
			inventory.m_CarouselItemBoxes[0]->Item = &inventoryItem;
			inventory.m_CarouselItemBoxes[0]->CurrentSize.SetXY(31.25F, 19.75F);
			inventory.m_CarouselItemBoxes[0]->Pos.SetXY(7.5F, 12.25F);
			inventory.m_CarouselExitingItemBox->Item = &inventoryItem;
			inventory.m_CarouselExitingItemBox->RoundedAndBorderedSides = {true, false};
			inventory.m_CenterPos.SetXY(325.25F, 247.75F);
			inventory.m_CarouselAnimationTimer.SetStartSimTimeTicks(-734000);
			clear_to_color(inventory.m_CarouselBitmap.get(), 17); clear_to_color(inventory.m_CarouselBGBitmap.get(), 29);
			const std::string carousel = inventory.SaveCheckpoint();
			if (inventory.Create(&controller, nullptr, InventoryMenuGUI::MenuMode::Full) < 0) throw std::runtime_error("inventory full fixture unavailable");
			check("inventory_later_full_mode_created", inventory.m_GUIControlManager != nullptr);
			check("inventory_remove_later_full_mode", inventory.LoadCheckpoint(carousel) && !inventory.m_GUIControlManager && !inventory.m_GUIInput && !inventory.m_GUIScreen);
			check("inventory_carousel_canonical", inventory.SaveCheckpoint() == carousel);
			check("inventory_carousel_aliases", inventory.GetInventoryActor() == &inventoryActor && inventory.m_CarouselItemBoxes[0]->Item == &inventoryItem && inventory.m_CarouselExitingItemBox->Item == &inventoryItem && inventory.m_InventoryActorEquippedItems[0] == std::pair<MovableObject*, MovableObject*>(&inventoryItem, &inventoryItem));
			check("inventory_carousel_pixels", getpixel(inventory.m_CarouselBitmap.get(), 0, 0) == 17 && getpixel(inventory.m_CarouselBGBitmap.get(), 0, 0) == 29);
			InventoryMenuGUI bareInventory;
			check("inventory_pending_checkpoint", bareInventory.LoadCheckpoint(carousel) && bareInventory.HasPendingCheckpoint() && bareInventory.SaveCheckpoint() == carousel);
			check("inventory_pending_carousel_apply", bareInventory.Create(&controller) == 0 && bareInventory.LoadCheckpoint(carousel) && bareInventory.SaveCheckpoint() == carousel);
			if (inventory.Create(&controller, nullptr, InventoryMenuGUI::MenuMode::Full) < 0) throw std::runtime_error("inventory full reopen unavailable");
			inventory.m_InventoryActor = &inventoryActor;
			inventory.m_GUITopLevelBox->SetPositionAbs(10, 230);
			inventory.m_GUIInventoryItemButtons[0].first = &inventoryItem;
			auto* selectedButton = inventory.m_GUIInventoryItemButtons[0].second;
			selectedButton->SetIconAndText(icon.get(), "checkpoint inventory item");
			inventory.SetSelectedItem(selectedButton, &inventoryItem, 0, -1, true);
			inventory.m_GUISelectedItem->DragHoldCount = 17;
			inventory.m_NonMouseHighlightedButton = selectedButton;
			inventory.m_NonMousePreviousInventoryItemsBoxButton = selectedButton;
			inventory.m_NonMousePreviousReloadOrDropButton = inventory.m_GUIDropButton;
			inventory.m_GUIInventoryItemsScrollbar->SetMaximum(12); inventory.m_GUIInventoryItemsScrollbar->SetValue(3);
			clear_to_color(bitmap.get(), 0); inventory.m_GUIControlManager->Draw(&screen);
			const std::string fullPixels = SaveBitmap(bitmap.get());
			const std::string fullInventory = inventory.SaveCheckpoint();
			inventory.ClearSelectedItem(); selectedButton->SetText("changed"); inventory.m_InventoryActor = nullptr;
			check("inventory_full_restore", inventory.LoadCheckpoint(fullInventory) && inventory.SaveCheckpoint() == fullInventory);
			check("inventory_full_selection_aliases", inventory.GetInventoryActor() == &inventoryActor && inventory.m_GUISelectedItem && inventory.m_GUISelectedItem->Object == &inventoryItem && inventory.m_GUISelectedItem->Button == selectedButton && inventory.m_GUISelectedItem->DragWasHeldForLongEnough() && inventory.m_NonMouseHighlightedButton == selectedButton);
			clear_to_color(bitmap.get(), 0); inventory.m_GUIControlManager->Draw(&screen);
			check("inventory_full_rendered_pixels", SaveBitmap(bitmap.get()) == fullPixels);
			check("inventory_bare_full_apply", bareInventory.LoadCheckpoint(fullInventory) && bareInventory.SaveCheckpoint() == fullInventory && bareInventory.m_GUISelectedItem && bareInventory.m_GUISelectedItem->Button == bareInventory.m_GUIInventoryItemButtons[0].second);
			{
				// A menu still pointing at an object that has left the world: a peer's drop kills an
				// actor and the resync snapshot is taken before the menu's next Update drops the
				// pointer. A default-constructed Actor holds exactly the state a destroyed one does -
				// no unique id, unknown to MovableMan - so the case needs no lifetime games.
				Actor departedActor;
				MOPixel departedItem;
				check("inventory_departed_object_is_unknown", departedActor.GetUniqueID() == 0 && !g_MovableMan.IsKnownObject(&departedActor) && !departedActor.IsOriginalPreset());
				const std::string liveInventory = inventory.SaveCheckpoint();
				Actor* const liveActor = inventory.m_InventoryActor;
				const auto liveEquipped = inventory.m_InventoryActorEquippedItems;
				MovableObject* const liveButtonItem = inventory.m_GUIInventoryItemButtons[0].first;

				inventory.m_InventoryActor = &departedActor;
				const std::string departedActorState = inventory.SaveCheckpoint();
				check("inventory_departed_actor_saves_absent", departedActorState != liveInventory);
				check("inventory_departed_actor_restores_empty", inventory.LoadCheckpoint(departedActorState) && inventory.GetInventoryActor() == nullptr && inventory.SaveCheckpoint() == departedActorState);

				// Every non-owning object pointer the menu keeps, not only the actor.
				inventory.m_InventoryActor = liveActor;
				inventory.m_InventoryActorEquippedItems = {{&departedItem, &departedItem}};
				inventory.m_GUIInventoryItemButtons[0].first = &departedItem;
				const std::string departedItemState = inventory.SaveCheckpoint();
				check("inventory_departed_items_restore_empty", inventory.LoadCheckpoint(departedItemState) && inventory.m_InventoryActorEquippedItems.size() == 1 &&
				    !inventory.m_InventoryActorEquippedItems[0].first && !inventory.m_InventoryActorEquippedItems[0].second && !inventory.m_GUIInventoryItemButtons[0].first);

				// A live reference takes the same path it always did.
				inventory.m_InventoryActor = liveActor;
				inventory.m_InventoryActorEquippedItems = liveEquipped;
				inventory.m_GUIInventoryItemButtons[0].first = liveButtonItem;
				check("inventory_live_reference_unchanged", inventory.SaveCheckpoint() == liveInventory);

				// The writer keeps its teeth: an orphaned reference is still refused, and now says what.
				bool refused = false;
				std::string refusal;
				try { SaveEntityReference(&departedActor); } catch (const std::exception& error) { refused = true; refusal = error.what(); }
				check("orphaned_reference_still_refused", refused && refusal.find("no persistent owner") != std::string::npos);
				check("orphaned_reference_names_the_entity", refusal.find("Actor") != std::string::npos);
			}
			const std::string steadyInventory = inventory.SaveCheckpoint();
			check("inventory_malformed_atomic", !inventory.LoadCheckpoint(steadyInventory + "bad") && inventory.SaveCheckpoint() == steadyInventory);
		}
		{
			const std::string previousFrame = g_FrameMan.SaveCheckpoint();
			const auto previousSmallFonts = g_FrameMan.m_SmallFonts, previousLargeFonts = g_FrameMan.m_LargeFonts;
			struct RestoreFrame { std::string state; ~RestoreFrame() { g_FrameMan.LoadCheckpoint(state); } } restoreFrame{previousFrame};
			std::array<GUIFont*, 4> fonts{g_FrameMan.GetSmallFont(), g_FrameMan.GetSmallFont(true), g_FrameMan.GetLargeFont(), g_FrameMan.GetLargeFont(true)};
			std::array<std::string, 4> rendered;
			const auto drawFont = [&](GUIFont& font) {
				std::unique_ptr<BITMAP, void(*)(BITMAP*)> target(create_bitmap_ex(font.m_Font->GetColorDepth(), 360, 90), destroy_bitmap);
				clear_to_color(target.get(), 0); AllegroBitmap image(target.get());
				font.Draw(&image, 11, 19, "Frame font checkpoint"); return SaveBitmap(target.get());
			};
			for (size_t index = 0; index < fonts.size(); ++index) {
				auto* font = fonts[index];
				const unsigned long color = font->m_Screen->ConvertColor(37 + index, font->m_Font->GetColorDepth());
				font->CacheColor(color); font->SetColor(color); font->SetKerning(2 + index); font->m_Leading = 3 + index;
				rendered[index] = drawFont(*font);
			}
			const std::string frameFonts = g_FrameMan.SaveCheckpoint();
			for (auto* font: fonts) { font->SetKerning(11); font->m_Leading = 13; font->m_CurrentBitmap->SetPixel(1, 1, 91); }
			check("frame_font_restore", g_FrameMan.LoadCheckpoint(frameFonts));
			check("frame_font_pointer_identity", g_FrameMan.GetSmallFont() == fonts[0] && g_FrameMan.GetSmallFont(true) == fonts[1] && g_FrameMan.GetLargeFont() == fonts[2] && g_FrameMan.GetLargeFont(true) == fonts[3]);
			check("frame_font_canonical", g_FrameMan.SaveCheckpoint() == frameFonts);
			bool samePixels = true;
			for (size_t index = 0; index < fonts.size(); ++index) samePixels = samePixels && drawFont(*fonts[index]) == rendered[index];
			check("frame_font_rendered_pixels", samePixels);
			const std::string steadyFrame = g_FrameMan.SaveCheckpoint();
			check("frame_font_malformed_atomic", !g_FrameMan.LoadCheckpoint(steadyFrame.substr(0, steadyFrame.size() - 1)) && g_FrameMan.SaveCheckpoint() == steadyFrame);
			check("frame_font_original_slots", g_FrameMan.LoadCheckpoint(previousFrame) && g_FrameMan.m_SmallFonts == previousSmallFonts && g_FrameMan.m_LargeFonts == previousLargeFonts && g_FrameMan.SaveCheckpoint() == previousFrame);
		}
		{
			Scene originalScene, restoredScene;
			std::vector<std::pair<const Entity*, const Entity*>> pairs;
			for (int set = 0; set < Scene::PLACEDSETSCOUNT; ++set) for (int index = 0; index < 2; ++index) {
				auto* original = new TerrainObject(); auto* restored = new TerrainObject();
				const std::string name = "checkpoint-scene-" + std::to_string(set) + "-" + std::to_string(index);
				original->SetPresetName(name, true); restored->SetPresetName(name, true);
				originalScene.AddPlacedObject(set, original); restoredScene.AddPlacedObject(set, restored);
				pairs.emplace_back(original, restored);
			}
			bool matches = true;
			for (const auto& [original, restored]: pairs) matches = matches && LoadEntityReference(SaveEntityReference(original, &originalScene), false, &restoredScene) == restored;
			check("scene_placed_object_aliases", matches);
			CheckpointWriter absent("GUIEntity2"); absent(true, 2, 0L, Scene::PLACEONLOAD, 2ULL, std::string("TerrainObject"), std::string{}, std::string{});
			bool refused = false; try { LoadEntityReference(absent.Text(), false, &restoredScene); } catch (const std::exception&) { refused = true; }
			check("scene_placed_missing_index", refused);
			CheckpointWriter wrongClass("GUIEntity2"); wrongClass(true, 2, 0L, Scene::PLACEONLOAD, 0ULL, std::string("AHuman"), std::string{}, std::string{});
			refused = false; try { LoadEntityReference(wrongClass.Text(), false, &restoredScene); } catch (const std::exception&) { refused = true; }
			check("scene_placed_class_mismatch", refused);
			CheckpointWriter oldNull("GUIEntity1"); oldNull(false);
			check("legacy_entity_reference", LoadEntityReference(oldNull.Text()) == nullptr);
		}
		{
			GAScripted seed, source;
			if (source.Create(seed) < 0) throw std::runtime_error("clone rejection fixture unavailable");
			const std::string sourceBefore = source.SaveCheckpoint();
			source.GetBuyGUI(0)->m_PendingCheckpoint = "invalid-checkpoint";
			const auto pools = [] {
				auto stream = std::make_unique<std::stringstream>(); auto* raw = stream.get(); Writer writer(std::move(stream));
				Entity::ClassInfo::DumpPoolMemoryInfo(writer); return raw->str();
			};
			const std::string poolBefore = pools();
			int refused = 0;
			for (int attempt = 0; attempt < 3; ++attempt) {
				MovableObject::FaithfulCloneScope cloning(false);
				try { std::unique_ptr<Entity> candidate(source.Clone()); } catch (const std::exception&) { ++refused; }
			}
			check("failed_checkpoint_clone_rejected", refused == 3);
			check("failed_checkpoint_clone_pool_cleanup", pools() == poolBefore);
			source.GetBuyGUI(0)->m_PendingCheckpoint.clear();
			check("failed_checkpoint_clone_original_unchanged", source.SaveCheckpoint() == sourceBefore);
		}
		{
			// A save written where a module was missing or extra is still that game's menu state.
			const int moduleCount = g_PresetMan.GetTotalModuleCount();
			BuyMenuGUI menu;
			ObjectPickerGUI picker;
			const auto moduleFlags = [](int count, int expandedModule) {
				std::vector<bool> flags(count > 0 ? count : 0, false);
				if (expandedModule >= 0 && expandedModule < count) flags[expandedModule] = true;
				return flags;
			};
			const auto oldBuyMenuRecord = [&](int count) {
				CheckpointWriter writer("BuyMenuGUI2");
				writer(menu.m_CheckpointInitialized);
				BuyMenuGUI::VisitCheckpoint(writer, menu);
				writer(GUICheckpoint::SaveEntityReference(menu.m_pSelectedCraft), moduleFlags(count, 0), size_t{0}, false);
				return writer.Text();
			};
			check("buy_menu_record_from_fewer_modules", menu.LoadCheckpoint(oldBuyMenuRecord(moduleCount - 1), true));
			check("buy_menu_record_from_more_modules", menu.LoadCheckpoint(oldBuyMenuRecord(moduleCount + 2), true));
			picker.m_ExpandedModules = moduleFlags(moduleCount - 1, 1);
			check("object_picker_record_from_fewer_modules", picker.LoadCheckpoint(picker.SaveCheckpoint()));
			const bool sized = picker.m_ExpandedModules.size() == static_cast<size_t>(moduleCount);
			check("object_picker_module_flags_sized", sized);
			check("object_picker_module_flags_kept", sized && moduleCount > 2 && picker.m_ExpandedModules[1] && !picker.m_ExpandedModules[moduleCount - 1]);
			picker.m_PendingCheckpoint.clear();
			const std::vector<bool> chosen = moduleFlags(moduleCount, moduleCount - 1);
			picker.m_ExpandedModules = chosen;
			check("object_picker_module_flags_round_trip", picker.LoadCheckpoint(picker.SaveCheckpoint()) && picker.m_ExpandedModules == chosen);
		}
	} catch (const std::exception& exception) { std::cout << "[gui-checkpoint-selftest] exception=" << exception.what() << std::endl; passed = false; }
	if (!GUIInput::LoadSharedCheckpoint(oldSharedInput)) passed = false;
	std::cout << "[gui-checkpoint-selftest] " << (passed ? "PASS" : "FAIL") << " complete checked=" << checked << std::endl;
	return passed;
}
