#include "CheckpointArchive.h"
#include "GUICheckpoint.h"
#include <iostream>
#include "InventoryMenuGUI.h"

#include "WindowMan.h"
#include "FrameMan.h"
#include "UInputMan.h"
#include "MovableMan.h"
#include "PresetMan.h"
#include "GLResourceMan.h"

#include "Controller.h"
#include "AHuman.h"
#include "HDFirearm.h"
#include "Magazine.h"
#include "Icon.h"
#include "NetGameCommand.h"
#include "ScenarioRunner.h"

#include "AllegroBitmap.h"
#include "GUIFont.h"

#include "GUIControlManager.h"
#include "GUICollectionBox.h"
#include "GUILabel.h"
#include "GUIButton.h"
#include "GUIScrollbar.h"

using namespace RTE;

const Vector InventoryMenuGUI::c_CarouselBoxMaxSize(50, 32);
const Vector InventoryMenuGUI::c_CarouselBoxMinSize(40, 24);
const Vector InventoryMenuGUI::c_CarouselBoxSizeStep = (c_CarouselBoxMaxSize - c_CarouselBoxMinSize) / (c_ItemsPerRow / 2);
const int InventoryMenuGUI::c_CarouselBoxCornerRadius = ((c_CarouselBoxMaxSize.GetFloorIntY() - c_CarouselBoxMinSize.GetFloorIntY()) / 2) - 1;

BITMAP* InventoryMenuGUI::s_CursorBitmap = nullptr;

InventoryMenuGUI::InventoryMenuGUI() {
	Clear();
}

InventoryMenuGUI::~InventoryMenuGUI() {
	Destroy();
}

void InventoryMenuGUI::CarouselItemBox::GetIconsAndMass(std::vector<BITMAP*>& itemIcons, float& totalItemMass, const std::vector<std::pair<MovableObject*, MovableObject*>>* equippedItems) const {
	if (IsForEquippedItems) {
		itemIcons.reserve(equippedItems->size());
		for (const auto& [equippedItem, offhandEquippedItem]: *equippedItems) {
			if (equippedItem && equippedItem->GetUniqueID() != 0) {
				itemIcons.emplace_back(equippedItem->GetGraphicalIcon());
				totalItemMass += equippedItem->GetMass();
			}
			if (offhandEquippedItem && offhandEquippedItem->GetUniqueID() != 0) {
				itemIcons.emplace_back(offhandEquippedItem->GetGraphicalIcon());
				totalItemMass += offhandEquippedItem->GetMass();
			}
		}
	} else {
		itemIcons.emplace_back(Item->GetGraphicalIcon());
		totalItemMass += Item->GetMass();
	}
}

void InventoryMenuGUI::Clear() {
	m_PendingCheckpoint.clear();
	m_CheckpointInitialized = false;
	m_SmallFont = nullptr;
	m_LargeFont = nullptr;

	m_MenuController = nullptr;
	m_InventoryActor = nullptr;
	m_MenuMode = MenuMode::Carousel;
	m_CenterPos.Reset();

	m_EnabledState = EnabledState::Disabled;
	m_EnableDisableAnimationTimer.Reset();
	m_EnableDisableAnimationTimer.SetRealTimeLimitMS(100);

	m_InventoryActorIsHuman = false;
	m_InventoryActorEquippedItems.clear();

	m_CarouselDrawEmptyBoxes = true;
	m_CarouselBackgroundTransparent = true;
	m_CarouselBackgroundBoxColor = 4;
	m_CarouselBackgroundBoxBorderSize = Vector(2, 1);
	m_CarouselBackgroundBoxBorderColor = g_MaskColor;

	m_CarouselAnimationDirection = CarouselAnimationDirection::None;
	m_CarouselAnimationTimer.Reset();
	m_CarouselAnimationTimer.SetRealTimeLimitMS(200);
	for (std::unique_ptr<CarouselItemBox>& carouselItemBox: m_CarouselItemBoxes) {
		carouselItemBox = nullptr;
	}
	m_CarouselExitingItemBox.reset();
	m_CarouselBitmap = nullptr;
	m_CarouselBGBitmap = nullptr;

	m_GUIDisplayOnly = false;
	m_GUIShowEmptyRows = false;
	m_GUICursorPos.Reset();
	m_PreviousGUICursorPos.Reset();
	m_GUIInventoryActorCurrentEquipmentSetIndex = 0;
	m_GUISelectedItem = nullptr;

	m_GUIRepeatStartTimer.Reset();
	m_GUIRepeatTimer.Reset();
	m_NonMouseHighlightedButton = nullptr;
	m_NonMousePreviousEquippedItemsBoxButton = nullptr;
	m_NonMousePreviousInventoryItemsBoxButton = nullptr;
	m_NonMousePreviousReloadOrDropButton = nullptr;

	m_GUITopLevelBoxFullSize.Reset();
	m_GUIShowInformationText = true;
	m_GUIInformationToggleButtonIcon = nullptr;
	m_GUIReloadButtonIcon = nullptr;
	m_GUIDropButtonIcon = nullptr;
	m_GUIInventoryItemButtons.clear();
	m_GUIInventoryItemButtons.reserve(c_FullViewPageItemLimit);

	m_GUIControlManager = nullptr;
	m_GUIScreen = nullptr;
	m_GUIInput = nullptr;
	m_GUITopLevelBox = nullptr;
	m_GUIInformationText = nullptr;
	m_GUIInformationToggleButton = nullptr;
	m_GUIEquippedItemsBox = nullptr;
	m_GUISwapSetButton = nullptr;
	m_GUIEquippedItemButton = nullptr;
	m_GUIOffhandEquippedItemButton = nullptr;
	m_GUIReloadButton = nullptr;
	m_GUIDropButton = nullptr;
	m_GUIInventoryItemsBox = nullptr;
	m_GUIInventoryItemsScrollbar = nullptr;
}

int InventoryMenuGUI::Create(Controller* activityPlayerController, Actor* inventoryActor, MenuMode menuMode) {
	m_CheckpointInitialized = true;
	RTEAssert(activityPlayerController, "No controller sent to InventoryMenuGUI on creation!");
	RTEAssert(c_ItemsPerRow % 2 == 1, "Don't you dare use an even number of items per inventory row, you filthy animal!");

	if (!m_SmallFont) {
		m_SmallFont = g_FrameMan.GetSmallFont();
	}
	if (!m_LargeFont) {
		m_LargeFont = g_FrameMan.GetLargeFont();
	}

	m_MenuController = activityPlayerController;
	SetInventoryActor(inventoryActor);
	m_MenuMode = menuMode;

	if (m_MenuMode == MenuMode::Carousel && !CarouselModeReadyForUse()) {
		return SetupCarouselMode();
	} else if ((m_MenuMode == MenuMode::Full || m_MenuMode == MenuMode::Transfer) && !FullOrTransferModeReadyForUse()) {
		return SetupFullOrTransferMode();
	}

	return 0;
}

void InventoryMenuGUI::Destroy() {
	destroy_bitmap(m_CarouselBitmap.release());
	destroy_bitmap(m_CarouselBGBitmap.release());

	Clear();
}

int InventoryMenuGUI::SetupCarouselMode() {
	for (std::unique_ptr<CarouselItemBox>& carouselItemBox: m_CarouselItemBoxes) {
		carouselItemBox = std::make_unique<CarouselItemBox>();
		carouselItemBox->IsForEquippedItems = false;
	}
	int equippedItemIndex = c_ItemsPerRow / 2;
	Vector currentBoxSize = c_CarouselBoxMaxSize;
	int carouselBitmapWidth = 0;
	for (int i = equippedItemIndex; i >= 0; i--) {
		m_CarouselItemBoxes.at(i)->FullSize = currentBoxSize;
		m_CarouselItemBoxes.at(i)->IconCenterPosition.SetY(c_CarouselBoxMaxSize.GetY() / 2 + static_cast<float>(m_SmallFont->GetFontHeight() / 2));
		carouselBitmapWidth += currentBoxSize.GetFloorIntX();
		if (i != equippedItemIndex) {
			m_CarouselItemBoxes.at((equippedItemIndex * 2) - i)->FullSize = currentBoxSize;
			m_CarouselItemBoxes.at((equippedItemIndex * 2) - i)->IconCenterPosition.SetY(c_CarouselBoxMaxSize.GetY() / 2 + static_cast<float>(m_SmallFont->GetFontHeight() / 2));
			carouselBitmapWidth += currentBoxSize.GetFloorIntX();
		} else {
			m_CarouselItemBoxes.at(i)->IsForEquippedItems = true;
		}
		currentBoxSize -= c_CarouselBoxSizeStep;
	}

	m_CarouselExitingItemBox = std::make_unique<CarouselItemBox>();
	m_CarouselExitingItemBox->IsForEquippedItems = false;
	m_CarouselExitingItemBox->IconCenterPosition.SetY(c_CarouselBoxMaxSize.GetY() / 2 + static_cast<float>(m_SmallFont->GetFontHeight() / 2));
	m_CarouselExitingItemBox->FullSize = c_CarouselBoxMinSize - c_CarouselBoxSizeStep;

	m_CarouselBitmap = std::unique_ptr<BITMAP>(create_bitmap_ex(8, carouselBitmapWidth, c_CarouselBoxMaxSize.GetFloorIntY() + m_SmallFont->GetFontHeight() / 2));
	m_CarouselBGBitmap = std::unique_ptr<BITMAP>(create_bitmap_ex(8, carouselBitmapWidth, c_CarouselBoxMaxSize.GetFloorIntY() + m_SmallFont->GetFontHeight() / 2));
	clear_to_color(m_CarouselBitmap.get(), g_MaskColor);
	clear_to_color(m_CarouselBGBitmap.get(), g_MaskColor);

	return 0;
}

int InventoryMenuGUI::SetupFullOrTransferMode() {
	if (!m_GUIControlManager) {
		m_GUIControlManager = std::make_unique<GUIControlManager>();
	}
	if (!m_GUIScreen) {
		m_GUIScreen = std::make_unique<AllegroScreen>(g_FrameMan.GetBackBuffer8());
	}
	if (!m_GUIInput) {
		m_GUIInput = std::make_unique<GUIInputWrapper>(m_MenuController->GetPlayer());
	}
	RTEAssert(m_GUIControlManager->Create(m_GUIScreen.get(), m_GUIInput.get(), "Base.rte/GUIs/Skins", "InventoryMenuSkin.ini"), "Failed to create InventoryMenuGUI GUIControlManager and load it from Base.rte/GUIs/Skins/Menus/InventoryMenuSkin.ini");

	// TODO When this is split into 2 classes, full mode should use the fonts from its gui control manager while transfer mode, will need to get its fonts from FrameMan. May be good for the ingame menu base class to have these font pointers, even if some subclasses set em up in different ways.
	// if (!m_SmallFont) { m_SmallFont = m_GUIControlManager->GetSkin()->GetFont("FontSmall.png"); }
	// if (!m_LargeFont) { m_LargeFont = m_GUIControlManager->GetSkin()->GetFont("FontLarge.png"); }

	m_GUIControlManager->Load("Base.rte/GUIs/InventoryMenuGUI.ini");
	m_GUIControlManager->EnableMouse(m_MenuController->IsMouseControlled());
	if (!s_CursorBitmap) {
		ContentFile cursorFile("Base.rte/GUIs/Skins/Cursor.png");
		s_CursorBitmap = cursorFile.GetAsBitmap();
	}

	dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("base"))->SetSize(g_WindowMan.GetResX(), g_WindowMan.GetResY());

	m_GUITopLevelBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBox_InventoryMenuGUI"));
	m_GUITopLevelBox->SetPositionAbs(g_FrameMan.GetPlayerFrameBufferWidth(m_MenuController->GetPlayer()), 0);
	m_GUITopLevelBoxFullSize.SetXY(static_cast<float>(m_GUITopLevelBox->GetWidth()), static_cast<float>(m_GUITopLevelBox->GetHeight()));
	m_GUIInformationText = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("Label_InformationText"));
	m_GUIInformationToggleButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("Button_InformationToggle"));
	m_GUIInformationToggleButton->SetText("");
	m_GUIInformationToggleButtonIcon = dynamic_cast<const Icon*>(g_PresetMan.GetEntityPreset("Icon", "Information"));

	m_GUIEquippedItemsBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBox_EquippedItems"));
	m_GUISwapSetButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("Button_SwapSet"));
	m_GUISwapSetButton->SetEnabled(false);
	m_GUISwapSetButton->SetVisible(false);
	m_GUIEquippedItemButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("Button_EquippedItem"));
	m_GUIEquippedItemButton->SetHorizontalOverflowScroll(true);
	m_GUIOffhandEquippedItemButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("Button_OffhandEquippedItem"));
	m_GUIOffhandEquippedItemButton->SetHorizontalOverflowScroll(true);
	m_GUIReloadButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("Button_Reload"));
	m_GUIReloadButton->SetText("");
	m_GUIReloadButtonIcon = dynamic_cast<const Icon*>(g_PresetMan.GetEntityPreset("Icon", "Refresh"));
	m_GUIDropButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("Button_Drop"));
	m_GUIDropButton->SetText("");
	m_GUIDropButtonIcon = dynamic_cast<const Icon*>(g_PresetMan.GetEntityPreset("Icon", "Drop"));

	m_GUIInventoryItemsBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBox_InventoryItems"));
	m_GUIInventoryItemsScrollbar = dynamic_cast<GUIScrollbar*>(m_GUIControlManager->GetControl("Scrollbar_InventoryItems"));
	m_GUIInventoryItemsScrollbar->SetValue(0);
	m_GUIInventoryItemsScrollbar->SetMinimum(0);

	GUIButton* inventoryItemButtonTemplate = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("Button_InventoryItemTemplate"));
	inventoryItemButtonTemplate->SetEnabled(false);
	inventoryItemButtonTemplate->SetVisible(false);

	GUIProperties inventoryItemButtonProperties;
	inventoryItemButtonProperties.AddVariable("ControlType", GUIButton::GetControlID());
	inventoryItemButtonProperties.AddVariable("Parent", inventoryItemButtonTemplate->GetParent()->GetName());
	inventoryItemButtonProperties.AddVariable("Width", inventoryItemButtonTemplate->GetWidth());
	inventoryItemButtonProperties.AddVariable("Height", inventoryItemButtonTemplate->GetHeight());
	std::string emptyButtonText = "> <";
	inventoryItemButtonProperties.AddVariable("Text", emptyButtonText);
	inventoryItemButtonProperties.AddVariable("HorizontalOverflowScroll", true);
	std::pair<MovableObject*, GUIButton*> itemButtonPair;

	for (int i = 0; i < c_FullViewPageItemLimit; i++) {
		inventoryItemButtonProperties.AddVariable("Name", std::to_string(i));
		itemButtonPair = {nullptr, dynamic_cast<GUIButton*>(m_GUIControlManager->AddControl(&inventoryItemButtonProperties))};
		itemButtonPair.second->SetPositionRel(inventoryItemButtonTemplate->GetRelXPos() + ((i % c_ItemsPerRow) * inventoryItemButtonTemplate->GetWidth()), inventoryItemButtonTemplate->GetRelYPos() + ((i / c_ItemsPerRow) * inventoryItemButtonTemplate->GetHeight()));
		m_GUIInventoryItemButtons.emplace_back(itemButtonPair);
	}

	return 0;
}

void InventoryMenuGUI::SetInventoryActor(Actor* newInventoryActor) {
	m_InventoryActor = newInventoryActor;
	if (m_InventoryActor) {
		m_InventoryActorIsHuman = dynamic_cast<AHuman*>(m_InventoryActor);

		if (g_SceneMan.ShortestDistance(m_CenterPos, m_InventoryActor->GetCPUPos(), g_SceneMan.SceneWrapsX()).GetMagnitude() > 2.0F) {
			m_CenterPos = m_InventoryActor->GetCPUPos();
		}
	}
}

void InventoryMenuGUI::SetEnabled(bool enable) {
	if (!m_MenuController || !m_InventoryActor) {
		return;
	}

	bool enabledStateDoesNotMatchInput = (enable && m_EnabledState != EnabledState::Enabled && m_EnabledState != EnabledState::Enabling) || (!enable && m_EnabledState != EnabledState::Disabled && m_EnabledState != EnabledState::Disabling);
	if (enabledStateDoesNotMatchInput) {
		m_EnabledState = enable ? EnabledState::Enabling : EnabledState::Disabling;
		m_EnableDisableAnimationTimer.Reset();

		if (m_MenuMode == MenuMode::Full || m_MenuMode == MenuMode::Transfer) {
			ClearSelectedItem();
			m_NonMousePreviousEquippedItemsBoxButton = nullptr;
			m_NonMousePreviousInventoryItemsBoxButton = nullptr;
			m_NonMousePreviousReloadOrDropButton = nullptr;
			if (m_NonMouseHighlightedButton) {
				m_NonMouseHighlightedButton->OnMouseLeave(0, 0, 0, 0);
			}
			for (const auto& [inventoryItem, inventoryItemButton]: m_GUIInventoryItemButtons) {
				inventoryItemButton->OnMouseLeave(0, 0, 0, 0);
			}
			SoundContainer* soundToPlay = enable ? g_GUISound.EnterMenuSound() : g_GUISound.ExitMenuSound();
			soundToPlay->Play();
		}
	}
}

bool InventoryMenuGUI::EnableIfNotEmpty() {
	bool shouldEnable = !m_InventoryActorEquippedItems.empty() || (m_InventoryActor && !m_InventoryActor->IsInventoryEmpty());
	SetEnabled(shouldEnable);
	return shouldEnable;
}

void InventoryMenuGUI::ClearSelectedItem() {
	if (m_GUISelectedItem) {
		m_GUISelectedItem->Button->OnLoseFocus();
		m_GUISelectedItem = nullptr;
	}
}

void InventoryMenuGUI::SetSelectedItem(GUIButton* selectedItemButton, MovableObject* selectedItemObject, int inventoryIndex, int equippedItemIndex, bool isBeingDragged) {
	if (!selectedItemButton || !selectedItemObject || (inventoryIndex < 0 && equippedItemIndex < 0)) {
		ClearSelectedItem();
	}

	if (!m_GUISelectedItem) {
		m_GUISelectedItem = std::make_unique<GUISelectedItem>();
	}
	m_GUISelectedItem->Button = selectedItemButton;
	m_GUISelectedItem->Object = selectedItemObject;
	m_GUISelectedItem->InventoryIndex = inventoryIndex;
	m_GUISelectedItem->EquippedItemIndex = equippedItemIndex;
	m_GUISelectedItem->IsBeingDragged = isBeingDragged;
	m_GUISelectedItem->DragHoldCount = 0;
}

void InventoryMenuGUI::Update() {
	if (IsEnabled() && (!m_MenuController || !m_InventoryActor || !g_MovableMan.ValidMO(m_InventoryActor))) {
		SetEnabled(false);
		m_EnableDisableAnimationTimer.SetElapsedRealTimeMS(m_EnableDisableAnimationTimer.GetRealTimeLimitMS());
	}
	if (IsEnablingOrDisabling() && m_EnableDisableAnimationTimer.IsPastRealTimeLimit()) {
		m_EnabledState = (m_EnabledState == EnabledState::Enabling) ? EnabledState::Enabled : EnabledState::Disabled;
		// Note gui size setting is handled here rather than in Full Mode updating so it can be made to only happen once when things are fully enabled, instead of setting over-and-over and potentially triggering extra redrawing.
		if (m_EnabledState == EnabledState::Enabled && m_MenuMode != MenuMode::Carousel) {
			m_GUITopLevelBox->SetSize(m_GUITopLevelBoxFullSize.GetFloorIntX(), m_GUITopLevelBoxFullSize.GetFloorIntY());
		}
	}

	if (m_InventoryActor && g_MovableMan.ValidMO(m_InventoryActor)) {
		if (const AHuman* inventoryActorAsAHuman = (m_InventoryActorIsHuman ? dynamic_cast<AHuman*>(m_InventoryActor) : nullptr)) {
			m_InventoryActorEquippedItems.clear();
			m_InventoryActorEquippedItems.reserve(1);
			if (inventoryActorAsAHuman->GetEquippedItem() || inventoryActorAsAHuman->GetEquippedBGItem()) {
				m_InventoryActorEquippedItems.push_back({inventoryActorAsAHuman->GetEquippedItem(), inventoryActorAsAHuman->GetEquippedBGItem()});
			}
		} else if (!m_InventoryActorEquippedItems.empty()) {
			m_InventoryActorEquippedItems.clear();
		}
	} else {
		m_InventoryActor = nullptr;
	}

	if (m_InventoryActor && IsVisible()) {
		switch (m_MenuMode) {
			case MenuMode::Carousel:
				if (m_InventoryActorEquippedItems.empty() && m_InventoryActor->IsInventoryEmpty()) {
					SetEnabled(false);
				} else {
					UpdateCarouselMode();
				}
				break;
			case MenuMode::Full:
				UpdateFullMode();
				break;
			case MenuMode::Transfer:
				UpdateTransferMode();
				break;
			default:
				RTEAbort("Unsupported InventoryMenuGUI MenuMode during Update " + std::to_string(static_cast<int>(m_MenuMode)));
		}
	}
}

void InventoryMenuGUI::Draw(BITMAP* targetBitmap, const Vector& targetPos) const {
	Vector drawPos = m_CenterPos - targetPos;

	switch (m_MenuMode) {
		case MenuMode::Carousel:
			if (!m_InventoryActor || (m_InventoryActor->IsInventoryEmpty() && m_InventoryActorEquippedItems.empty() && m_EnabledState != EnabledState::Disabling)) {
				return;
			}
			drawPos -= Vector(0, c_CarouselMenuVerticalOffset + c_CarouselBoxMaxSize.GetY() * 0.5F);
			DrawCarouselMode(targetBitmap, drawPos);
			break;
		case MenuMode::Full:
			if (!m_InventoryActor) {
				return;
			}
			drawPos -= Vector((m_GUITopLevelBoxFullSize.GetX() - static_cast<float>(m_GUIInventoryItemsScrollbar->IsEnabled() ? m_GUIInventoryItemsScrollbar->GetWidth() : 0)) / 2.0F, m_GUITopLevelBoxFullSize.GetY() + c_FullMenuVerticalOffset);
			DrawFullMode(targetBitmap, drawPos);
			break;
		case MenuMode::Transfer:
			break;
		default:
			RTEAbort("Unsupported InventoryMenuGUI MenuMode during Draw " + std::to_string(static_cast<int>(m_MenuMode)));
	}
}

void InventoryMenuGUI::UpdateCarouselMode() {
	if (!CarouselModeReadyForUse()) {
		SetupCarouselMode();
	}

	for (const std::unique_ptr<CarouselItemBox>& carouselItemBox: m_CarouselItemBoxes) {
		carouselItemBox->Item = nullptr;
	}
	const std::deque<MovableObject*>* inventory = m_InventoryActor->GetInventory();
	if (inventory && !inventory->empty()) {
		int leftSideItemCount = std::min(static_cast<int>(std::floor(static_cast<float>(inventory->size()) / 2)), c_ItemsPerRow / 2);
		int rightSideItemCount = std::min(static_cast<int>(std::ceil(static_cast<float>(inventory->size()) / 2)), c_ItemsPerRow / 2 + (m_InventoryActorEquippedItems.empty() ? 1 : 0));

		int carouselIndex = 0;
		std::vector<MovableObject*> temporaryLeftSideItemsForProperOrdering;
		temporaryLeftSideItemsForProperOrdering.reserve(leftSideItemCount);
		if (leftSideItemCount > 0) {
			std::for_each(inventory->crbegin(), inventory->crbegin() + leftSideItemCount, [&temporaryLeftSideItemsForProperOrdering](MovableObject* carouselItem) {
				temporaryLeftSideItemsForProperOrdering.emplace_back(carouselItem);
			});
		}
		std::for_each(temporaryLeftSideItemsForProperOrdering.crbegin(), temporaryLeftSideItemsForProperOrdering.crend(), [this, &carouselIndex, &leftSideItemCount](MovableObject* carouselItem) {
			m_CarouselItemBoxes.at(carouselIndex + (c_ItemsPerRow / 2) - leftSideItemCount)->Item = carouselItem;
			m_CarouselItemBoxes.at(carouselIndex)->IsForEquippedItems = false;
			carouselIndex++;
		});
		carouselIndex = c_ItemsPerRow / 2;
		if (!m_InventoryActorEquippedItems.empty()) {
			m_CarouselItemBoxes.at(carouselIndex)->IsForEquippedItems = true;
			carouselIndex++;
		}
		if (rightSideItemCount > 0) {
			std::for_each(inventory->cbegin(), inventory->cbegin() + rightSideItemCount, [this, &carouselIndex](MovableObject* carouselItem) {
				m_CarouselItemBoxes.at(carouselIndex)->Item = carouselItem;
				m_CarouselItemBoxes.at(carouselIndex)->IsForEquippedItems = false;
				carouselIndex++;
			});
		}

		if (m_InventoryActor->GetController()->IsState(ControlState::WEAPON_CHANGE_PREV)) {
			m_CarouselAnimationDirection = CarouselAnimationDirection::Right;
			m_CarouselAnimationTimer.Reset();
			m_CarouselExitingItemBox->Item = m_CarouselItemBoxes[m_CarouselItemBoxes.size() - 1]->Item;
		} else if (m_InventoryActor->GetController()->IsState(ControlState::WEAPON_CHANGE_NEXT)) {
			m_CarouselAnimationDirection = CarouselAnimationDirection::Left;
			m_CarouselAnimationTimer.Reset();
			m_CarouselExitingItemBox->Item = m_CarouselItemBoxes[0]->Item;
		}

		if (m_CarouselAnimationDirection != CarouselAnimationDirection::None && m_CarouselAnimationTimer.IsPastRealTimeLimit()) {
			m_CarouselAnimationDirection = CarouselAnimationDirection::None;
			m_CarouselExitingItemBox->Item = nullptr;
		}
	}
	UpdateCarouselItemBoxSizesAndPositions();
}

void InventoryMenuGUI::UpdateCarouselItemBoxSizesAndPositions() {
	float halfMassFontHeight = static_cast<float>(m_SmallFont->GetFontHeight() / 2);
	int carouselIndex = 0;
	float carouselAnimationProgress = static_cast<float>(m_CarouselAnimationTimer.GetRealTimeLimitProgress());
	float directionalAnimationProgress = carouselAnimationProgress * static_cast<float>(m_CarouselAnimationDirection);
	float currentBoxHorizontalOffset = (directionalAnimationProgress <= 0 ? directionalAnimationProgress : -(1.0F - directionalAnimationProgress)) * m_CarouselExitingItemBox->FullSize.GetX();

	/// <summary>
	/// Lambda to do all the repetitive boilerplate of setting up a carousel item box.
	/// </summary>
	auto SetupCarouselItemBox = [this, &halfMassFontHeight, &carouselIndex, &carouselAnimationProgress, &currentBoxHorizontalOffset](const std::unique_ptr<CarouselItemBox>& carouselItemBox, bool leftSideRoundedAndBordered, bool rightSideRoundedAndBordered) {
		carouselItemBox->CurrentSize = carouselItemBox->FullSize;
		if (carouselIndex == -1) {
			carouselItemBox->CurrentSize += c_CarouselBoxSizeStep * (1.0F - carouselAnimationProgress);
		} else if (m_CarouselAnimationDirection != CarouselAnimationDirection::None) {
			int animationStartSizeIndex = carouselIndex - static_cast<int>(m_CarouselAnimationDirection);
			if (animationStartSizeIndex < 0 || animationStartSizeIndex >= m_CarouselItemBoxes.size()) {
				carouselItemBox->CurrentSize -= c_CarouselBoxSizeStep * (1.0F - carouselAnimationProgress);
			} else {
				carouselItemBox->CurrentSize += (m_CarouselItemBoxes.at(animationStartSizeIndex)->FullSize - carouselItemBox->CurrentSize) * (1.0F - carouselAnimationProgress);
			}
		}
		carouselItemBox->Pos.SetXY(currentBoxHorizontalOffset, halfMassFontHeight + (c_CarouselBoxMaxSize.GetY() - carouselItemBox->CurrentSize.GetY()) / 2);
		carouselItemBox->IconCenterPosition.SetX(carouselItemBox->Pos.GetX() + (carouselItemBox->CurrentSize.GetX() / 2));
		carouselItemBox->RoundedAndBorderedSides = {leftSideRoundedAndBordered, rightSideRoundedAndBordered};
		carouselIndex++;
		currentBoxHorizontalOffset += carouselItemBox->CurrentSize.GetX();
	};

	if (m_CarouselAnimationDirection == CarouselAnimationDirection::Left) {
		carouselIndex = -1;
		SetupCarouselItemBox(m_CarouselExitingItemBox, true, false);
	}

	for (const std::unique_ptr<CarouselItemBox>& carouselItemBox: m_CarouselItemBoxes) {
		if (carouselIndex == 0 && m_CarouselAnimationDirection == CarouselAnimationDirection::Right && directionalAnimationProgress == 0.0F) {
			carouselIndex++;
			continue;
		}
		std::pair<bool, bool> roundedAndBorderedSides;
		if (carouselIndex < c_ItemsPerRow / 2) {
			roundedAndBorderedSides = {true, m_CarouselAnimationDirection == CarouselAnimationDirection::Left && carouselIndex == (c_ItemsPerRow / 2) - 1 && directionalAnimationProgress > -0.5F};
		} else if (carouselIndex == c_ItemsPerRow / 2) {
			roundedAndBorderedSides = {directionalAnimationProgress >= 0 || directionalAnimationProgress <= -0.5F, directionalAnimationProgress <= 0 || directionalAnimationProgress >= 0.5F};
		} else {
			roundedAndBorderedSides = {m_CarouselAnimationDirection == CarouselAnimationDirection::Right && carouselIndex == (c_ItemsPerRow / 2) + 1 && directionalAnimationProgress < 0.5F, true};
		}
		SetupCarouselItemBox(carouselItemBox, roundedAndBorderedSides.first, roundedAndBorderedSides.second);
	}

	if (m_CarouselAnimationDirection == CarouselAnimationDirection::Right) {
		carouselIndex = -1;
		SetupCarouselItemBox(m_CarouselExitingItemBox, false, true);
	}
}

void InventoryMenuGUI::UpdateFullMode() {
	if (!FullOrTransferModeReadyForUse()) {
		SetupFullOrTransferMode();
	}

	if (!m_GUIShowEmptyRows) {
		int numberOfRowsToShow = static_cast<int>(std::ceil(static_cast<float>(std::min(c_FullViewPageItemLimit, m_InventoryActor->GetInventorySize())) / static_cast<float>(c_ItemsPerRow)));
		int expectedInventoryHeight = m_GUIInventoryItemButtons[0].second->GetHeight() * numberOfRowsToShow;
		if (numberOfRowsToShow * c_ItemsPerRow < c_FullViewPageItemLimit) {
			expectedInventoryHeight -= 1;
		}
		if (m_GUIInventoryItemsBox->GetHeight() != expectedInventoryHeight) {
			int inventoryItemsBoxPreviousHeight = m_GUIInventoryItemsBox->GetHeight();
			m_GUIInventoryItemsBox->SetSize(m_GUIInventoryItemsBox->GetWidth(), expectedInventoryHeight);
			m_GUITopLevelBoxFullSize.SetY(m_GUITopLevelBoxFullSize.GetY() + static_cast<float>(expectedInventoryHeight - inventoryItemsBoxPreviousHeight));
			m_GUITopLevelBox->SetSize(m_GUITopLevelBoxFullSize.GetFloorIntX(), m_GUITopLevelBoxFullSize.GetFloorIntY());
		}
	}

	UpdateFullModeEquippedItemButtons();

	const std::deque<MovableObject*>* inventory = m_InventoryActor->GetInventory();

	UpdateFullModeScrollbar(inventory);

	UpdateFullModeInventoryItemButtons(inventory);

	m_GUIControlManager->Update(true);

	if (!m_GUIDisplayOnly) {
		HandleInput();
		if (!IsEnabled()) {
			return;
		}
	}

	m_GUIInformationToggleButton->SetEnabled(true);

	if (m_GUISelectedItem) {
		m_GUISelectedItem->Button->OnGainFocus();
		if (m_GUISelectedItem->DragWasHeldForLongEnough()) {
			m_GUIInformationToggleButton->SetEnabled(false);
		}
		if (const HDFirearm* selectedItemAsFirearm = dynamic_cast<HDFirearm*>(m_GUISelectedItem->Object)) {
			m_GUIReloadButton->SetEnabled(!selectedItemAsFirearm->IsFull());
		} else {
			m_GUIReloadButton->SetEnabled(false);
		}
		m_GUIDropButton->SetEnabled(true);
	} else {
		m_GUIReloadButton->SetEnabled(false);
		for (const auto [equippedItem, offhandEquippedItem]: m_InventoryActorEquippedItems) {
			const HDFirearm* equippedItemAsFirearm = dynamic_cast<const HDFirearm*>(equippedItem);
			if (equippedItemAsFirearm && !equippedItemAsFirearm->IsFull()) {
				m_GUIReloadButton->SetEnabled(true);
				break;
			}
			const HDFirearm* offhandEquippedItemAsFirearm = dynamic_cast<const HDFirearm*>(offhandEquippedItem);
			if (offhandEquippedItemAsFirearm && !offhandEquippedItemAsFirearm->IsFull()) {
				m_GUIReloadButton->SetEnabled(true);
				break;
			}
		}
		m_GUIDropButton->SetEnabled(false);
	}

	UpdateFullModeInformationText(inventory);

	UpdateFullModeNonItemButtonIconsAndHighlightWidths();
}

void InventoryMenuGUI::UpdateFullModeEquippedItemButtons() {
	const MovableObject* equippedItem = m_GUIInventoryActorCurrentEquipmentSetIndex < m_InventoryActorEquippedItems.size() ? m_InventoryActorEquippedItems.at(m_GUIInventoryActorCurrentEquipmentSetIndex).first : nullptr;
	const MovableObject* offhandEquippedItem = m_GUIInventoryActorCurrentEquipmentSetIndex < m_InventoryActorEquippedItems.size() ? m_InventoryActorEquippedItems.at(m_GUIInventoryActorCurrentEquipmentSetIndex).second : nullptr;

	m_GUIEquippedItemButton->SetEnabled(equippedItem);
	if (m_GUISelectedItem && m_GUISelectedItem->Button == m_GUIEquippedItemButton && m_GUISelectedItem->DragWasHeldForLongEnough()) {
		m_GUIEquippedItemButton->SetIconAndText(nullptr, ">>>");
	} else if (equippedItem) {
		m_GUIEquippedItemButton->SetIconAndText(equippedItem->GetGraphicalIcon(), equippedItem->GetPresetName());
	} else {
		m_GUIEquippedItemButton->SetEnabled(m_GUISelectedItem != nullptr);
		m_GUIEquippedItemButton->SetIconAndText(nullptr, "> <");
	}

	m_GUIOffhandEquippedItemButton->SetEnabled(offhandEquippedItem);
	if (m_GUISelectedItem && m_GUISelectedItem->Button == m_GUIOffhandEquippedItemButton && m_GUISelectedItem->DragWasHeldForLongEnough()) {
		m_GUIOffhandEquippedItemButton->SetIconAndText(nullptr, ">>>");
	} else if (offhandEquippedItem) {
		m_GUIOffhandEquippedItemButton->SetIconAndText(offhandEquippedItem->GetGraphicalIcon(), offhandEquippedItem->GetPresetName());
	} else {
		m_GUIOffhandEquippedItemButton->SetEnabled(m_GUISelectedItem != nullptr);
		m_GUIOffhandEquippedItemButton->SetIconAndText(nullptr, "> <");
	}

	bool showOffhandButton = offhandEquippedItem || (dynamic_cast<const HeldDevice*>(equippedItem) && dynamic_cast<const HeldDevice*>(equippedItem)->IsOneHanded());
	if (showOffhandButton && !m_GUIOffhandEquippedItemButton->GetVisible()) {
		m_GUIEquippedItemButton->Resize(m_GUIEquippedItemButton->GetWidth() / 2, m_GUIEquippedItemButton->GetHeight());
		m_GUIOffhandEquippedItemButton->SetVisible(true);
	} else if (!showOffhandButton && m_GUIOffhandEquippedItemButton->GetVisible()) {
		m_GUIEquippedItemButton->Resize(m_GUIEquippedItemButton->GetWidth() * 2, m_GUIEquippedItemButton->GetHeight());
		m_GUIOffhandEquippedItemButton->SetVisible(false);
	}
}

void InventoryMenuGUI::UpdateFullModeScrollbar(const std::deque<MovableObject*>* inventory) {
	if (inventory->size() > c_FullViewPageItemLimit) {
		m_GUIInventoryItemsScrollbar->SetMaximum(static_cast<int>(std::ceil(static_cast<float>(inventory->size() - c_FullViewPageItemLimit) / static_cast<float>(c_ItemsPerRow))) + 1);
		if (!m_GUIInventoryItemsScrollbar->GetVisible()) {
			m_GUIInventoryItemsScrollbar->SetVisible(true);
			m_GUIInventoryItemsScrollbar->SetEnabled(true);

			m_GUITopLevelBoxFullSize.SetX(m_GUITopLevelBoxFullSize.GetX() + static_cast<float>(m_GUIInventoryItemsScrollbar->GetWidth()));
			m_GUITopLevelBox->SetSize(m_GUITopLevelBoxFullSize.GetFloorIntX(), m_GUITopLevelBoxFullSize.GetFloorIntY());
			m_GUIEquippedItemsBox->SetSize(m_GUIEquippedItemsBox->GetWidth() + m_GUIInventoryItemsScrollbar->GetWidth(), m_GUIEquippedItemsBox->GetHeight());
			m_GUIInventoryItemsBox->SetSize(m_GUIInventoryItemsBox->GetWidth() + m_GUIInventoryItemsScrollbar->GetWidth(), m_GUIInventoryItemsBox->GetHeight());
			m_GUIInformationToggleButton->SetPositionAbs(m_GUIInformationToggleButton->GetXPos() + m_GUIInventoryItemsScrollbar->GetWidth(), m_GUIInformationToggleButton->GetYPos());
		}
	} else if (m_GUIInventoryItemsScrollbar->GetVisible() && inventory->size() <= c_FullViewPageItemLimit) {
		m_GUIInventoryItemsScrollbar->SetValue(0);
		m_GUIInventoryItemsScrollbar->SetMaximum(1);
		m_GUIInventoryItemsScrollbar->SetVisible(false);
		m_GUIInventoryItemsScrollbar->SetEnabled(false);

		m_GUITopLevelBoxFullSize.SetX(m_GUITopLevelBoxFullSize.GetX() - static_cast<float>(m_GUIInventoryItemsScrollbar->GetWidth()));
		m_GUITopLevelBox->SetSize(m_GUITopLevelBoxFullSize.GetFloorIntX(), m_GUITopLevelBoxFullSize.GetFloorIntY());
		m_GUIEquippedItemsBox->SetSize(m_GUIEquippedItemsBox->GetWidth() - m_GUIInventoryItemsScrollbar->GetWidth(), m_GUIEquippedItemsBox->GetHeight());
		m_GUIInventoryItemsBox->SetSize(m_GUIInventoryItemsBox->GetWidth() - m_GUIInventoryItemsScrollbar->GetWidth(), m_GUIInventoryItemsBox->GetHeight());
		m_GUIInformationToggleButton->SetPositionAbs(m_GUIInformationToggleButton->GetXPos() - m_GUIInventoryItemsScrollbar->GetWidth(), m_GUIInformationToggleButton->GetYPos());
	}
}

void InventoryMenuGUI::UpdateFullModeInventoryItemButtons(const std::deque<MovableObject*>* inventory) {
	int startIndex = m_GUIInventoryItemsScrollbar->GetValue() * c_ItemsPerRow;
	int lastPopulatedIndex = static_cast<int>(inventory->size() - 1);
	GUIButton* itemButton;
	MovableObject* inventoryItem;
	for (int i = startIndex; (i - startIndex) < c_FullViewPageItemLimit; i++) {
		itemButton = m_GUIInventoryItemButtons.at(i - startIndex).second;
		if (i <= lastPopulatedIndex) {
			inventoryItem = inventory->at(i);
			itemButton->SetEnabled(true);
			m_GUIInventoryItemButtons.at(i - startIndex).first = inventoryItem;
			if (m_GUISelectedItem && m_GUISelectedItem->Button == itemButton && m_GUISelectedItem->DragWasHeldForLongEnough()) {
				itemButton->SetIconAndText(nullptr, ">>>");
			} else {
				itemButton->SetIconAndText(inventoryItem->GetGraphicalIcon(), inventoryItem->GetPresetName());
			}
		} else if (i > lastPopulatedIndex) {
			if (!m_GUIShowEmptyRows && inventory->size() < c_FullViewPageItemLimit && ((i - startIndex) >= (inventory->size() + c_ItemsPerRow - (inventory->size() % c_ItemsPerRow)))) {
				break;
			}
			if (m_GUIInventoryItemButtons.at(i - startIndex).first) {
				m_GUIInventoryItemButtons.at(i - startIndex).first = nullptr;
				itemButton->SetIconAndText(nullptr, "> <");
			}
			itemButton->SetEnabled(m_GUISelectedItem != nullptr);
		}
	}
}

void InventoryMenuGUI::UpdateFullModeInformationText(const std::deque<MovableObject*>* inventory) {
	if (!m_GUIShowInformationText && m_GUIInformationText->GetVisible()) {
		m_GUIInformationText->SetVisible(false);
	} else if (m_GUIShowInformationText) {
		if (!m_GUIInformationText->GetVisible()) {
			m_GUIInformationText->SetVisible(true);
		}

		std::string informationText;
		if (m_GUISelectedItem) {
			if (m_GUISelectedItem->DragWasHeldForLongEnough()) {
				informationText = "Drag this item to another one to swap places with it, ";
				informationText += m_GUIReloadButton->GetEnabled() ? "the reload button to reload it, " : "";
				informationText += "or the drop button to drop it. Drag it out of the inventory window to toss it.";
			} else {
				informationText = m_MenuController->IsMouseControlled() ? "Click another item to swap places with it, " : "Press another item to swap places with it, ";
				informationText += m_GUIReloadButton->GetEnabled() ? "the reload button to reload it, " : "";
				informationText += m_MenuController->IsMouseControlled() ? "or the drop button " : "or press the drop button/use the drop key ";
				informationText += "to drop it.";
			}
		} else {
			if (m_GUIDisplayOnly) {
				informationText = ">> DISPLAY ONLY <<";
			} else if (m_InventoryActorEquippedItems.empty() && inventory->empty()) {
				informationText = "No items to display.";
			} else {
				informationText = m_MenuController->IsMouseControlled() ? "Click an item to interact with it. You can also click and hold to drag items." : "Press an item to interact with it.";
			}
		}
		m_GUIInformationText->SetText(informationText);
	}
}

void InventoryMenuGUI::UpdateFullModeNonItemButtonIconsAndHighlightWidths() {
	std::vector<std::pair<GUIButton*, const Icon*>> buttonsToCheckIconsFor = {
	    {m_GUIInformationToggleButton, m_GUIInformationToggleButtonIcon},
	    {m_GUIReloadButton, m_GUIReloadButtonIcon},
	    {m_GUIDropButton, m_GUIDropButtonIcon}};

	for (const auto& [button, icon]: buttonsToCheckIconsFor) {
		if (icon) {
			if (button->IsEnabled()) {
				button->SetIcon((button->HasFocus() || button->IsMousedOver() || button->IsPushed()) ? icon->GetBitmaps8()[1] : icon->GetBitmaps8()[0]);
			} else {
				button->SetIcon(icon->GetBitmaps8()[2]);
			}
		}

		if (!button->IsEnabled() && button->GetWidth() == 15 && (button->HasFocus() || button->IsMousedOver() || button->IsPushed())) {
			button->SetPositionAbs(button->GetXPos() - 1, button->GetYPos());
			button->Resize(button->GetWidth() + 2, button->GetHeight());
		} else if (button->GetWidth() != 15 && (button->IsEnabled() || !(button->HasFocus() || button->IsMousedOver() || button->IsPushed()))) {
			button->SetPositionAbs(button->GetXPos() + 1, button->GetYPos());
			button->Resize(button->GetWidth() - 2, button->GetHeight());
		}
	}
}

void InventoryMenuGUI::UpdateTransferMode() {
	// TODO Make Transfer mode
}

void InventoryMenuGUI::HandleInput() {
	if (m_MenuController->IsState(ControlState::PRESS_SECONDARY)) {
		SetEnabled(false);
		return;
	}

	// TODO Maybe add support for other click types, e.g. if player right clicks on an item when they have one selected, we can call some custom lua function, so you can do custom stuff like combining items or whatever
	if (m_MenuController->IsMouseControlled()) {
		if (HandleMouseInput()) {
			return;
		}
	} else {
		HandleNonMouseInput();
	}

	if (m_GUISelectedItem && m_MenuController->IsState(ControlState::WEAPON_DROP)) {
		DropSelectedItem();
	}

	GUIEvent guiEvent;
	const GUIControl* guiControl;
	while (m_GUIControlManager->GetEvent(&guiEvent)) {
		guiControl = guiEvent.GetControl();
		if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUIButton::Focused) {
			g_GUISound.SelectionChangeSound()->Play(m_MenuController->GetPlayer());
			break;
		}
		bool buttonHeld = guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUIButton::Pushed;
		if (buttonHeld || guiEvent.GetType() == GUIEvent::Command) {
			if (!buttonHeld && guiControl == m_GUIInformationToggleButton) {
				m_GUIShowInformationText = !m_GUIShowInformationText;
				g_GUISound.ItemChangeSound()->Play(m_MenuController->GetPlayer());
				m_GUIInformationToggleButton->OnLoseFocus();
			} else if (guiControl == m_GUIEquippedItemButton) {
				int equippedItemButtonIndexToUse = 0;
				if (const AHuman* inventoryActorAsAHuman = (m_InventoryActorIsHuman ? dynamic_cast<AHuman*>(m_InventoryActor) : nullptr); inventoryActorAsAHuman && !inventoryActorAsAHuman->GetFGArm() && !inventoryActorAsAHuman->GetEquippedBGItem()) {
					equippedItemButtonIndexToUse = 1;
				}
				HandleItemButtonPressOrHold(m_GUIEquippedItemButton, m_InventoryActorEquippedItems.empty() ? nullptr : m_InventoryActorEquippedItems.at(m_GUIInventoryActorCurrentEquipmentSetIndex).first, equippedItemButtonIndexToUse, buttonHeld);
			} else if (guiControl == m_GUIOffhandEquippedItemButton) {
				HandleItemButtonPressOrHold(m_GUIOffhandEquippedItemButton, m_InventoryActorEquippedItems.empty() ? nullptr : m_InventoryActorEquippedItems.at(m_GUIInventoryActorCurrentEquipmentSetIndex).second, 1, buttonHeld);
				m_GUIOffhandEquippedItemButton->OnMouseLeave(0, 0, 0, 0);
			} else if (!buttonHeld && guiControl == m_GUIReloadButton) {
				ReloadSelectedItem();
			} else if (!buttonHeld && guiControl == m_GUIDropButton) {
				DropSelectedItem();
			} else {
				for (const auto& [inventoryObject, inventoryItemButton]: m_GUIInventoryItemButtons) {
					if (guiControl == inventoryItemButton) {
						HandleItemButtonPressOrHold(inventoryItemButton, inventoryObject, -1, buttonHeld);
					}
				}
			}
		}
	}
}

bool InventoryMenuGUI::HandleMouseInput() {
	int mouseX;
	int mouseY;
	m_GUIInput->GetMousePosition(&mouseX, &mouseY);
	m_PreviousGUICursorPos.SetXY(m_GUICursorPos.GetX(), m_GUICursorPos.GetY());
	m_GUICursorPos.SetXY(static_cast<float>(mouseX), static_cast<float>(mouseY));

	if (m_GUIInventoryItemsScrollbar->GetVisible()) {
		int mouseWheelChange = m_MenuController->IsState(ControlState::SCROLL_UP) ? -1 : (m_MenuController->IsState(ControlState::SCROLL_DOWN) ? 1 : 0);
		if (mouseWheelChange != 0) {
			m_GUIInventoryItemsScrollbar->SetValue(std::clamp(m_GUIInventoryItemsScrollbar->GetValue() + mouseWheelChange, m_GUIInventoryItemsScrollbar->GetMinimum(), m_GUIInventoryItemsScrollbar->GetMaximum()));
		}
	}

	if (m_GUISelectedItem && m_GUISelectedItem->IsBeingDragged) {
		if (!m_GUISelectedItem->DragWasHeldForLongEnough()) {
			m_GUISelectedItem->DragHoldCount++;
			if (m_GUISelectedItem->DragWasHeldForLongEnough()) {
				g_GUISound.ItemChangeSound()->Play(m_MenuController->GetPlayer());
			}
		}

		if (m_GUIEquippedItemButton->IsPushed() && !m_GUIEquippedItemButton->PointInside(mouseX, mouseY)) {
			m_GUIEquippedItemButton->SetPushed(false);
		}
		if (m_GUIOffhandEquippedItemButton->IsPushed() && !m_GUIOffhandEquippedItemButton->PointInside(mouseX, mouseY)) {
			m_GUIOffhandEquippedItemButton->SetPushed(false);
		}
		if (m_GUIReloadButton->IsPushed() && !m_GUIReloadButton->PointInside(mouseX, mouseY)) {
			m_GUIReloadButton->SetPushed(false);
		}
		if (m_GUIDropButton->IsPushed() && !m_GUIDropButton->PointInside(mouseX, mouseY)) {
			m_GUIDropButton->SetPushed(false);
		}
		for (const auto& [unused, inventoryItemButton]: m_GUIInventoryItemButtons) {
			if (inventoryItemButton->IsPushed() && !inventoryItemButton->PointInside(mouseX, mouseY)) {
				inventoryItemButton->SetPushed(false);
			}
		}

		int mouseEvents[3];
		int mouseStates[3];
		m_GUIInput->GetMouseButtons(mouseEvents, mouseStates);
		bool mouseHeld = mouseEvents[0] == GUIInput::Repeat || mouseStates[0] == GUIInput::Down;
		bool mouseReleased = mouseEvents[0] == GUIInput::Released || mouseStates[0] == GUIInput::Up;

		if (mouseReleased && !m_GUISelectedItem->DragWasHeldForLongEnough()) {
			ClearSelectedItem();
			return false;
		}

		if (mouseReleased && !m_GUITopLevelBox->PointInside(mouseX, mouseY)) {
			Vector cursorPosDifference = (m_GUICursorPos - m_PreviousGUICursorPos) * 2.0F;
			DropSelectedItem(&cursorPosDifference.CapMagnitude(25));
		} else {
			if (m_GUIEquippedItemButton->PointInside(mouseX, mouseY)) {
				if (mouseHeld && !m_GUIEquippedItemButton->IsPushed()) {
					m_GUIEquippedItemButton->SetPushed(true);
					g_GUISound.SelectionChangeSound()->Play(m_MenuController->GetPlayer());
				} else if (mouseReleased) {
					int equippedItemButtonIndexToUse = 0;
					if (const AHuman* inventoryActorAsAHuman = (m_InventoryActorIsHuman ? dynamic_cast<AHuman*>(m_InventoryActor) : nullptr); inventoryActorAsAHuman && !inventoryActorAsAHuman->GetFGArm() && !inventoryActorAsAHuman->GetEquippedBGItem()) {
						equippedItemButtonIndexToUse = 1;
					}
					HandleItemButtonPressOrHold(m_GUIEquippedItemButton, m_InventoryActorEquippedItems.empty() ? nullptr : m_InventoryActorEquippedItems.at(m_GUIInventoryActorCurrentEquipmentSetIndex).first, equippedItemButtonIndexToUse);
					m_GUIEquippedItemButton->SetPushed(false);
					if (!m_GUISelectedItem) {
						return true;
					}
				}
			} else if (m_GUIOffhandEquippedItemButton->PointInside(mouseX, mouseY)) {
				if (mouseHeld && !m_GUIOffhandEquippedItemButton->IsPushed()) {
					m_GUIOffhandEquippedItemButton->SetPushed(true);
					g_GUISound.SelectionChangeSound()->Play(m_MenuController->GetPlayer());
				} else if (mouseReleased) {
					HandleItemButtonPressOrHold(m_GUIOffhandEquippedItemButton, m_InventoryActorEquippedItems.empty() ? nullptr : m_InventoryActorEquippedItems.at(m_GUIInventoryActorCurrentEquipmentSetIndex).second, 1);
					m_GUIOffhandEquippedItemButton->SetPushed(false);
					if (!m_GUISelectedItem) {
						return true;
					}
				}
			} else if (m_GUIReloadButton->IsEnabled() && m_GUIReloadButton->PointInside(mouseX, mouseY)) {
				if (mouseHeld && !m_GUIReloadButton->IsPushed()) {
					m_GUIReloadButton->SetPushed(true);
					g_GUISound.SelectionChangeSound()->Play(m_MenuController->GetPlayer());
				} else if (mouseReleased) {
					ReloadSelectedItem();
					m_GUIReloadButton->SetPushed(false);
				}
			} else if (m_GUIDropButton->IsEnabled() && m_GUIDropButton->PointInside(mouseX, mouseY)) {
				if (mouseHeld && !m_GUIDropButton->IsPushed()) {
					m_GUIDropButton->SetPushed(true);
					g_GUISound.SelectionChangeSound()->Play(m_MenuController->GetPlayer());
				} else if (mouseReleased) {
					DropSelectedItem();
					m_GUIDropButton->SetPushed(false);
				}
			} else {
				for (const auto& [inventoryObject, inventoryItemButton]: m_GUIInventoryItemButtons) {
					if (inventoryItemButton->PointInside(mouseX, mouseY)) {
						if (mouseHeld && !inventoryItemButton->IsPushed()) {
							inventoryItemButton->SetPushed(true);
							g_GUISound.SelectionChangeSound()->Play(m_MenuController->GetPlayer());
						} else if (mouseReleased) {
							HandleItemButtonPressOrHold(inventoryItemButton, inventoryObject, -1);
							inventoryItemButton->SetPushed(false);
							if (!m_GUISelectedItem) {
								return true;
							}
							break;
						}
					}
				}
			}
		}
		if (mouseReleased) {
			bool selectedItemWasHeld = m_GUISelectedItem && m_GUISelectedItem->DragWasHeldForLongEnough();
			ClearSelectedItem();
			return selectedItemWasHeld;
		}
	}
	return false;
}

void InventoryMenuGUI::HandleNonMouseInput() {
	if (!m_NonMouseHighlightedButton || !m_NonMouseHighlightedButton->GetVisible() || (!m_GUIShowEmptyRows && m_NonMouseHighlightedButton->GetParent() == m_GUIInventoryItemsBox && m_InventoryActor->IsInventoryEmpty())) {
		m_NonMouseHighlightedButton = m_GUIEquippedItemButton;
	}
	if (!m_NonMouseHighlightedButton->IsMousedOver()) {
		m_NonMouseHighlightedButton->OnMouseEnter(0, 0, 0, 0);
	}

	if (m_MenuController->IsState(ControlState::PRESS_PRIMARY)) {
		if (m_NonMouseHighlightedButton->IsEnabled()) {
			m_NonMouseHighlightedButton->SetCaptureState(true);
			m_NonMouseHighlightedButton->OnMouseUp(m_NonMouseHighlightedButton->GetXPos(), m_NonMouseHighlightedButton->GetYPos(), GUIInput::Released, 0);
			m_NonMouseHighlightedButton->SetCaptureState(false);
			return;
		} else {
			g_GUISound.UserErrorSound()->Play(m_MenuController->GetPlayer());
		}
	}

	GUIButton* nextButtonToHighlight = nullptr;
	Directions pressedDirection = GetNonMouseButtonControllerMovement();
	switch (pressedDirection) {
		case Directions::Up:
			nextButtonToHighlight = HandleNonMouseUpInput();
			break;
		case Directions::Down:
			nextButtonToHighlight = HandleNonMouseDownInput();
			break;
		case Directions::Left:
			nextButtonToHighlight = HandleNonMouseLeftInput();
			break;
		case Directions::Right:
			nextButtonToHighlight = HandleNonMouseRightInput();
			break;
		default:
			break;
	}

	if (nextButtonToHighlight && m_NonMouseHighlightedButton != nextButtonToHighlight && !nextButtonToHighlight->IsMousedOver()) {
		if (m_NonMouseHighlightedButton->GetParent() == m_GUIEquippedItemsBox) {
			m_NonMousePreviousEquippedItemsBoxButton = m_NonMouseHighlightedButton;
		} else if (m_NonMouseHighlightedButton->GetParent() == m_GUIInventoryItemsBox) {
			m_NonMousePreviousInventoryItemsBoxButton = m_NonMouseHighlightedButton;
		}
		m_NonMouseHighlightedButton->OnMouseLeave(0, 0, 0, 0);
		m_NonMouseHighlightedButton = nextButtonToHighlight;
		m_NonMouseHighlightedButton->OnMouseEnter(0, 0, 0, 0);
	} else if (!nextButtonToHighlight && pressedDirection != Directions::None) {
		g_GUISound.UserErrorSound()->Play(m_MenuController->GetPlayer());
	}
}

Directions InventoryMenuGUI::GetNonMouseButtonControllerMovement() {
	bool pressUp = m_MenuController->IsState(ControlState::PRESS_UP) || m_MenuController->IsState(ControlState::SCROLL_UP);
	bool pressDown = m_MenuController->IsState(ControlState::PRESS_DOWN) || m_MenuController->IsState(ControlState::SCROLL_DOWN);
	bool pressLeft = m_MenuController->IsState(ControlState::PRESS_LEFT);
	bool pressRight = m_MenuController->IsState(ControlState::PRESS_RIGHT);
	if (!(m_MenuController->IsState(ControlState::MOVE_UP) || m_MenuController->IsState(ControlState::MOVE_DOWN) || m_MenuController->IsState(ControlState::MOVE_LEFT) || m_MenuController->IsState(ControlState::MOVE_RIGHT))) {
		m_GUIRepeatStartTimer.Reset();
		m_GUIRepeatTimer.Reset();
	}
	if (m_GUIRepeatStartTimer.IsPastRealMS(200) && m_GUIRepeatTimer.IsPastRealMS(70)) {
		if (m_MenuController->IsState(ControlState::MOVE_UP)) {
			pressUp = true;
		} else if (m_MenuController->IsState(ControlState::MOVE_DOWN)) {
			pressDown = true;
		} else if (m_MenuController->IsState(ControlState::MOVE_LEFT)) {
			pressLeft = true;
		} else if (m_MenuController->IsState(ControlState::MOVE_RIGHT)) {
			pressRight = true;
		}
		m_GUIRepeatTimer.Reset();
	}
	if (pressUp) {
		return Directions::Up;
	} else if (pressDown) {
		return Directions::Down;
	} else if (pressLeft) {
		return Directions::Left;
	} else if (pressRight) {
		return Directions::Right;
	}
	return Directions::None;
}

GUIButton* InventoryMenuGUI::HandleNonMouseUpInput() {
	GUIButton* nextButtonToHighlight = nullptr;

	if (m_NonMouseHighlightedButton == m_GUIDropButton) {
		nextButtonToHighlight = m_GUIReloadButton;
		m_NonMousePreviousReloadOrDropButton = m_GUIReloadButton;
	} else if (m_NonMouseHighlightedButton->GetParent() == m_GUIInventoryItemsBox) {
		try {
			int highlightedButtonIndex = std::stoi(m_NonMouseHighlightedButton->GetName());
			if (highlightedButtonIndex < c_ItemsPerRow) {
				if (m_GUIInventoryItemsScrollbar->GetValue() > m_GUIInventoryItemsScrollbar->GetMinimum()) {
					m_GUIInventoryItemsScrollbar->SetValue(m_GUIInventoryItemsScrollbar->GetValue() - 1);
					nextButtonToHighlight = m_NonMouseHighlightedButton;
				} else {
					nextButtonToHighlight = m_NonMousePreviousEquippedItemsBoxButton && m_NonMousePreviousEquippedItemsBoxButton->GetVisible() ? m_NonMousePreviousEquippedItemsBoxButton : nullptr;
					if (!nextButtonToHighlight) {
						if (highlightedButtonIndex >= 4) {
							nextButtonToHighlight = m_GUIDropButton;
						} else {
							nextButtonToHighlight = highlightedButtonIndex >= 2 && m_GUIOffhandEquippedItemButton->GetVisible() ? m_GUIOffhandEquippedItemButton : m_GUIEquippedItemButton;
						}
					}
				}
			} else {
				nextButtonToHighlight = m_GUIInventoryItemButtons.at(highlightedButtonIndex - c_ItemsPerRow).second;
			}
		} catch (std::invalid_argument) {
			RTEAbort("Invalid inventory item button when pressing UP in InventoryMenuGUI keyboard/controller handling - " + m_NonMouseHighlightedButton->GetName());
		}
	}
	return nextButtonToHighlight;
}

GUIButton* InventoryMenuGUI::HandleNonMouseDownInput() {
	GUIButton* nextButtonToHighlight = nullptr;

	if (!m_InventoryActor->IsInventoryEmpty() && (m_NonMouseHighlightedButton == m_GUISwapSetButton || m_NonMouseHighlightedButton == m_GUIEquippedItemButton || m_NonMouseHighlightedButton == m_GUIOffhandEquippedItemButton || m_NonMouseHighlightedButton == m_GUIDropButton || m_NonMouseHighlightedButton == m_GUIInformationToggleButton)) {
		nextButtonToHighlight = m_NonMousePreviousInventoryItemsBoxButton;
		if (!nextButtonToHighlight) {
			int inventoryIndexToHighlight = m_GUIOffhandEquippedItemButton->GetVisible() ? 1 : 2;
			if (m_NonMouseHighlightedButton == m_GUIOffhandEquippedItemButton) {
				inventoryIndexToHighlight = 2;
			} else if (m_NonMouseHighlightedButton == m_GUIDropButton || m_NonMouseHighlightedButton == m_GUIInformationToggleButton) {
				inventoryIndexToHighlight = 4;
			}
			inventoryIndexToHighlight = std::clamp(inventoryIndexToHighlight, 0, m_InventoryActor->GetInventorySize() - 1);
			nextButtonToHighlight = m_GUIInventoryItemButtons.at(inventoryIndexToHighlight).second;
		}
	} else if (m_NonMouseHighlightedButton == m_GUIReloadButton) {
		nextButtonToHighlight = m_GUIDropButton;
		m_NonMousePreviousReloadOrDropButton = m_GUIDropButton;
	} else if (m_NonMouseHighlightedButton->GetParent() == m_GUIInventoryItemsBox) {
		try {
			int highlightedButtonIndex = std::stoi(m_NonMouseHighlightedButton->GetName());
			if (highlightedButtonIndex + c_ItemsPerRow >= c_FullViewPageItemLimit && m_GUIInventoryItemsScrollbar->GetValue() + 1 < m_GUIInventoryItemsScrollbar->GetMaximum()) {
				m_GUIInventoryItemsScrollbar->SetValue(m_GUIInventoryItemsScrollbar->GetValue() + 1);
				nextButtonToHighlight = m_NonMouseHighlightedButton;
			} else {
				int numberOfVisibleButtons = m_GUIShowEmptyRows ? c_FullViewPageItemLimit : c_ItemsPerRow * static_cast<int>(std::ceil(static_cast<float>(std::min(c_FullViewPageItemLimit, m_InventoryActor->GetInventorySize())) / static_cast<float>(c_ItemsPerRow)));
				if (highlightedButtonIndex + c_ItemsPerRow < numberOfVisibleButtons) {
					nextButtonToHighlight = m_GUIInventoryItemButtons.at(highlightedButtonIndex + c_ItemsPerRow).second;
				}
			}
		} catch (std::invalid_argument) {
			RTEAbort("Invalid inventory item button when pressing DOWN in InventoryMenuGUI keyboard/controller handling - " + m_NonMouseHighlightedButton->GetName());
		}
	}
	return nextButtonToHighlight;
}

GUIButton* InventoryMenuGUI::HandleNonMouseLeftInput() {
	GUIButton* nextButtonToHighlight = nullptr;

	if (m_NonMouseHighlightedButton->GetParent() == m_GUIInventoryItemsBox) {
		try {
			int highlightedButtonIndex = std::stoi(m_NonMouseHighlightedButton->GetName());
			if (highlightedButtonIndex == 0 && m_GUIInventoryItemsScrollbar->GetValue() > m_GUIInventoryItemsScrollbar->GetMinimum()) {
				m_GUIInventoryItemsScrollbar->SetValue(m_GUIInventoryItemsScrollbar->GetValue() - 1);
				nextButtonToHighlight = m_GUIInventoryItemButtons.at(c_ItemsPerRow - 1).second;
			} else if (highlightedButtonIndex > 0) {
				nextButtonToHighlight = m_GUIInventoryItemButtons.at(highlightedButtonIndex - 1).second;
				m_NonMousePreviousEquippedItemsBoxButton = nullptr;
			}
		} catch (std::invalid_argument) {
			RTEAbort("Invalid inventory item button when pressing LEFT in InventoryMenuGUI keyboard/controller handling - " + m_NonMouseHighlightedButton->GetName());
		}
	} else {
		if (m_NonMouseHighlightedButton == m_GUIOffhandEquippedItemButton) {
			nextButtonToHighlight = m_GUIEquippedItemButton;
		} else if (m_NonMouseHighlightedButton == m_GUIReloadButton || m_NonMouseHighlightedButton == m_GUIDropButton) {
			nextButtonToHighlight = m_GUIOffhandEquippedItemButton->GetVisible() ? m_GUIOffhandEquippedItemButton : m_GUIEquippedItemButton;
		} else if (m_NonMouseHighlightedButton == m_GUIInformationToggleButton) {
			nextButtonToHighlight = m_NonMousePreviousReloadOrDropButton ? m_NonMousePreviousReloadOrDropButton : m_GUIReloadButton;
		}
		if (nextButtonToHighlight) {
			m_NonMousePreviousInventoryItemsBoxButton = nullptr;
		}
	}
	return nextButtonToHighlight;
}

GUIButton* InventoryMenuGUI::HandleNonMouseRightInput() {
	GUIButton* nextButtonToHighlight = nullptr;

	if (m_NonMouseHighlightedButton->GetParent() == m_GUIInventoryItemsBox) {
		try {
			int highlightedButtonIndex = std::stoi(m_NonMouseHighlightedButton->GetName());
			if (highlightedButtonIndex == c_FullViewPageItemLimit - 1 && m_GUIInventoryItemsScrollbar->GetValue() + 1 < m_GUIInventoryItemsScrollbar->GetMaximum()) {
				m_GUIInventoryItemsScrollbar->SetValue(m_GUIInventoryItemsScrollbar->GetValue() + 1);
				nextButtonToHighlight = m_GUIInventoryItemButtons.at(highlightedButtonIndex - c_ItemsPerRow + 1).second;
			} else {
				int numberOfVisibleButtons = m_GUIShowEmptyRows ? c_FullViewPageItemLimit : c_ItemsPerRow * static_cast<int>(std::ceil(static_cast<float>(std::min(c_FullViewPageItemLimit, m_InventoryActor->GetInventorySize())) / static_cast<float>(c_ItemsPerRow)));
				if (highlightedButtonIndex + 1 < numberOfVisibleButtons) {
					nextButtonToHighlight = m_GUIInventoryItemButtons.at(highlightedButtonIndex + 1).second;
				}
				m_NonMousePreviousEquippedItemsBoxButton = nullptr;
			}
		} catch (std::invalid_argument) {
			RTEAbort("Invalid inventory item button when pressing RIGHT in InventoryMenuGUI keyboard/controller handling - " + m_NonMouseHighlightedButton->GetName());
		}
	} else {
		if (m_NonMouseHighlightedButton == m_GUIEquippedItemButton) {
			if (m_GUIOffhandEquippedItemButton->GetVisible()) {
				nextButtonToHighlight = m_GUIOffhandEquippedItemButton;
			} else {
				nextButtonToHighlight = m_NonMousePreviousReloadOrDropButton ? m_NonMousePreviousReloadOrDropButton : m_GUIReloadButton;
			}
		} else if (m_NonMouseHighlightedButton == m_GUIOffhandEquippedItemButton) {
			nextButtonToHighlight = m_NonMousePreviousReloadOrDropButton ? m_NonMousePreviousReloadOrDropButton : m_GUIReloadButton;
		} else if (m_NonMouseHighlightedButton == m_GUIReloadButton || m_NonMouseHighlightedButton == m_GUIDropButton) {
			nextButtonToHighlight = m_GUIInformationToggleButton;
		}
		if (nextButtonToHighlight) {
			m_NonMousePreviousInventoryItemsBoxButton = nullptr;
		}
	}
	return nextButtonToHighlight;
}

void InventoryMenuGUI::HandleItemButtonPressOrHold(GUIButton* pressedButton, MovableObject* buttonObject, int buttonEquippedItemIndex, bool buttonHeld) {
	if (buttonHeld && m_GUISelectedItem) {
		return;
	}
	if ((buttonEquippedItemIndex == 1 && m_GUISelectedItem && !m_GUISelectedItem->Object->HasObjectInGroup("Shields") && !dynamic_cast<HeldDevice*>(m_GUISelectedItem->Object)->IsDualWieldable()) || (m_GUISelectedItem && m_GUISelectedItem->EquippedItemIndex == 1 && buttonObject && !buttonObject->HasObjectInGroup("Shields") && !dynamic_cast<HeldDevice*>(buttonObject)->IsDualWieldable())) {
		g_GUISound.UserErrorSound()->Play(m_MenuController->GetPlayer());
		return;
	}

	int pressedButtonItemIndex = buttonEquippedItemIndex;
	if (pressedButtonItemIndex == -1) {
		try {
			pressedButtonItemIndex = std::stoi(pressedButton->GetName()) + (m_GUIInventoryItemsScrollbar->GetValue() * c_ItemsPerRow);
		} catch (std::invalid_argument) {
			pressedButtonItemIndex = -1;
		}
	}

	if (m_GUISelectedItem == nullptr) {
		if (!buttonHeld) {
			g_GUISound.ItemChangeSound()->Play(m_MenuController->GetPlayer());
		}
		if (buttonEquippedItemIndex > -1) {
			SetSelectedItem(pressedButton, buttonObject, -1, pressedButtonItemIndex, buttonHeld);
		} else {
			SetSelectedItem(pressedButton, buttonObject, pressedButtonItemIndex, -1, buttonHeld);
		}
	} else if (m_GUISelectedItem->Object == buttonObject) {
		ClearSelectedItem();
		g_GUISound.ItemChangeSound()->Play(m_MenuController->GetPlayer());
	} else {
		if (m_GUISelectedItem->EquippedItemIndex > -1) {
			if (buttonEquippedItemIndex > -1) {
				AHuman* inventoryActorAsAHuman = dynamic_cast<AHuman*>(m_InventoryActor);
				if (inventoryActorAsAHuman && dynamic_cast<HeldDevice*>(buttonObject) && dynamic_cast<HeldDevice*>(m_GUISelectedItem->Object) && HandleLockstepInventoryOp(NetGameInventoryOp::SwapHands, -1, -1)) {
					m_InventoryActor->GetDeviceSwitchSound()->Play(m_MenuController->GetPlayer());
				} else if (inventoryActorAsAHuman && dynamic_cast<HeldDevice*>(buttonObject) && dynamic_cast<HeldDevice*>(m_GUISelectedItem->Object) && inventoryActorAsAHuman->SwapEquippedHeldDevices()) {
					m_InventoryActor->GetDeviceSwitchSound()->Play(m_MenuController->GetPlayer());
				} else {
					g_GUISound.UserErrorSound()->Play(m_MenuController->GetPlayer());
				}
			} else {
				SwapEquippedItemAndInventoryItem(m_GUISelectedItem->EquippedItemIndex, pressedButtonItemIndex);
			}
		} else {
			if (buttonEquippedItemIndex > -1) {
				SwapEquippedItemAndInventoryItem(pressedButtonItemIndex, m_GUISelectedItem->InventoryIndex);
			} else {
				if (!HandleLockstepInventoryOp(NetGameInventoryOp::Reorder, m_GUISelectedItem->InventoryIndex, pressedButtonItemIndex)) {
					if (pressedButtonItemIndex >= m_InventoryActor->GetInventorySize()) {
						m_InventoryActor->AddInventoryItem(m_InventoryActor->RemoveInventoryItemAtIndex(m_GUISelectedItem->InventoryIndex));
					} else {
						m_InventoryActor->SwapInventoryItemsByIndex(m_GUISelectedItem->InventoryIndex, pressedButtonItemIndex);
					}
				}
				m_InventoryActor->GetDeviceSwitchSound()->Play(m_MenuController->GetPlayer());
			}
		}
		ClearSelectedItem();
	}
	pressedButton->OnLoseFocus();
}

bool InventoryMenuGUI::HandleLockstepInventoryOp(uint8_t op, int a, int b, const Vector* dropDirection) {
	if (!ScenarioRunner::IsLockstepControllerSyncActive() || !m_InventoryActor) {
		return false;
	}
	NetGameInventoryOp payload;
	payload.actorUID = static_cast<int64_t>(m_InventoryActor->GetUniqueID());
	payload.team = m_InventoryActor->GetTeam();
	payload.op = op;
	payload.a = static_cast<int16_t>(a);
	payload.b = static_cast<int16_t>(b);
	if (dropDirection) {
		payload.hasDropDirection = true;
		payload.dirX = dropDirection->GetX();
		payload.dirY = dropDirection->GetY();
	}
	ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, payload});
	return true;
}

bool InventoryMenuGUI::SwapEquippedItemAndInventoryItem(int equippedItemIndex, int inventoryItemIndex) {
	AHuman* inventoryActorAsAHuman = m_InventoryActorIsHuman ? dynamic_cast<AHuman*>(m_InventoryActor) : nullptr;
	if (inventoryActorAsAHuman && HandleLockstepInventoryOp(NetGameInventoryOp::SwapEquipped, equippedItemIndex, inventoryItemIndex)) {
		m_InventoryActor->GetDeviceSwitchSound()->Play(m_MenuController->GetPlayer());
		return true;
	}
	if (!inventoryActorAsAHuman || !inventoryActorAsAHuman->SwapEquippedItemAndInventoryItem(equippedItemIndex, inventoryItemIndex)) {
		g_GUISound.UserErrorSound()->Play(m_MenuController->GetPlayer());
		return false;
	}
	m_InventoryActor->GetDeviceSwitchSound()->Play(m_MenuController->GetPlayer());
	return true;
}

void InventoryMenuGUI::ReloadSelectedItem() {
	if (!m_InventoryActorIsHuman) {
		return;
	}
	AHuman* inventoryActorAsAHuman = dynamic_cast<AHuman*>(m_InventoryActor);
	const int equippedItemIndex = m_GUISelectedItem ? m_GUISelectedItem->EquippedItemIndex : -1;
	const int inventoryItemIndex = m_GUISelectedItem ? m_GUISelectedItem->InventoryIndex : -1;
	if (m_GUISelectedItem == nullptr || dynamic_cast<HDFirearm*>(m_GUISelectedItem->Object)) {
		if (!HandleLockstepInventoryOp(NetGameInventoryOp::Reload, equippedItemIndex, inventoryItemIndex)) {
			inventoryActorAsAHuman->ReloadEquippedOrInventoryFirearm(equippedItemIndex, inventoryItemIndex);
		}
	}
	ClearSelectedItem();
	m_GUIReloadButton->OnLoseFocus();
}

void InventoryMenuGUI::DropSelectedItem(const Vector* dropDirection) {
	if (!HandleLockstepInventoryOp(NetGameInventoryOp::Drop, m_GUISelectedItem->EquippedItemIndex, m_GUISelectedItem->InventoryIndex, dropDirection)) {
		m_InventoryActor->DropHeldOrInventoryItem(m_GUISelectedItem->EquippedItemIndex, m_GUISelectedItem->InventoryIndex, dropDirection);
	}
	m_InventoryActor->GetDeviceSwitchSound()->Play(m_MenuController->GetPlayer());
	ClearSelectedItem();
	m_GUIDropButton->OnLoseFocus();
}

void InventoryMenuGUI::DrawCarouselMode(BITMAP* targetBitmap, const Vector& drawPos) const {
	clear_to_color(m_CarouselBitmap.get(), g_MaskColor);
	clear_to_color(m_CarouselBGBitmap.get(), g_MaskColor);
	AllegroBitmap carouselAllegroBitmap(m_CarouselBitmap.get());
	float enableDisableProgress = static_cast<float>(m_EnableDisableAnimationTimer.GetRealTimeLimitProgress());

	for (const std::unique_ptr<CarouselItemBox>& carouselItemBox: m_CarouselItemBoxes) {
		if ((carouselItemBox->Item && carouselItemBox->Item->GetUniqueID() != 0) || (carouselItemBox->IsForEquippedItems && !m_InventoryActorEquippedItems.empty())) {
			DrawCarouselItemBoxBackground(*carouselItemBox);
			DrawCarouselItemBoxForeground(*carouselItemBox, &carouselAllegroBitmap);
		} else if (m_CarouselDrawEmptyBoxes) {
			DrawCarouselItemBoxBackground(*carouselItemBox);
			m_SmallFont->DrawAligned(&carouselAllegroBitmap, carouselItemBox->IconCenterPosition.GetFloorIntX(), carouselItemBox->IconCenterPosition.GetFloorIntY() - (m_SmallFont->GetFontHeight() / 2), "Empty", GUIFont::Centre);
		}
	}
	if (m_CarouselAnimationDirection != CarouselAnimationDirection::None && (m_CarouselExitingItemBox->Item || m_CarouselDrawEmptyBoxes)) {
		if (m_CarouselExitingItemBox->Item) {
			DrawCarouselItemBoxBackground(*m_CarouselExitingItemBox);
			if (m_CarouselExitingItemBox->Item->GetUniqueID() != 0) {
				DrawCarouselItemBoxForeground(*m_CarouselExitingItemBox, &carouselAllegroBitmap);
			}
		} else if (m_CarouselDrawEmptyBoxes) {
			DrawCarouselItemBoxBackground(*m_CarouselExitingItemBox);
			m_SmallFont->DrawAligned(&carouselAllegroBitmap, m_CarouselExitingItemBox->IconCenterPosition.GetFloorIntX(), m_CarouselExitingItemBox->IconCenterPosition.GetFloorIntY() - (m_SmallFont->GetFontHeight() / 2), "Empty", GUIFont::Centre);
		}
	}
	if (IsEnablingOrDisabling()) {
		int hiddenAreaHalfWidth = static_cast<int>((m_EnabledState == EnabledState::Enabling ? 1.0F - enableDisableProgress : enableDisableProgress) * static_cast<float>(m_CarouselBitmap->w / 2));
		rectfill(m_CarouselBitmap.get(), 0, 0, hiddenAreaHalfWidth, m_CarouselBitmap->h, g_MaskColor);
		rectfill(m_CarouselBitmap.get(), m_CarouselBitmap->w - hiddenAreaHalfWidth, 0, m_CarouselBitmap->w, m_CarouselBitmap->h, g_MaskColor);
		rectfill(m_CarouselBGBitmap.get(), 0, 0, hiddenAreaHalfWidth, m_CarouselBGBitmap->h, g_MaskColor);
		rectfill(m_CarouselBGBitmap.get(), m_CarouselBGBitmap->w - hiddenAreaHalfWidth, 0, m_CarouselBGBitmap->w, m_CarouselBGBitmap->h, g_MaskColor);
	}

	bool hasDrawnAtLeastOnce = false;
	std::list<IntRect> wrappedRectangles;
	g_SceneMan.WrapRect(IntRect(drawPos.GetFloorIntX(), drawPos.GetFloorIntY(), drawPos.GetFloorIntX() + m_CarouselBitmap->w, drawPos.GetFloorIntY() + m_CarouselBitmap->h), wrappedRectangles);
	for (const IntRect& wrappedRectangle: wrappedRectangles) {
		g_GLResourceMan.UpdateDynamicBitmap(m_CarouselBGBitmap.get(), true);
		g_GLResourceMan.UpdateDynamicBitmap(m_CarouselBitmap.get(), true);
		if (m_CarouselBackgroundTransparent) {
			g_FrameMan.SetTransTableFromPreset(TransparencyPreset::MoreTrans);
			DrawTexture(g_GLResourceMan.GetStaticTextureFromBitmap(m_CarouselBGBitmap.get()), wrappedRectangle.m_Left - m_CarouselBGBitmap->w / 2, wrappedRectangle.m_Top - m_CarouselBGBitmap->h / 2, {255, 255, 255, 75});
			DrawTexture(g_GLResourceMan.GetStaticTextureFromBitmap(m_CarouselBitmap.get()), wrappedRectangle.m_Left - m_CarouselBitmap->w / 2, wrappedRectangle.m_Top - m_CarouselBitmap->h / 2, {255, 255, 255, 255});
		} else {
			if (!hasDrawnAtLeastOnce) {
				
				draw_sprite(m_CarouselBGBitmap.get(), m_CarouselBitmap.get(), 0, 0);
			}
			DrawTexture(g_GLResourceMan.GetStaticTextureFromBitmap(m_CarouselBGBitmap.get()), wrappedRectangle.m_Left - m_CarouselBGBitmap->w / 2, wrappedRectangle.m_Top - m_CarouselBGBitmap->h / 2, {255, 255, 255, 255});
		}
		hasDrawnAtLeastOnce = true;
	}
}

void InventoryMenuGUI::DrawCarouselItemBoxBackground(const CarouselItemBox& itemBoxToDraw) const {
	auto DrawBox = [](BITMAP* targetBitmap, const Vector& boxTopLeftCorner, const Vector& boxBottomRightCorner, int color, bool roundedLeftSide, bool roundedRightSide) {
		if (roundedLeftSide) {
			circlefill(targetBitmap, boxTopLeftCorner.GetFloorIntX() + c_CarouselBoxCornerRadius, boxTopLeftCorner.GetFloorIntY() + c_CarouselBoxCornerRadius, c_CarouselBoxCornerRadius, color);
			circlefill(targetBitmap, boxTopLeftCorner.GetFloorIntX() + c_CarouselBoxCornerRadius, boxBottomRightCorner.GetFloorIntY() - c_CarouselBoxCornerRadius, c_CarouselBoxCornerRadius, color);
			rectfill(targetBitmap, boxTopLeftCorner.GetFloorIntX(), boxTopLeftCorner.GetFloorIntY() + c_CarouselBoxCornerRadius, boxTopLeftCorner.GetFloorIntX() + c_CarouselBoxCornerRadius, boxBottomRightCorner.GetFloorIntY() - c_CarouselBoxCornerRadius, color);
		}
		if (roundedRightSide) {
			circlefill(targetBitmap, boxBottomRightCorner.GetFloorIntX() - c_CarouselBoxCornerRadius, boxTopLeftCorner.GetFloorIntY() + c_CarouselBoxCornerRadius, c_CarouselBoxCornerRadius, color);
			circlefill(targetBitmap, boxBottomRightCorner.GetFloorIntX() - c_CarouselBoxCornerRadius, boxBottomRightCorner.GetFloorIntY() - c_CarouselBoxCornerRadius, c_CarouselBoxCornerRadius, color);
			rectfill(targetBitmap, boxBottomRightCorner.GetFloorIntX() - c_CarouselBoxCornerRadius, boxTopLeftCorner.GetFloorIntY() + c_CarouselBoxCornerRadius, boxBottomRightCorner.GetFloorIntX(), boxBottomRightCorner.GetFloorIntY() - c_CarouselBoxCornerRadius, color);
		}
		rectfill(targetBitmap, boxTopLeftCorner.GetFloorIntX() + (roundedLeftSide ? c_CarouselBoxCornerRadius : 0), boxTopLeftCorner.GetFloorIntY(), boxBottomRightCorner.GetFloorIntX() - (roundedRightSide ? c_CarouselBoxCornerRadius : 0), boxBottomRightCorner.GetFloorIntY(), color);
	};

	Vector spriteZeroIndexSizeOffset(1, 1);
	if (!m_CarouselBackgroundBoxBorderSize.IsZero()) {
		DrawBox(m_CarouselBGBitmap.get(), itemBoxToDraw.Pos, itemBoxToDraw.Pos + itemBoxToDraw.CurrentSize - spriteZeroIndexSizeOffset, m_CarouselBackgroundBoxBorderColor, itemBoxToDraw.RoundedAndBorderedSides.first, itemBoxToDraw.RoundedAndBorderedSides.second);
	}
	DrawBox(m_CarouselBGBitmap.get(), itemBoxToDraw.Pos + (itemBoxToDraw.RoundedAndBorderedSides.first ? m_CarouselBackgroundBoxBorderSize : Vector(0, m_CarouselBackgroundBoxBorderSize.GetY())), itemBoxToDraw.Pos + itemBoxToDraw.CurrentSize - spriteZeroIndexSizeOffset - (itemBoxToDraw.RoundedAndBorderedSides.second ? m_CarouselBackgroundBoxBorderSize : Vector(0, m_CarouselBackgroundBoxBorderSize.GetY())), m_CarouselBackgroundBoxColor, itemBoxToDraw.RoundedAndBorderedSides.first, itemBoxToDraw.RoundedAndBorderedSides.second);
}

void InventoryMenuGUI::DrawCarouselItemBoxForeground(const CarouselItemBox& itemBoxToDraw, AllegroBitmap* carouselAllegroBitmap) const {
	std::vector<BITMAP*> itemIcons;
	float totalItemMass = 0;
	itemBoxToDraw.GetIconsAndMass(itemIcons, totalItemMass, &m_InventoryActorEquippedItems);

	Vector spriteZeroIndexSizeOffset(1, 1);
	Vector multiItemDrawOffset = Vector(c_MultipleItemInBoxOffset * static_cast<float>(itemIcons.size() - 1), -c_MultipleItemInBoxOffset * static_cast<float>(itemIcons.size() - 1));
	Vector iconMaxSize = itemBoxToDraw.CurrentSize - Vector(c_MinimumItemPadding * 2, c_MinimumItemPadding * 2) - spriteZeroIndexSizeOffset;
	if (itemBoxToDraw.RoundedAndBorderedSides.first) {
		iconMaxSize.SetX(iconMaxSize.GetX() - m_CarouselBackgroundBoxBorderSize.GetX());
	}
	if (itemBoxToDraw.RoundedAndBorderedSides.second) {
		iconMaxSize.SetX(iconMaxSize.GetX() - m_CarouselBackgroundBoxBorderSize.GetX());
	}
	std::for_each(itemIcons.crbegin(), itemIcons.crend(), [this, &itemBoxToDraw, &multiItemDrawOffset, &iconMaxSize](BITMAP* iconToDraw) {
		if (iconToDraw) {
			float stretchRatio = std::max(static_cast<float>(iconToDraw->w - 1 + (multiItemDrawOffset.GetFloorIntX() / 2)) / iconMaxSize.GetX(), static_cast<float>(iconToDraw->h - 1 + (multiItemDrawOffset.GetFloorIntY() / 2)) / iconMaxSize.GetY());
			if (stretchRatio > 1.0F) {
				float stretchedWidth = static_cast<float>(iconToDraw->w) / stretchRatio;
				float stretchedHeight = static_cast<float>(iconToDraw->h) / stretchRatio;
				stretch_sprite(m_CarouselBitmap.get(), iconToDraw,
				               itemBoxToDraw.IconCenterPosition.GetFloorIntX() - static_cast<int>(itemBoxToDraw.RoundedAndBorderedSides.first ? std::floor(stretchedWidth / 2.0F) : std::ceil(stretchedWidth / 2.0F)) + multiItemDrawOffset.GetFloorIntX() + (itemBoxToDraw.RoundedAndBorderedSides.first ? m_CarouselBackgroundBoxBorderSize.GetFloorIntX() / 2 : 0) - (itemBoxToDraw.RoundedAndBorderedSides.second ? m_CarouselBackgroundBoxBorderSize.GetFloorIntX() / 2 : 0),
				               itemBoxToDraw.IconCenterPosition.GetFloorIntY() - static_cast<int>(stretchedHeight / 2.0F) + multiItemDrawOffset.GetFloorIntY(),
				               static_cast<int>(itemBoxToDraw.RoundedAndBorderedSides.first ? std::ceil(stretchedWidth) : std::floor(stretchedWidth)), static_cast<int>(stretchedHeight));
			} else {
				draw_sprite(m_CarouselBitmap.get(), iconToDraw, itemBoxToDraw.IconCenterPosition.GetFloorIntX() - (iconToDraw->w / 2) + multiItemDrawOffset.GetFloorIntX(), itemBoxToDraw.IconCenterPosition.GetFloorIntY() - (iconToDraw->h / 2) + multiItemDrawOffset.GetFloorIntY());
			}
			multiItemDrawOffset -= Vector(c_MultipleItemInBoxOffset, -c_MultipleItemInBoxOffset);
		}
	});

	std::string massString = totalItemMass < 0.1F ? "<0.1 kg" : RoundFloatToPrecision(std::fminf(999, totalItemMass), (totalItemMass < 9.95F ? 1 : 0)) + (totalItemMass > 999 ? "+ " : " ") + "kg";
	m_SmallFont->DrawAligned(carouselAllegroBitmap, itemBoxToDraw.IconCenterPosition.GetFloorIntX(), itemBoxToDraw.IconCenterPosition.GetFloorIntY() - ((itemBoxToDraw.CurrentSize.GetFloorIntY() + m_SmallFont->GetFontHeight()) / 2) + 1, massString, GUIFont::Centre);
}

void InventoryMenuGUI::DrawFullMode(BITMAP* targetBitmap, const Vector& drawPos) const {
	m_GUITopLevelBox->SetPositionAbs(drawPos.GetFloorIntX(), drawPos.GetFloorIntY());

	if (IsEnablingOrDisabling()) {
		float enableDisableProgress = static_cast<float>(m_EnableDisableAnimationTimer.GetRealTimeLimitProgress());
		if (m_EnabledState == EnabledState::Disabling) {
			enableDisableProgress = 1.0F - enableDisableProgress;
		}
		m_GUITopLevelBox->SetSize(static_cast<int>(m_GUITopLevelBoxFullSize.GetX() * enableDisableProgress), static_cast<int>(m_GUITopLevelBoxFullSize.GetY() * enableDisableProgress));
		m_GUITopLevelBox->SetPositionAbs(m_GUITopLevelBox->GetXPos() + ((m_GUITopLevelBoxFullSize.GetFloorIntX() - m_GUITopLevelBox->GetWidth()) / 2), m_GUITopLevelBox->GetYPos() + ((m_GUITopLevelBoxFullSize.GetFloorIntY() - m_GUITopLevelBox->GetHeight()) / 2));
	}

	AllegroScreen guiScreen(targetBitmap);
	m_GUIControlManager->Draw(&guiScreen);
	if (IsEnabled() && !m_GUIDisplayOnly && m_MenuController->IsMouseControlled()) {
		if (m_GUISelectedItem && m_GUISelectedItem->DragWasHeldForLongEnough()) {
			BITMAP* selectedObjectIcon = m_GUISelectedItem->Object->GetGraphicalIcon();
			draw_sprite(targetBitmap, selectedObjectIcon, m_GUICursorPos.GetFloorIntX() - (selectedObjectIcon->w / 2), m_GUICursorPos.GetFloorIntY() - (selectedObjectIcon->h / 2));
		} else {
			draw_sprite(targetBitmap, s_CursorBitmap, m_GUICursorPos.GetFloorIntX(), m_GUICursorPos.GetFloorIntY());
		}
	}
}

std::string InventoryMenuGUI::SaveCheckpoint() const {
	if (!m_PendingCheckpoint.empty()) return m_PendingCheckpoint;
	CheckpointWriter writer("InventoryMenuGUI2");
	writer(m_CheckpointInitialized);
	VisitCheckpoint(writer, *this);
	writer(GUICheckpoint::SaveEntityReference(m_InventoryActor), m_InventoryActorEquippedItems.size());
	for (const auto& [item, offhand]: m_InventoryActorEquippedItems) writer(GUICheckpoint::SaveEntityReference(item), GUICheckpoint::SaveEntityReference(offhand));
	const auto saveBox = [](const std::unique_ptr<CarouselItemBox>& box) {
		CheckpointWriter state("CarouselItemBox1");
		state(box != nullptr);
		if (box) state(GUICheckpoint::SaveEntityReference(box->Item), box->IsForEquippedItems, box->FullSize, box->CurrentSize, box->Pos, box->IconCenterPosition, box->RoundedAndBorderedSides);
		return state.Text();
	};
	for (const auto& box: m_CarouselItemBoxes) writer(saveBox(box));
	writer(saveBox(m_CarouselExitingItemBox), GUICheckpoint::SaveBitmap(m_CarouselBitmap.get()), GUICheckpoint::SaveBitmap(m_CarouselBGBitmap.get()));
	const auto buttonName = [](GUIButton* button) { return button ? button->GetName() : std::string{}; };
	writer(buttonName(m_NonMouseHighlightedButton), buttonName(m_NonMousePreviousEquippedItemsBoxButton), buttonName(m_NonMousePreviousInventoryItemsBoxButton), buttonName(m_NonMousePreviousReloadOrDropButton));
	writer(GUICheckpoint::SaveEntityReference(m_GUIInformationToggleButtonIcon), GUICheckpoint::SaveEntityReference(m_GUIReloadButtonIcon), GUICheckpoint::SaveEntityReference(m_GUIDropButtonIcon));
	writer(m_GUISelectedItem != nullptr);
	if (m_GUISelectedItem) writer(buttonName(m_GUISelectedItem->Button), GUICheckpoint::SaveEntityReference(m_GUISelectedItem->Object), m_GUISelectedItem->InventoryIndex, m_GUISelectedItem->EquippedItemIndex, m_GUISelectedItem->IsBeingDragged, m_GUISelectedItem->DragHoldCount);
	writer(m_GUIInventoryItemButtons.size());
	for (const auto& [item, button]: m_GUIInventoryItemButtons) writer(GUICheckpoint::SaveEntityReference(item), buttonName(button));
	writer(m_GUIControlManager != nullptr);
	if (m_GUIControlManager) writer(m_GUIControlManager->SaveCheckpoint());
	return writer.Text();
}

bool InventoryMenuGUI::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		if (!validateOnly && !LoadCheckpoint(text, true)) return false;
		if (text.starts_with("17 InventoryMenuGUI1 ")) {
			CheckpointReader reader(text, "InventoryMenuGUI1", validateOnly); VisitCheckpoint(reader, *this); reader.Finish(); return true;
		}
		CheckpointReader reader(text, "InventoryMenuGUI2", validateOnly);
		reader(m_CheckpointInitialized);
		VisitCheckpoint(reader, *this);
		std::string actor, carousel, background, controls;
		size_t count;
		reader.Value(actor); reader.Value(count);
		GUICheckpoint::LoadEntityReference(actor, true);
		if (count > text.size()) return false;
		std::vector<std::pair<std::string, std::string>> equipment(count);
		for (auto& [item, offhand]: equipment) { reader.Value(item); reader.Value(offhand); GUICheckpoint::LoadEntityReference(item, true); GUICheckpoint::LoadEntityReference(offhand, true); }
		std::array<std::unique_ptr<CarouselItemBox>, c_ItemsPerRow + 1> boxes;
		std::array<std::string, c_ItemsPerRow + 1> boxItems;
		for (size_t index = 0; index < boxes.size(); ++index) {
			std::string saved;
			reader.Value(saved);
			CheckpointReader state(saved, "CarouselItemBox1");
			bool present;
			state.Value(present);
			if (present) {
				boxes[index] = std::make_unique<CarouselItemBox>();
				auto& box = *boxes[index];
				state.Value(boxItems[index]); state.Value(box.IsForEquippedItems); state.Value(box.FullSize); state.Value(box.CurrentSize); state.Value(box.Pos); state.Value(box.IconCenterPosition); state.Value(box.RoundedAndBorderedSides);
				GUICheckpoint::LoadEntityReference(boxItems[index], true);
			}
			state.Finish();
		}
		reader.Value(carousel); reader.Value(background);
		GUICheckpoint::LoadBitmap(carousel, true); GUICheckpoint::LoadBitmap(background, true);
		std::array<std::string, 4> buttonNames;
		std::array<std::string, 3> icons;
		reader.Value(buttonNames); reader.Value(icons);
		for (const auto& icon: icons) GUICheckpoint::LoadEntityReference(icon, true);
		bool hasSelected;
		reader.Value(hasSelected);
		std::unique_ptr<GUISelectedItem> selected;
		std::string selectedButton, selectedObject;
		if (hasSelected) {
			selected = std::make_unique<GUISelectedItem>();
			reader.Value(selectedButton); reader.Value(selectedObject); reader.Value(selected->InventoryIndex); reader.Value(selected->EquippedItemIndex); reader.Value(selected->IsBeingDragged); reader.Value(selected->DragHoldCount);
			GUICheckpoint::LoadEntityReference(selectedObject, true);
		}
		reader.Value(count);
		if (count > text.size()) return false;
		std::vector<std::pair<std::string, std::string>> buttons(count);
		for (auto& [item, button]: buttons) { reader.Value(item); reader.Value(button); GUICheckpoint::LoadEntityReference(item, true); }
		bool hasControls;
		reader.Value(hasControls);
		if (hasControls) { reader.Value(controls); if (!GUICheckpoint::Validate(controls)) return false; }
		else if (hasSelected || !buttons.empty() || std::any_of(buttonNames.begin(), buttonNames.end(), [](const auto& name) { return !name.empty(); })) return false;
		if (validateOnly) { reader.Finish(); return true; }
		if (!m_MenuController) { reader.Finish(); m_PendingCheckpoint.assign(text); return true; }
		const auto movable = [](const std::string& value) { return const_cast<MovableObject*>(dynamic_cast<const MovableObject*>(GUICheckpoint::LoadEntityReference(value))); };
		auto* restoredActor = dynamic_cast<Actor*>(movable(actor));
		std::vector<std::pair<MovableObject*, MovableObject*>> restoredEquipment;
		for (const auto& [item, offhand]: equipment) restoredEquipment.emplace_back(movable(item), movable(offhand));
		for (size_t index = 0; index < boxes.size(); ++index) if (boxes[index]) boxes[index]->Item = movable(boxItems[index]);
		std::unique_ptr<BITMAP, void(*)(BITMAP*)> restoredCarousel(GUICheckpoint::LoadBitmap(carousel), destroy_bitmap);
		std::unique_ptr<BITMAP, void(*)(BITMAP*)> restoredBackground(GUICheckpoint::LoadBitmap(background), destroy_bitmap);
		std::array<const Icon*, 3> restoredIcons;
		for (size_t index = 0; index < icons.size(); ++index) restoredIcons[index] = dynamic_cast<const Icon*>(GUICheckpoint::LoadEntityReference(icons[index]));
		if (hasControls) {
			if (!FullOrTransferModeReadyForUse() && SetupFullOrTransferMode() < 0) return false;
			if (!m_GUIControlManager->LoadCheckpoint(controls)) return false;
		}
		const auto button = [&](const std::string& name) -> GUIButton* {
			if (name.empty()) return nullptr;
			auto* value = m_GUIControlManager ? dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl(name)) : nullptr;
			if (!value) throw std::runtime_error("an inventory button reference is missing");
			return value;
		};
		std::array<GUIButton*, 4> restoredButtons;
		for (size_t index = 0; index < buttonNames.size(); ++index) restoredButtons[index] = button(buttonNames[index]);
		if (selected) { selected->Object = movable(selectedObject); selected->Button = button(selectedButton); }
		std::vector<std::pair<MovableObject*, GUIButton*>> itemButtons;
		for (const auto& [item, name]: buttons) itemButtons.emplace_back(movable(item), button(name));
		reader.Finish();
		m_InventoryActor = restoredActor; m_InventoryActorEquippedItems = std::move(restoredEquipment);
		for (size_t index = 0; index < m_CarouselItemBoxes.size(); ++index) m_CarouselItemBoxes[index] = std::move(boxes[index]);
		m_CarouselExitingItemBox = std::move(boxes.back());
		destroy_bitmap(m_CarouselBitmap.release()); destroy_bitmap(m_CarouselBGBitmap.release());
		m_CarouselBitmap.reset(restoredCarousel.release()); m_CarouselBGBitmap.reset(restoredBackground.release());
		m_NonMouseHighlightedButton = restoredButtons[0]; m_NonMousePreviousEquippedItemsBoxButton = restoredButtons[1]; m_NonMousePreviousInventoryItemsBoxButton = restoredButtons[2]; m_NonMousePreviousReloadOrDropButton = restoredButtons[3];
		m_GUIInformationToggleButtonIcon = restoredIcons[0]; m_GUIReloadButtonIcon = restoredIcons[1]; m_GUIDropButtonIcon = restoredIcons[2];
		m_GUISelectedItem = std::move(selected); m_GUIInventoryItemButtons = std::move(itemButtons); m_PendingCheckpoint.clear();
		if (!hasControls) {
			m_GUIControlManager.reset(); m_GUIScreen.reset(); m_GUIInput.reset();
			m_GUITopLevelBox = nullptr; m_GUIInformationText = nullptr; m_GUIInformationToggleButton = nullptr;
			m_GUIEquippedItemsBox = nullptr; m_GUISwapSetButton = nullptr; m_GUIEquippedItemButton = nullptr;
			m_GUIOffhandEquippedItemButton = nullptr; m_GUIReloadButton = nullptr; m_GUIDropButton = nullptr;
			m_GUIInventoryItemsBox = nullptr; m_GUIInventoryItemsScrollbar = nullptr;
		}
		return true;
	} catch (const std::exception& exception) { std::cout << "[gui-checkpoint] inventory validation=" << validateOnly << " error=" << exception.what() << std::endl; return false; }
}
