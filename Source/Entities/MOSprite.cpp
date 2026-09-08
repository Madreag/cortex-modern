#include "MOSprite.h"
#include "CheckpointArchive.h"
#include "NativeCheckpoint.h"
#include "GUICheckpoint.h"
#include "GLResourceMan.h"
#include <unordered_map>
#include <iostream>

#include "AEmitter.h"
#include "MOSParticle.h"
#include "AHuman.h"
#include "ACraft.h"
#include "GraphicalPrimitive.h"
#include "PresetMan.h"
#include "SceneMan.h"
#include "FrameMan.h"
#include "TimerMan.h"
#include "Draw.h"

using namespace RTE;

Matrix MOSprite::GetRenderRotMatrix() const {
	return Lerp(m_PrevRotation, m_Rotation, g_TimerMan.GetSimUpdateProportion());
}

AbstractClassInfo(MOSprite, MovableObject);

MOSprite::MOSprite() {
	Clear();
}

MOSprite::~MOSprite() {
	Destroy(true);
}

void MOSprite::Clear() {
	m_PersistedMOSpriteRuntime.clear();
	m_SpriteFile.Reset();
	m_aSprite.clear();
	m_IconFile.Reset();
	m_GraphicalIcon = nullptr;
	m_SpriteBitmapOwners.clear();
	m_FrameCount = 1;
	m_SpriteOffset.Reset();
	m_Frame = 0;
	m_SpriteAnimMode = NOANIM;
	m_SpriteAnimDuration = 500;
	m_SpriteAnimTimer.Reset();
	m_SpriteAnimIsReversingFrames = false;
	m_HFlipped = false;
	m_ForcedHFlip = -1;
	m_SpriteRadius = 1.0F;
	m_SpriteDiameter = 2.0F;
	m_Rotation.Reset();
	m_PrevRotation.Reset();
	m_AngularVel = 0;
	m_PrevAngVel = 0;
	m_AngOscillations = 0;
	m_PersistedAngOscillations = 0;
	m_HasPersistedAngOscillations = false;
	m_PersistedSpriteAnimTimerAnchor = {};
	m_PersistedPrevAngVel = 0;
	m_PersistedSpriteAnimIsReversingFrames = false;
	m_HasPersistedSpriteAnimState = false;
	m_SettleMaterialDisabled = false;
	m_pEntryWound = 0;
	m_pExitWound = 0;
	m_SpriteModified = false;
}

int MOSprite::Create() {
	if (MovableObject::Create() < 0)
		return -1;
	
	// Post-process reading
	m_aSprite.clear();
	m_SpriteFile.GetAsAnimation(m_aSprite, m_FrameCount);

	if (!m_aSprite.empty() && m_aSprite[0]) {
		// Set default sprite offset
		if (m_SpriteOffset.IsZero()) {
			m_SpriteOffset.SetXY(static_cast<float>(-m_aSprite[0]->w) / 2.0F, static_cast<float>(-m_aSprite[0]->h) / 2.0F);
		}

		// Calc maximum dimensions from the Pos, based on the sprite
		float maxX = std::max(std::fabs(m_SpriteOffset.GetX()), std::fabs(static_cast<float>(m_aSprite[0]->w) + m_SpriteOffset.GetX()));
		float maxY = std::max(std::fabs(m_SpriteOffset.GetY()), std::fabs(static_cast<float>(m_aSprite[0]->h) + m_SpriteOffset.GetY()));
		m_SpriteRadius = std::sqrt((maxX * maxX) + (maxY * maxY));
		m_SpriteDiameter = m_SpriteRadius * 2.0F;
	} else
		return -1;

	return 0;
}

int MOSprite::Create(ContentFile spriteFile,
                     const int frameCount,
                     const float mass,
                     const Vector& position,
                     const Vector& velocity,
                     const unsigned long lifetime) {
	MovableObject::Create(mass, position, velocity, 0, 0, lifetime);

	m_SpriteFile = std::move(spriteFile);
	m_FrameCount = frameCount;
	m_aSprite.clear();
	m_SpriteFile.GetAsAnimation(m_aSprite, m_FrameCount);
	m_SpriteOffset.SetXY(static_cast<float>(-m_aSprite[0]->w) / 2.0F, static_cast<float>(-m_aSprite[0]->h) / 2.0F);

	m_HFlipped = false;
	if (m_ForcedHFlip == 1) {
		m_HFlipped = true;
	}

	// Calc maximum dimensions from the Pos, based on the sprite
	float maxX = std::max(std::fabs(m_SpriteOffset.GetX()), std::fabs(static_cast<float>(m_aSprite[0]->w) + m_SpriteOffset.GetX()));
	float maxY = std::max(std::fabs(m_SpriteOffset.GetY()), std::fabs(static_cast<float>(m_aSprite[0]->h) + m_SpriteOffset.GetY()));
	m_SpriteRadius = std::sqrt((maxX * maxX) + (maxY * maxY));
	m_SpriteDiameter = m_SpriteRadius * 2.0F;

	return 0;
}

int MOSprite::Create(const MOSprite& reference) {
	MovableObject::Create(reference);

	if (reference.m_aSprite.empty())
		return -1;

	m_SpriteFile = reference.m_SpriteFile;
	m_IconFile = reference.m_IconFile;
	m_GraphicalIcon = reference.m_GraphicalIcon;

	m_FrameCount = reference.m_FrameCount;
	m_Frame = reference.m_Frame;
	m_aSprite = reference.m_aSprite;
	m_SpriteBitmapOwners = reference.m_SpriteBitmapOwners;
	m_SpriteModified = reference.m_SpriteModified;
	if (reference.m_SpriteModified) {
		std::unordered_map<BITMAP*, std::shared_ptr<BITMAP>> copies;
		m_SpriteBitmapOwners.clear();
		for (BITMAP*& frame: m_aSprite) {
			auto [copy, inserted] = copies.emplace(frame, nullptr);
			if (inserted) {
				copy->second = std::shared_ptr<BITMAP>(GUICheckpoint::LoadBitmap(GUICheckpoint::SaveBitmap(frame)), [](BITMAP* value) {
					if (value) { g_GLResourceMan.DestroyBitmapInfo(value); destroy_bitmap(value); }
				});
				m_SpriteBitmapOwners.push_back(copy->second);
			}
			if (frame == reference.m_GraphicalIcon) m_GraphicalIcon = copy->second.get();
			frame = copy->second.get();
		}
		if (reference.m_GraphicalIcon && !copies.contains(reference.m_GraphicalIcon)) m_SpriteBitmapOwners.push_back(reference.ShareSpriteBitmap(reference.m_GraphicalIcon));
	}
	m_SpriteOffset = reference.m_SpriteOffset;
	m_SpriteAnimMode = reference.m_SpriteAnimMode;
	m_SpriteAnimDuration = reference.m_SpriteAnimDuration;
	m_HFlipped = reference.m_HFlipped;
	m_ForcedHFlip = reference.m_ForcedHFlip;
	if (m_ForcedHFlip == 0) {
		m_HFlipped = false;
	} else if (m_ForcedHFlip == 1) {
		m_HFlipped = true;
	}
	m_SpriteRadius = reference.m_SpriteRadius;
	m_SpriteDiameter = reference.m_SpriteDiameter;

	m_Rotation = reference.m_Rotation;
	m_PrevRotation = reference.m_PrevRotation;
	m_AngularVel = reference.m_AngularVel;
	m_AngOscillations = reference.m_AngOscillations;
	m_PersistedAngOscillations = reference.m_PersistedAngOscillations;
	m_HasPersistedAngOscillations = reference.m_HasPersistedAngOscillations;
	m_PersistedSpriteAnimTimerAnchor = reference.m_PersistedSpriteAnimTimerAnchor;
	m_PersistedPrevAngVel = reference.m_PersistedPrevAngVel;
	m_PersistedSpriteAnimIsReversingFrames = reference.m_PersistedSpriteAnimIsReversingFrames;
	m_HasPersistedSpriteAnimState = reference.m_HasPersistedSpriteAnimState;
	m_SettleMaterialDisabled = reference.m_SettleMaterialDisabled;
	m_pEntryWound = reference.m_pEntryWound;
	m_pExitWound = reference.m_pExitWound;
	//    if (reference.m_pExitWound)  Not doing anymore since we're not owning
	//        m_pExitWound = dynamic_cast<AEmitter *>(reference.m_pExitWound->Clone());

	if (IsFaithfulClone()) {
		m_PrevAngVel = reference.m_PrevAngVel;
		m_SpriteAnimTimer = reference.m_SpriteAnimTimer;
		m_SpriteAnimIsReversingFrames = reference.m_SpriteAnimIsReversingFrames;
		m_SpriteModified = reference.m_SpriteModified;
	}
	m_PersistedMOSpriteRuntime = reference.m_PersistedMOSpriteRuntime;
	if (IsFaithfulClone() && m_PersistedMOSpriteRuntime.empty()) m_PersistedMOSpriteRuntime = reference.SaveMOSpriteRuntime();
	return 0;
}

int MOSprite::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return MovableObject::ReadProperty(propName, reader));
	MatchProperty("SpecialBehaviour_MOSpriteRuntime", {
		m_PersistedMOSpriteRuntime = base64_decode(reader.ReadPropValue());
		if (!LoadMOSpriteRuntime(m_PersistedMOSpriteRuntime, true)) reader.ReportError("invalid MOSprite runtime checkpoint");
	});

	MatchProperty("SpriteFile", { reader >> m_SpriteFile; });
	MatchProperty("IconFile", {
		reader >> m_IconFile;
		m_GraphicalIcon = m_IconFile.GetAsBitmap();
	});
	MatchProperty("FrameCount", {
		reader >> m_FrameCount;
		m_aSprite.reserve(m_FrameCount);
	});
	MatchProperty("SpriteOffset", { reader >> m_SpriteOffset; });
	MatchProperty("SpriteAnimMode",
	              {
		              //        string mode;
		              //        reader >> mode;
		              int mode;
		              reader >> mode;
		              m_SpriteAnimMode = (SpriteAnimMode)mode;
		              /*
		                      if (mode == "NOANIM")
		                          m_SpriteAnimMode = NOANIM;
		                      else if (mode == "ALWAYSLOOP")
		                          m_SpriteAnimMode = ALWAYSLOOP;
		                      else if (mode == "ALWAYSPINGPONG")
		                          m_SpriteAnimMode = ALWAYSPINGPONG;
		                      else if (mode == "LOOPWHENACTIVE")
		                          m_SpriteAnimMode = LOOPWHENACTIVE;
		                      else
		                          Abort
		              */
	              });
	MatchProperty("SpriteAnimDuration", { reader >> m_SpriteAnimDuration; });
	MatchProperty("HFlipped", { reader >> m_HFlipped; });
	MatchProperty("ForcedHFlip", { reader >> m_ForcedHFlip; });
	MatchProperty("Rotation", { reader >> m_Rotation; });
	MatchProperty("PrevRotation", { reader >> m_PrevRotation; });
	MatchProperty("AngularVel", { reader >> m_AngularVel; });
	MatchProperty("Frame", { reader >> m_Frame; });
	MatchProperty("SpecialBehaviour_PrevAngVel", {
		reader >> m_PersistedPrevAngVel;
		m_HasPersistedSpriteAnimState = true;
	});
	MatchProperty("SpriteAnimTimerStart", {
		reader >> m_PersistedSpriteAnimTimerAnchor.startTicks;
		m_PersistedSpriteAnimTimerAnchor.pending = true;
	});
	MatchProperty("SpecialBehaviour_SpriteAnimIsReversingFrames", {
		reader >> m_PersistedSpriteAnimIsReversingFrames;
		m_HasPersistedSpriteAnimState = true;
	});
	MatchProperty("SpecialBehaviour_AngOscillations", {
		reader >> m_PersistedAngOscillations;
		m_HasPersistedAngOscillations = true;
	});
	MatchProperty("SettleMaterialDisabled", { reader >> m_SettleMaterialDisabled; });
	MatchProperty("EntryWound", { m_pEntryWound = dynamic_cast<const AEmitter*>(g_PresetMan.GetEntityPreset(reader)); });
	MatchProperty("ExitWound", { m_pExitWound = dynamic_cast<const AEmitter*>(g_PresetMan.GetEntityPreset(reader)); });
	MatchProperty("SpecialBehaviour_EntryWoundPreset", { m_pEntryWound = dynamic_cast<const AEmitter*>(g_PresetMan.GetEntityPreset("AEmitter", reader.ReadPropValue())); });
	MatchProperty("SpecialBehaviour_ExitWoundPreset", { m_pExitWound = dynamic_cast<const AEmitter*>(g_PresetMan.GetEntityPreset("AEmitter", reader.ReadPropValue())); });

	EndPropertyList;
}

void MOSprite::AdoptPersistedUniqueID() {
	MovableObject::AdoptPersistedUniqueID();
	if (m_HasPersistedAngOscillations) {
		// Applied here because the add path's NotResting() zeroes the live counter.
		m_AngOscillations = m_PersistedAngOscillations;
		m_HasPersistedAngOscillations = false;
	}
	m_PersistedSpriteAnimTimerAnchor.Apply(m_SpriteAnimTimer);
	if (m_HasPersistedSpriteAnimState) {
		m_PrevAngVel = m_PersistedPrevAngVel;
		m_SpriteAnimIsReversingFrames = m_PersistedSpriteAnimIsReversingFrames;
		m_HasPersistedSpriteAnimState = false;
	}
	if (!m_PersistedMOSpriteRuntime.empty()) {
		if (!LoadMOSpriteRuntime(m_PersistedMOSpriteRuntime)) throw std::runtime_error("could not restore MOSprite runtime checkpoint");
		m_PersistedMOSpriteRuntime.clear();
	}
}

void MOSprite::DiscardPersistedSnapshotState() {
	MovableObject::DiscardPersistedSnapshotState();
	m_HasPersistedAngOscillations = false;
	m_PersistedSpriteAnimTimerAnchor.pending = false;
	m_HasPersistedSpriteAnimState = false;
	m_PersistedMOSpriteRuntime.clear();
}

void MOSprite::SetEntryWound(const std::string& presetName, std::string moduleName) {
	if (presetName == "")
		m_pEntryWound = 0;
	else
		m_pEntryWound = dynamic_cast<const AEmitter*>(g_PresetMan.GetEntityPreset("AEmitter", presetName, std::move(moduleName)));
}

void MOSprite::SetExitWound(const std::string& presetName, std::string moduleName) {
	if (presetName == "")
		m_pExitWound = 0;
	else
		m_pExitWound = dynamic_cast<const AEmitter*>(g_PresetMan.GetEntityPreset("AEmitter", presetName, std::move(moduleName)));
}

std::string MOSprite::GetEntryWoundPresetName() const {
	return m_pEntryWound ? m_pEntryWound->GetPresetName() : "";
};

std::string MOSprite::GetExitWoundPresetName() const {
	return m_pExitWound ? m_pExitWound->GetPresetName() : "";
};

void MOSprite::SaveSnapshotConfiguration(Writer& writer) const {
	MovableObject::SaveSnapshotConfiguration(writer);
	writer.NewPropertyWithValue("SpriteOffset", m_SpriteOffset);
	writer.NewPropertyWithValue("SpriteAnimMode", static_cast<int>(m_SpriteAnimMode));
	writer.NewPropertyWithValue("SpriteAnimDuration", m_SpriteAnimDuration);
	writer.NewPropertyWithValue("ForcedHFlip", m_ForcedHFlip);
	writer.NewPropertyWithValue("SettleMaterialDisabled", m_SettleMaterialDisabled);
	writer.NewPropertyWithValue("SpecialBehaviour_EntryWoundPreset", m_pEntryWound ? m_pEntryWound->GetModuleAndPresetName() : "None");
	writer.NewPropertyWithValue("SpecialBehaviour_ExitWoundPreset", m_pExitWound ? m_pExitWound->GetModuleAndPresetName() : "None");
	writer.NewPropertyWithValue("SpecialBehaviour_MOSpriteRuntime", base64_encode(m_PersistedMOSpriteRuntime.empty() ? SaveMOSpriteRuntime() : m_PersistedMOSpriteRuntime, true));
}

int MOSprite::Save(Writer& writer) const {
	MovableObject::Save(writer);
	// TODO: Make proper save system that knows not to save redundant data!
	/*
	    writer.NewProperty("SpriteFile");
	    writer << m_SpriteFile;
	    writer.NewProperty("FrameCount");
	    writer << m_FrameCount;
	    writer.NewProperty("SpriteOffset");
	    writer << m_SpriteOffset;
	    writer.NewProperty("SpriteAnimMode");
	    writer << m_SpriteAnimMode;
	    writer.NewProperty("SpriteAnimDuration");
	    writer << m_SpriteAnimDuration;
	    writer.NewProperty("HFlipped");
	    writer << m_HFlipped;
	    writer.NewProperty("Rotation");
	    writer << m_Rotation.GetRadAngle();
	    writer.NewProperty("AngularVel");
	    writer << m_AngularVel;
	    writer.NewProperty("SettleMaterialDisabled");
	    writer << m_SettleMaterialDisabled;
	    writer.NewProperty("EntryWound");
	    writer << m_pEntryWound;
	    writer.NewProperty("ExitWound");
	    writer << m_pExitWound;
	*/
	return 0;
}

void MOSprite::Destroy(bool notInherited) {
	//    delete m_pEntryWound; Not doing this anymore since we're not owning
	//    delete m_pExitWound;

	if (!notInherited)
		MovableObject::Destroy();
	Clear();
}

bool MOSprite::HitTestAtPixel(int pixelX, int pixelY, bool validOnly) const {
	if (validOnly && (!GetsHitByMOs() || GetRootParent()->GetTraveling())) {
		return false;
	}

	Vector distanceBetweenTestPositionAndMO = g_SceneMan.ShortestDistance(m_Pos, Vector(static_cast<float>(pixelX), static_cast<float>(pixelY)));
	if (distanceBetweenTestPositionAndMO.MagnitudeIsGreaterThan(m_SpriteRadius)) {
		return false;
	}

	// Check the scene position in the current local space of the MO, accounting for Position, Sprite Offset, Angle and HFlipped.
	// TODO Account for Scale as well someday, maybe.
	Matrix rotation = m_Rotation; // <- Copy to non-const variable so / operator overload works.
	Vector entryPos = (distanceBetweenTestPositionAndMO / rotation).GetXFlipped(m_HFlipped) - m_SpriteOffset;
	int localX = entryPos.GetFloorIntX();
	int localY = entryPos.GetFloorIntY();

	BITMAP* sprite = m_aSprite[m_Frame];
	return is_inside_bitmap(sprite, localX, localY, 0) && _getpixel(sprite, localX, localY) != ColorKeys::g_MaskColor;
}

void MOSprite::SetFrame(unsigned int newFrame) {
	if (newFrame < 0)
		newFrame = 0;
	if (newFrame >= m_FrameCount)
		newFrame = m_FrameCount - 1;

	m_Frame = newFrame;
}

bool MOSprite::SetNextFrame() {
	if (++m_Frame >= m_FrameCount) {
		m_Frame = 0;
		return true;
	}
	return false;
}

bool MOSprite::IsOnScenePoint(Vector& scenePoint) const {
	if (!m_aSprite[m_Frame])
		return false;
	// TODO: TAKE CARE OF WRAPPING
	/*
	    // Take care of wrapping situations
	    bitmapPos = m_Pos + m_BitmapOffset;
	    Vector aScenePoint[4];
	    aScenePoint[0] = scenePoint;
	    int passes = 1;

	    // See if need to double draw this across the scene seam if we're being drawn onto a scenewide bitmap
	    if (targetPos.IsZero())
	    {
	        if (g_SceneMan.SceneWrapsX())
	        {
	            if (bitmapPos.m_X < m_pFGColor->w)
	            {
	                aScenePoint[passes] = aScenePoint[0];
	                aScenePoint[passes].m_X += g_SceneMan.GetSceneWidth();
	                passes++;
	            }
	            else if (aScenePoint[0].m_X > pTargetBitmap->w - m_pFGColor->w)
	            {
	                aScenePoint[passes] = aScenePoint[0];
	                aScenePoint[passes].m_X -= g_SceneMan.GetSceneWidth();
	                passes++;
	            }
	        }
	        if (g_SceneMan.SceneWrapsY())
	        {

	        }
	    }

	    // Check all the passes needed
	    for (int i = 0; i < passes; ++i)
	    {
	        if (IsWithinBox(aScenePoint[i], m_Pos + m_BitmapOffset, m_pFGColor->w, m_pFGColor->h))
	        {
	            if (getpixel(m_pFGColor, aScenePoint[i].m_X, aScenePoint[i].m_Y) != g_MaskColor ||
	               (m_pBGColor && getpixel(m_pBGColor, aScenePoint[i].m_X, aScenePoint[i].m_Y) != g_MaskColor) ||
	               (m_pMaterial && getpixel(m_pMaterial, aScenePoint[i].m_X, aScenePoint[i].m_Y) != g_MaterialAir))
	               return true;
	        }
	    }
	*/
	if (WithinBox(scenePoint, m_Pos.m_X - m_SpriteRadius, m_Pos.m_Y - m_SpriteRadius, m_Pos.m_X + m_SpriteRadius, m_Pos.m_Y + m_SpriteRadius)) {
		// Get scene point in object's relative space
		Vector spritePoint = scenePoint - m_Pos;
		spritePoint.FlipX(m_HFlipped);
		// Check over overlap
		int pixel = getpixel(m_aSprite[m_Frame], spritePoint.m_X - m_SpriteOffset.m_X, spritePoint.m_Y - m_SpriteOffset.m_Y);
		// Check that it isn't outside the bitmap, and not of the key color
		if (pixel != -1 && pixel != g_MaskColor)
			return true;
	}

	return false;
}

Vector MOSprite::RotateOffset(const Vector& offset) const {
	Vector rotOff(offset.GetXFlipped(m_HFlipped));
	rotOff *= const_cast<Matrix&>(m_Rotation);
	return rotOff;
}

Vector MOSprite::UnRotateOffset(const Vector& offset) const {
	Vector rotOff(offset);
	rotOff /= const_cast<Matrix&>(m_Rotation);
	return rotOff.GetXFlipped(m_HFlipped);
}

int MOSprite::GetSpritePixelIndex(int x, int y, int whichFrame) const {
	unsigned int clampedFrame = std::max(std::min(whichFrame, static_cast<int>(m_FrameCount) - 1), 0);
	BITMAP* targetSprite = m_aSprite[clampedFrame];
	if (is_inside_bitmap(targetSprite, x, y, 0)) {
		return _getpixel(targetSprite, x, y);
	}
	return -1;
}

std::vector<Vector>* MOSprite::GetAllSpritePixelPositions(const Vector& origin, float angle, bool hflipped, int whichFrame, int ignoreIndex, bool invert, bool includeChildren) {
	std::vector<Vector>* posList = new std::vector<Vector>();
	unsigned int clampedFrame = std::max(std::min(whichFrame, static_cast<int>(m_FrameCount) - 1), 0);
	int spriteSize = m_SpriteDiameter;
	if (includeChildren && dynamic_cast<MOSRotating*>(this)) {
		spriteSize = dynamic_cast<MOSRotating*>(this)->GetDiameter();
	}
	BITMAP* sprite = m_aSprite[clampedFrame];
	BITMAP* temp = create_bitmap_ex(8, spriteSize, spriteSize);
	rectfill(temp, 0, 0, temp->w - 1, temp->h - 1, 0);
	Vector tempCentre = Vector(temp->w / 2, temp->h / 2);
	Vector spriteCentre = Vector(sprite->w / 2, sprite->h / 2);

	if (includeChildren) {
		Draw(temp, m_Pos - tempCentre);
	} else {
		Vector offset = (tempCentre + (m_SpriteOffset + spriteCentre).GetXFlipped(m_HFlipped).RadRotate(m_Rotation.GetRadAngle()) - spriteCentre);
		if (!hflipped) {
			rotate_scaled_sprite(temp, sprite, offset.m_X, offset.m_Y, ftofix(GetAllegroAngle(-m_Rotation.GetDegAngle())), ftofix(m_Scale));
		} else {
			rotate_scaled_sprite_v_flip(temp, sprite, offset.m_X, offset.m_Y, ftofix(GetAllegroAngle(-m_Rotation.GetDegAngle())) + itofix(128), ftofix(m_Scale));
		}
	}

	for (int y = 0; y < temp->h; y++) {
		for (int x = 0; x < temp->w; x++) {
			int pixelIndex = _getpixel(temp, x, y);
			if (pixelIndex >= 0 && (pixelIndex != ignoreIndex) != invert) {
				Vector pixelPos = (Vector(x, y) - tempCentre) + origin;
				posList->push_back(pixelPos);
			}
		}
	}

	destroy_bitmap(temp);
	return posList;
}

bool MOSprite::SetSpritePixelIndex(int x, int y, int whichFrame, int colorIndex, int ignoreIndex, bool invert) {
	if (m_aSprite.empty()) return false;
	if (!m_SpriteModified) {
		std::vector<BITMAP*> spriteList;
		std::vector<std::shared_ptr<BITMAP>> owners;

		for (BITMAP* sprite : m_aSprite) {
			BITMAP* spriteCopy = create_bitmap_ex(8, sprite->w, sprite->h);
			rectfill(spriteCopy, 0, 0, spriteCopy->w - 1, spriteCopy->h - 1, 0);
			draw_sprite(spriteCopy, sprite, 0, 0);
			spriteList.push_back(spriteCopy);
			owners.emplace_back(spriteCopy, [](BITMAP* value) { g_GLResourceMan.DestroyBitmapInfo(value); destroy_bitmap(value); });
		}

		m_aSprite = spriteList;
		if (m_GraphicalIcon) owners.push_back(ShareSpriteBitmap(m_GraphicalIcon));
		m_SpriteBitmapOwners = std::move(owners);
		m_SpriteModified = true;
	}

	unsigned int clampedFrame = std::max(std::min(whichFrame, static_cast<int>(m_FrameCount) - 1), 0);
	BITMAP* targetSprite = m_aSprite[clampedFrame];
	if (is_inside_bitmap(targetSprite, x, y, 0) && (ignoreIndex < 0 || (_getpixel(targetSprite, x, y) != ignoreIndex) != invert)) {
		_putpixel(targetSprite, x, y, colorIndex);
		g_GLResourceMan.DestroyBitmapInfo(targetSprite);
		return true;
	}
	return false;
}

void MOSprite::SetAllSpritePixelIndexes(int whichFrame, int colorIndex, int ignoreIndex, bool invert) {
	unsigned int clampedFrame = std::max(std::min(whichFrame, static_cast<int>(m_FrameCount) - 1), 0);
	BITMAP* targetSprite = m_aSprite[clampedFrame];
	for (int y = 0; y < targetSprite->h; y++) {
		for (int x = 0; x < targetSprite->w; x++) {
			SetSpritePixelIndex(x, y, clampedFrame, colorIndex, ignoreIndex, invert);
		}
	}
}

void MOSprite::Update() {
	MovableObject::Update();

	// First, check that the sprite has enough frames to even have an animation and override the setting if not
	if (m_FrameCount > 1) {
		// If animation mode is set to something other than ALWAYSLOOP but only has 2 frames, override it because it's pointless
		if ((m_SpriteAnimMode == ALWAYSRANDOM || m_SpriteAnimMode == ALWAYSPINGPONG) && m_FrameCount == 2) {
			m_SpriteAnimMode = ALWAYSLOOP;
		} else if (m_SpriteAnimMode == OVERLIFETIME) {
			// If animation mode is set to over lifetime but lifetime is unlimited, override to always loop otherwise it will never animate.
			if (m_Lifetime == 0) {
				m_SpriteAnimMode = ALWAYSLOOP;
			} else {
				double lifeTimeFrame = static_cast<double>(m_FrameCount) * (m_AgeTimer.GetElapsedSimTimeMS() / static_cast<double>(m_Lifetime));
				m_Frame = static_cast<int>(std::floor(lifeTimeFrame));
				if (m_Frame >= m_FrameCount) {
					m_Frame = m_FrameCount - 1;
				}
				return;
			}
		}
	} else {
		m_SpriteAnimMode = NOANIM;
	}

	// Animate the sprite, if applicable
	unsigned int frameTime = m_SpriteAnimDuration / m_FrameCount;
	unsigned int prevFrame = m_Frame;

	if (m_SpriteAnimTimer.GetElapsedSimTimeMS() > frameTime) {
		switch (m_SpriteAnimMode) {
			case ALWAYSLOOP:
				m_Frame = ((m_Frame + 1) % m_FrameCount);
				m_SpriteAnimTimer.Reset();
				break;
			case ALWAYSRANDOM:
				while (m_Frame == prevFrame) {
					m_Frame = RandomNum<int>(0, m_FrameCount - 1);
				}
				m_SpriteAnimTimer.Reset();
				break;
			case ALWAYSPINGPONG:
				if (m_Frame == m_FrameCount - 1) {
					m_SpriteAnimIsReversingFrames = true;
				} else if (m_Frame == 0) {
					m_SpriteAnimIsReversingFrames = false;
				}
				m_SpriteAnimIsReversingFrames ? m_Frame-- : m_Frame++;
				m_SpriteAnimTimer.Reset();
				break;
			default:
				break;
		}
	}
}

void MOSprite::Draw(BITMAP* pTargetBitmap,
                    const Vector& targetPos,
                    DrawMode mode,
                    bool onlyPhysical) const {
	if (!m_aSprite[m_Frame])
		RTEAbort("Sprite frame pointer is null when drawing MOSprite!");

	// Apply offsets and positions.
	Vector spriteOffset;
	if (m_HFlipped)
		spriteOffset.SetXY(-(m_aSprite[m_Frame]->w + m_SpriteOffset.m_X), m_SpriteOffset.m_Y);
	else
		spriteOffset = m_SpriteOffset;

	// Sim-bound modes (MOID, material, door) snap to sim pos; visual modes lerp. Mirrors the pattern in MOSRotating / MOSParticle / MOPixel.
	const bool simBoundMode = mode == g_DrawMOID || mode == g_DrawMaterial || mode == g_DrawDoor;
	const float fLerp = simBoundMode ? 1.0f : g_TimerMan.GetSimUpdateProportion();
	Vector spritePos(Lerp(GetPrevPos(), GetPos(), fLerp) + spriteOffset - targetPos);

	// Take care of wrapping situations
	Vector aDrawPos[4];
	aDrawPos[0] = spritePos;
	int passes = 1;

	// Only bother with wrap drawing if the scene actually wraps around
	if (g_SceneMan.SceneWrapsX()) {
		// See if need to double draw this across the scene seam if we're being drawn onto a scenewide bitmap
		if (targetPos.IsZero() && m_WrapDoubleDraw) {
			if (spritePos.m_X < m_aSprite[m_Frame]->w) {
				aDrawPos[passes] = spritePos;
				aDrawPos[passes].m_X += pTargetBitmap->w;
				passes++;
			} else if (spritePos.m_X > pTargetBitmap->w - m_aSprite[m_Frame]->w) {
				aDrawPos[passes] = spritePos;
				aDrawPos[passes].m_X -= pTargetBitmap->w;
				passes++;
			}
		}
		// Only screenwide target bitmap, so double draw within the screen if the screen is straddling a scene seam
		else if (m_WrapDoubleDraw) {
			if (targetPos.m_X < 0) {
				aDrawPos[passes] = aDrawPos[0];
				aDrawPos[passes].m_X -= g_SceneMan.GetSceneWidth();
				passes++;
			}
			if (targetPos.m_X + pTargetBitmap->w > g_SceneMan.GetSceneWidth()) {
				aDrawPos[passes] = aDrawPos[0];
				aDrawPos[passes].m_X += g_SceneMan.GetSceneWidth();
				passes++;
			}
		}
	}

	for (int i = 0; i < passes; ++i) {
		int spriteX = aDrawPos[i].GetFloorIntX();
		int spriteY = aDrawPos[i].GetFloorIntY();
		switch (mode) {
			case g_DrawMaterial:
				RTEAbort("Ordered to draw an MOSprite in its material, which is not possible!");
				break;
			case g_DrawWhite:
				draw_character_ex(pTargetBitmap, m_aSprite[m_Frame], spriteX, spriteY, g_WhiteColor, -1);
				break;
			case g_DrawTrans:
				DrawTexture(m_aSprite[m_Frame], spriteX, spriteY, {255, 255, 255, g_FrameMan.GetCurrentAlpha()});
				break;
			case g_DrawAlpha:
				set_alpha_blender();
				draw_trans_sprite(pTargetBitmap, m_aSprite[m_Frame], spriteX, spriteY);
				break;
			default:
				if (!m_HFlipped) {
					draw_sprite(pTargetBitmap, m_aSprite[m_Frame], spriteX, spriteY);
				} else {
					draw_sprite_h_flip(pTargetBitmap, m_aSprite[m_Frame], spriteX, spriteY);
				}
		}

		g_SceneMan.RegisterDrawing(pTargetBitmap, m_MOID, spriteX, spriteY, spriteX + m_aSprite[m_Frame]->w, spriteY + m_aSprite[m_Frame]->h);
	}
}

std::string MOSprite::SaveMOSpriteRuntime() const {
	CheckpointWriter archive("MOSpriteRuntime2");
	archive(m_Rotation, m_PrevRotation, m_AngularVel, m_PrevAngVel, m_FrameCount, m_SpriteOffset, m_Frame);
	archive(m_SpriteAnimMode, m_SpriteAnimDuration, m_SpriteAnimTimer, m_SpriteAnimIsReversingFrames, m_HFlipped, m_ForcedHFlip, m_SpriteRadius);
	archive(m_SpriteDiameter, m_AngOscillations, m_SettleMaterialDisabled, m_SpriteModified);
	archive(m_SpriteFile.SaveCheckpoint(), m_IconFile.SaveCheckpoint());
	std::vector<BITMAP*> images;
	std::unordered_map<BITMAP*, size_t> indices;
	const auto index = [&](BITMAP* image) {
		if (!image) return size_t{0};
		auto [found, inserted] = indices.emplace(image, images.size() + 1);
		if (inserted) images.push_back(image);
		return found->second;
	};
	std::vector<size_t> frames;
	for (BITMAP* frame: m_aSprite) frames.push_back(index(frame));
	const size_t icon = index(m_GraphicalIcon);
	archive(images.size());
	for (BITMAP* image: images) archive(GUICheckpoint::SaveSharedBitmap(image));
	archive(frames, icon);
	return archive.Text();
}

bool MOSprite::LoadMOSpriteRuntime(std::string_view text, bool validateOnly) {
	try {
		if (!validateOnly && !LoadMOSpriteRuntime(text, true)) return false;
		const bool complete = text.starts_with("16 MOSpriteRuntime2 ");
		if (!validateOnly && complete && SaveMOSpriteRuntime() == text) return true;
		CheckpointReader archive(text, complete ? "MOSpriteRuntime2" : "MOSpriteRuntime1", validateOnly);
		archive(m_Rotation, m_PrevRotation, m_AngularVel, m_PrevAngVel, m_FrameCount, m_SpriteOffset, m_Frame);
		archive(m_SpriteAnimMode, m_SpriteAnimDuration, m_SpriteAnimTimer, m_SpriteAnimIsReversingFrames, m_HFlipped, m_ForcedHFlip, m_SpriteRadius);
		archive(m_SpriteDiameter, m_AngOscillations, m_SettleMaterialDisabled, m_SpriteModified);
		if (complete) {
			std::string spriteFile, iconFile;
			archive.Value(spriteFile); archive.Value(iconFile);
			if (!m_SpriteFile.LoadCheckpoint(spriteFile, true) || !m_IconFile.LoadCheckpoint(iconFile, true)) throw std::runtime_error("invalid sprite content file");
			std::vector<std::string> images;
			std::vector<size_t> frames;
			size_t icon = 0;
			archive.Value(images); archive.Value(frames); archive.Value(icon);
			for (const auto& image: images) GUICheckpoint::LoadSharedBitmap(image, true);
			if (icon > images.size()) throw std::runtime_error("invalid sprite icon reference");
			for (size_t frame: frames) if (frame > images.size()) throw std::runtime_error("invalid sprite frame reference");
			std::vector<std::shared_ptr<BITMAP>> owners;
			if (!validateOnly) for (const auto& image: images) {
				auto bitmap = GUICheckpoint::LoadSharedBitmap(image);
				if (!bitmap) throw std::runtime_error("null sprite bitmap pool entry");
				owners.push_back(std::move(bitmap));
			}
			archive.OnCommit([this, spriteFile = std::move(spriteFile), iconFile = std::move(iconFile), owners = std::move(owners), frames = std::move(frames), icon]() mutable {
				std::vector<BITMAP*> sprites;
				for (size_t frame: frames) sprites.push_back(frame ? owners[frame - 1].get() : nullptr);
				m_SpriteFile.LoadCheckpoint(spriteFile); m_IconFile.LoadCheckpoint(iconFile);
				m_aSprite = std::move(sprites);
				m_GraphicalIcon = icon ? owners[icon - 1].get() : nullptr;
				m_SpriteBitmapOwners = std::move(owners);
			});
		}
		archive.Finish();
		return true;
	} catch (const std::exception& error) { std::cerr << "[sprite-checkpoint] " << error.what() << std::endl; return false; }
}

std::shared_ptr<BITMAP> MOSprite::ShareSpriteBitmap(BITMAP* bitmap) const {
	std::function<std::shared_ptr<BITMAP>(const MOSprite*)> find = [&](const MOSprite* sprite) -> std::shared_ptr<BITMAP> {
		if (!sprite) return {};
		for (const auto& owner: sprite->m_SpriteBitmapOwners) if (owner.get() == bitmap) return owner;
		if (const auto* rotating = dynamic_cast<const MOSRotating*>(sprite)) {
			for (const auto* child: rotating->GetAttachables()) if (auto owner = find(child)) return owner;
			for (const auto* child: rotating->GetWoundList()) if (auto owner = find(child)) return owner;
		}
		if (const auto* actor = dynamic_cast<const Actor*>(sprite)) for (const auto* child: *actor->GetInventory()) if (auto owner = find(dynamic_cast<const MOSprite*>(child))) return owner;
		if (const auto* craft = dynamic_cast<const ACraft*>(sprite)) for (const auto* child: craft->GetCollectedInventory()) if (auto owner = find(dynamic_cast<const MOSprite*>(child))) return owner;
		return {};
	};
	if (auto owner = find(this)) return owner;
	return std::shared_ptr<BITMAP>(bitmap, [](BITMAP*) {});
}

bool MOSprite::RunCheckpointSelfTest() {
	bool passed = true;
	const auto check = [&](const char* name, bool valid) { passed &= valid; std::cout << "[sprite-checkpoint-selftest] " << (valid ? "PASS " : "FAIL ") << name << std::endl; };
	try {
		MOSParticle source;
		source.m_SpriteFile = ContentFile("Base.rte/GUIs/Skins/Cursor.png");
		BITMAP* cached = source.m_SpriteFile.GetAsBitmap();
		source.m_aSprite = {cached, cached}; source.m_FrameCount = 2; source.m_Frame = 1;
		source.m_GraphicalIcon = cached;
		const std::string cachePixels = GUICheckpoint::SaveBitmap(cached), cachedState = source.SaveMOSpriteRuntime();
		check("cached_frame_and_icon_aliases", source.LoadMOSpriteRuntime(cachedState) && source.GetSpriteFrame(0) == source.GetSpriteFrame(1) && source.GetGraphicalIcon() == cached);
		check("public_pixel_mutation", source.SetSpritePixelIndex(1, 1, 0, 37, -1, false) && source.GetSpritePixelIndex(1, 1, 0) == 37);
		check("preset_pixels_remain_independent", GUICheckpoint::SaveBitmap(cached) == cachePixels && source.GetSpriteFrame(0) != cached);
		const std::string modified = source.SaveMOSpriteRuntime();
		MOSParticle clone;
		clone.MOSprite::Create(source);
		check("clone_keeps_mutated_pixels", clone.GetSpritePixelIndex(1, 1, 0) == 37 && clone.GetSpriteFrame(0) != source.GetSpriteFrame(0));
		clone.SetSpritePixelIndex(1, 1, 0, 91, -1, false);
		check("clone_pixel_mutations_are_independent", source.GetSpritePixelIndex(1, 1, 0) == 37 && clone.GetSpritePixelIndex(1, 1, 0) == 91);
		MOSParticle restored;
		check("fresh_mutated_frame_restore", restored.LoadMOSpriteRuntime(modified) && restored.GetSpritePixelIndex(1, 1, 0) == 37 && restored.SaveMOSpriteRuntime() == modified);
		check("trailing_rejected_atomically", !restored.LoadMOSpriteRuntime(modified + "extra") && restored.SaveMOSpriteRuntime() == modified);
		check("truncated_rejected_atomically", !restored.LoadMOSpriteRuntime(modified.substr(0, modified.size() / 2)) && restored.SaveMOSpriteRuntime() == modified);
		auto bitmap = std::make_unique<BitmapPrimitive>(-1, Vector(), &source, 0, 0, false, false);
		const std::string retained = GUICheckpoint::SaveBitmap(bitmap->m_Bitmap);
		source.Reset();
		check("queued_bitmap_survives_source_reset", bitmap->m_SpriteOwner.get() == nullptr && GUICheckpoint::SaveBitmap(bitmap->m_Bitmap) == retained);
		check("queued_expired_source_checkpoint", !bitmap->SaveCheckpoint().empty());
		check("fresh_cached_restore_alias", restored.LoadMOSpriteRuntime(cachedState) && restored.GetSpriteFrame(0) == cached && restored.GetGraphicalIcon() == cached);
		const auto* preset = dynamic_cast<const AHuman*>(g_PresetMan.GetEntityPreset("AHuman", "Green Dummy", "Base.rte"));
		if (!preset) throw std::runtime_error("missing sprite icon fixture preset");
		std::unique_ptr<AHuman> actor(static_cast<AHuman*>(preset->Clone()));
		actor->m_GraphicalIcon = nullptr;
		if (!actor->GetHead()) throw std::runtime_error("missing sprite icon fixture head");
		actor->GetHead()->SetSpritePixelIndex(1, 1, 0, 95, -1, false);
		auto childIcon = actor->ShareSpriteBitmap(actor->GetGraphicalIcon());
		const std::string childPixels = GUICheckpoint::SaveBitmap(childIcon.get());
		check("child_icon_owner_shared", childIcon.use_count() > 1 && childIcon.get() == actor->GetHead()->GetSpriteFrame());
		actor.reset();
		check("child_icon_survives_actor_delete", GUICheckpoint::SaveBitmap(childIcon.get()) == childPixels);
	} catch (const std::exception& error) { check("exception", false); std::cerr << "[sprite-checkpoint-selftest] " << error.what() << std::endl; }
	return passed;
}
