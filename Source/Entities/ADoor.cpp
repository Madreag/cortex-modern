#include "ADoor.h"
#include "ActivityMan.h"
#include "CheckpointNativeSnapshot.h"
#include "Vector.h"
#include "Timer.h"
#include "ADSensor.h"
#include "MetricsCollector.h"
#include "CheckpointArchive.h"
#include "NativeCheckpoint.h"
#include "AtomGroup.h"
#include "Attachable.h"
#include "Matrix.h"
#include "SLTerrain.h"
#include "PresetMan.h"
#include "SettingsMan.h"
#include "SoundContainer.h"
#include "ScenarioRunner.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <vector>

using namespace RTE;

ConcreteClassInfo(ADoor, Actor, 20);

namespace {
	// Single player's path update redraws each drawn door of an active team every 100 ms of sim time, so a dug door is whole again by then.
	// A match's path updates land on each machine's own ticks, so there the redraw follows the shared sim clock and every peer makes it on the same tick.
	bool MatchPathUpdateResealDue(int team) {
		if (!ScenarioRunner::LockstepCoordinatorAttached() || !g_ActivityMan.ActivityRunning() || !g_ActivityMan.GetActivity()->TeamActive(team)) {
			return false;
		}
		const long long period = g_TimerMan.GetTicksPerSecond() / 10;
		const long long now = g_TimerMan.GetSimTickCount();
		return now / period != (now - g_TimerMan.GetDeltaTimeTicks()) / period;
	}

	// CC_TEST_DOOR_DIG_AGE_MS=N: in a match, each closed door with its material drawn has a slit dug through it once it is N sim ms old,
	// then says how many sim ms its material took to be whole again.
	long long DoorDigAgeMs() {
		static const long long ageMs = [] {
			const char* value = std::getenv("CC_TEST_DOOR_DIG_AGE_MS");
			return value ? std::atoll(value) : -1LL;
		}();
		return ageMs;
	}

	struct DoorDig {
		long long dugAtMs = 0;
		std::vector<std::pair<int, int>> pixels;
		bool reported = false;
	};

	std::unordered_map<long, DoorDig>& DoorDigs() {
		static std::unordered_map<long, DoorDig> digs;
		return digs;
	}

	void DigDoorForTest(const ADoor& door) {
		if (!ScenarioRunner::LockstepCoordinatorAttached() || !door.GetDoor() || !door.GetDoorMaterialDrawn() || door.GetDoorState() != ADoor::CLOSED ||
		    static_cast<long long>(door.GetAge()) < DoorDigAgeMs() || DoorDigs().count(door.GetUniqueID()) != 0) {
			return;
		}
		BITMAP* material = g_SceneMan.GetTerrain()->GetMaterialBitmap();
		Box bounds = door.GetDoor()->GetBoundingBox();
		bounds.Unflip();
		const int left = std::max(0, static_cast<int>(std::floor(bounds.GetCorner().GetX())));
		const int top = std::max(0, static_cast<int>(std::floor(bounds.GetCorner().GetY())));
		const int right = std::min(material->w, static_cast<int>(std::ceil(bounds.GetCorner().GetX() + bounds.GetWidth())));
		const int bottom = std::min(material->h, static_cast<int>(std::ceil(bounds.GetCorner().GetY() + bounds.GetHeight())));
		if (right - left < 4 || bottom - top < 4) {
			return;
		}
		// Only this door's own silhouette is cut: a neighbouring door's material inside the box comes back with that door, not this one.
		BITMAP* silhouette = create_bitmap_ex(8, right - left, bottom - top);
		clear_to_color(silhouette, g_MaterialAir);
		const bool renderDraw = g_SceneMan.IsRenderDrawContext();
		g_SceneMan.SetRenderDrawContext(true);
		door.GetDoor()->Draw(silhouette, Vector(static_cast<float>(left), static_cast<float>(top)), g_DrawDoor, true);
		g_SceneMan.SetRenderDrawContext(renderDraw);
		int minX = silhouette->w;
		int minY = silhouette->h;
		int maxX = -1;
		int maxY = -1;
		for (int y = 0; y < silhouette->h; ++y) {
			for (int x = 0; x < silhouette->w; ++x) {
				if (silhouette->line[y][x] != g_MaterialAir) {
					minX = std::min(minX, x);
					maxX = std::max(maxX, x);
					minY = std::min(minY, y);
					maxY = std::max(maxY, y);
				}
			}
		}
		DoorDig& dig = DoorDigs()[door.GetUniqueID()];
		dig.dugAtMs = g_TimerMan.GetSimTimeMS();
		if (maxX >= 0) {
			// Three pixels wide across the silhouette's short side, a quarter of the way from its end, so the slit parts the door the way a dig does.
			const bool tall = maxY - minY >= maxX - minX;
			const int along = tall ? minY + (maxY - minY) * 3 / 4 : minX + (maxX - minX) * 3 / 4;
			for (int y = tall ? along : minY; y <= (tall ? std::min(along + 2, maxY) : maxY); ++y) {
				for (int x = tall ? minX : along; x <= (tall ? maxX : std::min(along + 2, maxX)); ++x) {
					if (silhouette->line[y][x] != g_MaterialAir && _getpixel(material, left + x, top + y) == g_MaterialDoor) {
						_putpixel(material, left + x, top + y, g_MaterialAir);
						dig.pixels.emplace_back(left + x, top + y);
					}
				}
			}
		}
		destroy_bitmap(silhouette);
		g_SceneMan.GetTerrain()->AddUpdatedMaterialArea(Box(Vector(static_cast<float>(left), static_cast<float>(top)), Vector(static_cast<float>(right), static_cast<float>(bottom))));
		std::cout << "[door-reseal] door=" << door.GetUniqueID() << " team=" << door.GetTeam() << " dug=" << dig.pixels.size() << " at_ms=" << dig.dugAtMs << std::endl;
	}

	void NoteDoorResealForTest(const ADoor& door) {
		const auto found = DoorDigs().find(door.GetUniqueID());
		if (found == DoorDigs().end() || found->second.reported) {
			return;
		}
		DoorDig& dig = found->second;
		BITMAP* material = g_SceneMan.GetTerrain()->GetMaterialBitmap();
		const long long afterMs = g_TimerMan.GetSimTimeMS() - dig.dugAtMs;
		const auto whole = std::count_if(dig.pixels.begin(), dig.pixels.end(), [material](const std::pair<int, int>& pixel) { return _getpixel(material, pixel.first, pixel.second) == g_MaterialDoor; });
		if (!dig.pixels.empty() && static_cast<size_t>(whole) == dig.pixels.size()) {
			std::cout << "[door-reseal] door=" << door.GetUniqueID() << " team=" << door.GetTeam() << " resealed_after_ms=" << afterMs << std::endl;
			dig.reported = true;
		} else if (afterMs > 12000) {
			std::cout << "[door-reseal] door=" << door.GetUniqueID() << " team=" << door.GetTeam() << " not resealed after_ms=" << afterMs << " whole=" << whole << "/" << dig.pixels.size()
			          << " state=" << door.GetDoorState() << " drawn=" << door.GetDoorMaterialDrawn() << std::endl;
			dig.reported = true;
		}
	}
} // namespace

ADoor::ADoor(const ADoor& source, CheckpointNativeSnapshot& snapshot) :
	Actor(source, snapshot),
	m_InitialSpriteAnimDuration(snapshot.Freeze(source.m_InitialSpriteAnimDuration)),
	m_Sensors(snapshot.Freeze(source.m_Sensors)),
	m_SensorTimer(snapshot.Freeze(source.m_SensorTimer)),
	m_SensorInterval(snapshot.Freeze(source.m_SensorInterval)),
	m_Door(snapshot.Freeze(source.m_Door)),
	m_DoorState(snapshot.Freeze(source.m_DoorState)),
	m_DoorStateOnStop(snapshot.Freeze(source.m_DoorStateOnStop)),
	m_ClosedByDefault(snapshot.Freeze(source.m_ClosedByDefault)),
	m_OpenOffset(snapshot.Freeze(source.m_OpenOffset)),
	m_ClosedOffset(snapshot.Freeze(source.m_ClosedOffset)),
	m_OpenAngle(snapshot.Freeze(source.m_OpenAngle)),
	m_ClosedAngle(snapshot.Freeze(source.m_ClosedAngle)),
	m_DoorMoveTimer(snapshot.Freeze(source.m_DoorMoveTimer)),
	m_DoorMoveTime(snapshot.Freeze(source.m_DoorMoveTime)),
	m_ResumeAfterStop(snapshot.Freeze(source.m_ResumeAfterStop)),
	m_ChangedDirectionAfterStop(snapshot.Freeze(source.m_ChangedDirectionAfterStop)),
	m_DoorMoveStopTime(snapshot.Freeze(source.m_DoorMoveStopTime)),
	m_ResetToDefaultStateTimer(snapshot.Freeze(source.m_ResetToDefaultStateTimer)),
	m_ResetToDefaultStateDelay(snapshot.Freeze(source.m_ResetToDefaultStateDelay)),
	m_DrawMaterialLayerWhenOpen(snapshot.Freeze(source.m_DrawMaterialLayerWhenOpen)),
	m_DrawMaterialLayerWhenClosed(snapshot.Freeze(source.m_DrawMaterialLayerWhenClosed)),
	m_DoorMaterialID(snapshot.Freeze(source.m_DoorMaterialID)),
	m_DoorMaterialDrawn(snapshot.Freeze(source.m_DoorMaterialDrawn)),
	m_DoorMaterialTempErased(snapshot.Freeze(source.m_DoorMaterialTempErased)),
	m_DoorMaterialRedrawTimer(snapshot.Freeze(source.m_DoorMaterialRedrawTimer)),
	m_LastDoorMaterialPos(snapshot.Freeze(source.m_LastDoorMaterialPos)),
	m_DoorMoveStartSound(snapshot.Freeze(source.m_DoorMoveStartSound)),
	m_DoorMoveSound(snapshot.Freeze(source.m_DoorMoveSound)),
	m_DoorDirectionChangeSound(snapshot.Freeze(source.m_DoorDirectionChangeSound)),
	m_DoorMoveEndSound(snapshot.Freeze(source.m_DoorMoveEndSound)),
	m_PersistedADoorRuntime(snapshot.Freeze(source.m_PersistedADoorRuntime, &m_PersistedADoorRuntime)),
	m_CheckpointInitialized(snapshot.Freeze(source.m_CheckpointInitialized)) {
}

Entity* ADoor::FreezeCheckpointNative(CheckpointNativeSnapshot& snapshot) const {
	if (&GetClass() != &m_sClass) return Entity::FreezeCheckpointNative(snapshot);
	return snapshot.Make(*this);
}

ADoor::ADoor() {
	Clear();
}

ADoor::~ADoor() {
	if (IsFrozenCheckpointNative()) return;
	Destroy(true);
}

void ADoor::Clear() {
	CheckpointChange changed(*this, [this] {
		return CheckpointFields(
			m_CanBeSquished, m_ChangedDirectionAfterStop, m_ClosedAngle, m_ClosedByDefault, m_ClosedOffset, m_Door,
			m_DoorDirectionChangeSound, m_DoorMaterialDrawn, m_DoorMaterialID, m_DoorMaterialRedrawTimer, m_DoorMaterialTempErased, m_DoorMoveEndSound,
			m_DoorMoveSound, m_DoorMoveStartSound, m_DoorMoveStopTime, m_DoorMoveTime, m_DoorMoveTimer, m_DoorState,
			m_DoorStateOnStop, m_DrawMaterialLayerWhenClosed, m_DrawMaterialLayerWhenOpen, m_InitialSpriteAnimDuration, m_LastDoorMaterialPos, m_OpenAngle,
			m_OpenOffset, m_PersistedADoorRuntime.empty(), m_ResetToDefaultStateDelay, m_ResetToDefaultStateTimer, m_ResumeAfterStop, m_SensorInterval,
			m_SensorTimer, m_Sensors.empty());
	}, m_CheckpointInitialized);
	m_CheckpointInitialized = true;
	m_PersistedADoorRuntime.clear();
	m_InitialSpriteAnimDuration = 0;
	m_Sensors.clear();
	m_SensorTimer.Reset();
	m_SensorInterval = 1000;
	m_Door = nullptr;
	m_DoorState = CLOSED;
	m_DoorStateOnStop = CLOSED;
	m_ClosedByDefault = true;
	m_OpenOffset.Reset();
	m_ClosedOffset.Reset();
	m_OpenAngle = 0;
	m_ClosedAngle = 0;
	m_DoorMoveTimer.Reset();
	m_DoorMoveTime = 0;
	m_ResumeAfterStop = false;
	m_ChangedDirectionAfterStop = false;
	m_DoorMoveStopTime = 0;
	m_ResetToDefaultStateTimer.Reset();
	m_ResetToDefaultStateDelay = 5000;
	m_DrawMaterialLayerWhenOpen = true;
	m_DrawMaterialLayerWhenClosed = true;
	m_DoorMaterialID = g_MaterialDoor;
	m_DoorMaterialDrawn = false;
	m_DoorMaterialTempErased = false;
	m_DoorMaterialRedrawTimer.Reset();
	m_DoorMaterialRedrawTimer.SetSimTimeLimitMS(10000);
	// We need the door material layer to redraw right away when it's created, so set the door material redraw timer to be past its limit, thereby forcing an immediate redraw.
	m_DoorMaterialRedrawTimer.SetElapsedSimTimeMS(m_DoorMaterialRedrawTimer.GetSimTimeLimitMS() + 1);
	m_LastDoorMaterialPos.Reset();
	m_DoorMoveStartSound = nullptr;
	m_DoorMoveSound = nullptr;
	m_DoorDirectionChangeSound = nullptr;
	m_DoorMoveEndSound = nullptr;

	// NOTE: This special override of a parent class member variable avoids needing an extra variable to avoid overwriting INI values.
	m_CanBeSquished = false;
}

int ADoor::Create(const ADoor& reference) {
	if (reference.m_Door) {
		m_ReferenceHardcodedAttachableUniqueIDs.insert(reference.m_Door->GetUniqueID());
	}

	Actor::Create(reference);

	if (reference.m_Door) {
		SetDoor(dynamic_cast<Attachable*>(reference.m_Door->Clone()));
	}

	m_InitialSpriteAnimDuration = reference.m_SpriteAnimDuration;

	for (const ADSensor& sensor: reference.m_Sensors) {
		m_Sensors.push_back(sensor);
		m_Sensors.back().SetCheckpointOwner(this);
	}

	m_SensorInterval = reference.m_SensorInterval;
	m_DoorState = reference.m_DoorState;
	m_ClosedByDefault = reference.m_ClosedByDefault;
	m_OpenOffset = reference.m_OpenOffset;
	m_ClosedOffset = reference.m_ClosedOffset;
	m_OpenAngle = reference.m_OpenAngle;
	m_ClosedAngle = reference.m_ClosedAngle;
	m_DoorMoveTime = reference.m_DoorMoveTime;
	m_ResetToDefaultStateDelay = reference.m_ResetToDefaultStateDelay;
	m_DrawMaterialLayerWhenOpen = reference.m_DrawMaterialLayerWhenOpen;
	m_DrawMaterialLayerWhenClosed = reference.m_DrawMaterialLayerWhenClosed;
	m_DoorMaterialID = reference.m_DoorMaterialID;

	if (reference.m_DoorMoveStartSound) {
		m_DoorMoveStartSound.reset(dynamic_cast<SoundContainer*>(reference.m_DoorMoveStartSound->Clone()));
	}

	if (reference.m_DoorMoveSound) {
		m_DoorMoveSound.reset(dynamic_cast<SoundContainer*>(reference.m_DoorMoveSound->Clone()));
	}

	if (reference.m_DoorDirectionChangeSound) {
		m_DoorDirectionChangeSound.reset(dynamic_cast<SoundContainer*>(reference.m_DoorDirectionChangeSound->Clone()));
	}

	if (reference.m_DoorMoveEndSound) {
		m_DoorMoveEndSound.reset(dynamic_cast<SoundContainer*>(reference.m_DoorMoveEndSound->Clone()));
	}

	if (IsFaithfulClone()) {
		m_InitialSpriteAnimDuration = reference.m_InitialSpriteAnimDuration;
		m_SensorTimer = reference.m_SensorTimer;
		m_DoorStateOnStop = reference.m_DoorStateOnStop;
		m_DoorMoveTimer = reference.m_DoorMoveTimer;
		m_ResumeAfterStop = reference.m_ResumeAfterStop;
		m_ChangedDirectionAfterStop = reference.m_ChangedDirectionAfterStop;
		m_DoorMoveStopTime = reference.m_DoorMoveStopTime;
		m_ResetToDefaultStateTimer = reference.m_ResetToDefaultStateTimer;
		m_DoorMaterialDrawn = reference.m_DoorMaterialDrawn;
		m_DoorMaterialTempErased = reference.m_DoorMaterialTempErased;
		m_DoorMaterialRedrawTimer = reference.m_DoorMaterialRedrawTimer;
		m_LastDoorMaterialPos = reference.m_LastDoorMaterialPos;
	}
	m_PersistedADoorRuntime = reference.m_PersistedADoorRuntime;
	if (FaithfulCloneKeepsRuntime() && m_PersistedADoorRuntime.empty()) m_PersistedADoorRuntime = reference.SaveADoorRuntime();
	return 0;
}

int ADoor::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Actor::ReadProperty(propName, reader));
	MatchProperty("SpecialBehaviour_ADoorRuntime", {
		m_PersistedADoorRuntime = base64_decode(reader.ReadPropValue());
		if (!LoadADoorRuntime(m_PersistedADoorRuntime, true)) reader.ReportError("invalid ADoor runtime checkpoint");
	});

	MatchProperty("Door", { SetDoor(dynamic_cast<Attachable*>(g_PresetMan.ReadReflectedPreset(reader))); });
	MatchProperty("OpenOffset", { reader >> m_OpenOffset; });
	MatchProperty("ClosedOffset", { reader >> m_ClosedOffset; });
	MatchProperty("OpenClosedOffset", {
		reader >> m_OpenOffset;
		m_ClosedOffset = m_OpenOffset;
	});
	MatchProperty("OpenAngle", {
		Matrix rotation;
		reader >> rotation;
		m_OpenAngle = rotation.GetRadAngle();
		if (m_OpenAngle < 0) {
			reader.ReportError("Door OpenAngle cannot be less than 0.");
		}
	});
	MatchProperty("ClosedAngle", {
		Matrix rotation;
		reader >> rotation;
		m_ClosedAngle = rotation.GetRadAngle();
		if (m_ClosedAngle < 0) {
			reader.ReportError("Door ClosedAngle cannot be less than 0.");
		}
	});
	MatchProperty("OpenClosedAngle", {
		Matrix rotation;
		reader >> rotation;
		m_OpenAngle = rotation.GetRadAngle();
		m_ClosedAngle = rotation.GetRadAngle();
	});
	MatchProperty("DoorMoveTime", { reader >> m_DoorMoveTime; });
	MatchProperty("ClosedByDefault", { reader >> m_ClosedByDefault; });
	MatchProperty("ResetDefaultDelay", { reader >> m_ResetToDefaultStateDelay; });
	MatchProperty("SensorInterval", { reader >> m_SensorInterval; });
	MatchProperty("SpecialBehaviour_ClearSensors", { bool clear; reader >> clear; if (clear) m_Sensors.clear(); });
	MatchProperty("AddSensor", {
		ADSensor sensor;
		reader >> sensor;
		m_Sensors.push_back(sensor);
		m_Sensors.back().SetCheckpointOwner(this);
	});
	MatchProperty("DrawMaterialLayerWhenOpen", { reader >> m_DrawMaterialLayerWhenOpen; });
	MatchProperty("DrawMaterialLayerWhenClosed", { reader >> m_DrawMaterialLayerWhenClosed; });
	MatchProperty("DoorMoveStartSound", { m_DoorMoveStartSound.reset(dynamic_cast<SoundContainer*>(g_PresetMan.ReadReflectedPreset(reader))); });
	MatchProperty("DoorMoveSound", { m_DoorMoveSound.reset(dynamic_cast<SoundContainer*>(g_PresetMan.ReadReflectedPreset(reader))); });
	MatchProperty("DoorDirectionChangeSound", { m_DoorDirectionChangeSound.reset(dynamic_cast<SoundContainer*>(g_PresetMan.ReadReflectedPreset(reader))); });
	MatchProperty("DoorMoveEndSound", { m_DoorMoveEndSound.reset(dynamic_cast<SoundContainer*>(g_PresetMan.ReadReflectedPreset(reader))); });

	EndPropertyList;
}

void ADoor::SaveSnapshotConfiguration(Writer& writer) const {
	Actor::SaveSnapshotConfiguration(writer);
	writer.NewPropertyWithValue("SpecialBehaviour_ClearSensors", true);
	for (const ADSensor& sensor: m_Sensors) writer.NewPropertyWithValue("AddSensor", sensor);
	writer.NewPropertyWithValue("OpenOffset", m_OpenOffset);
	writer.NewPropertyWithValue("ClosedOffset", m_ClosedOffset);
	writer.NewPropertyWithValue("OpenAngle", Matrix(m_OpenAngle));
	writer.NewPropertyWithValue("ClosedAngle", Matrix(m_ClosedAngle));
	writer.NewPropertyWithValue("DoorMoveTime", m_DoorMoveTime);
	writer.NewPropertyWithValue("ClosedByDefault", m_ClosedByDefault);
	writer.NewPropertyWithValue("ResetDefaultDelay", m_ResetToDefaultStateDelay);
	writer.NewPropertyWithValue("SensorInterval", m_SensorInterval);
	writer.NewPropertyWithValue("DrawMaterialLayerWhenOpen", m_DrawMaterialLayerWhenOpen);
	writer.NewPropertyWithValue("DrawMaterialLayerWhenClosed", m_DrawMaterialLayerWhenClosed);
	writer.NewPropertyWithValue("DoorMoveStartSound", m_DoorMoveStartSound.get());
	writer.NewPropertyWithValue("DoorMoveSound", m_DoorMoveSound.get());
	writer.NewPropertyWithValue("DoorDirectionChangeSound", m_DoorDirectionChangeSound.get());
	writer.NewPropertyWithValue("DoorMoveEndSound", m_DoorMoveEndSound.get());
	writer.NewPropertyWithValue("SpecialBehaviour_ADoorRuntime", CheckpointWriter::Native([&] { return m_PersistedADoorRuntime.empty() ? SaveADoorRuntime() : m_PersistedADoorRuntime.Value(); }).Base64(true));
}

int ADoor::Save(Writer& writer) const {
	Actor::Save(writer);

	writer.NewProperty("Door");
	writer << m_Door;
	writer.NewProperty("OpenOffset");
	writer << m_OpenOffset;
	writer.NewProperty("ClosedOffset");
	writer << m_ClosedOffset;
	writer.NewProperty("OpenAngle");
	writer << Matrix(m_OpenAngle);
	writer.NewProperty("ClosedAngle");
	writer << Matrix(m_ClosedAngle);
	writer.NewProperty("DoorDelay");
	writer << m_DoorMoveTime;
	writer.NewProperty("ClosedByDefault");
	writer << m_ClosedByDefault;
	writer.NewProperty("ResetDefaultDelay");
	writer << m_ResetToDefaultStateDelay;
	writer.NewProperty("SensorInterval");
	writer << m_SensorInterval;
	for (const ADSensor& sensor: m_Sensors) {
		writer.NewProperty("AddSensor");
		writer << sensor;
	}
	writer.NewProperty("DrawMaterialLayerWhenOpen");
	writer << m_DrawMaterialLayerWhenOpen;
	writer.NewProperty("DrawMaterialLayerWhenClosed");
	writer << m_DrawMaterialLayerWhenClosed;
	writer.NewProperty("DoorMoveStartSound");
	writer << m_DoorMoveStartSound.get();
	writer.NewProperty("DoorMoveSound");
	writer << m_DoorMoveSound.get();
	writer.NewProperty("DoorDirectionChangeSound");
	writer << m_DoorDirectionChangeSound.get();
	writer.NewProperty("DoorMoveEndSound");
	writer << m_DoorMoveEndSound.get();

	return 0;
}

void ADoor::Destroy(bool notInherited) {
	if (m_DoorMoveStartSound) {
		m_DoorMoveStartSound->Stop();
	}

	if (m_DoorMoveSound) {
		m_DoorMoveSound->Stop();
	}

	if (m_DoorDirectionChangeSound) {
		m_DoorDirectionChangeSound->Stop();
	}

	if (m_DoorMoveEndSound) {
		m_DoorMoveEndSound->Stop();
	}

	if (!notInherited) {
		Actor::Destroy();
	}

	for (ADSensor& sensor: m_Sensors) {
		sensor.Destroy();
	}

	if (m_DoorMaterialDrawn) {
		EraseDoorMaterial();
	}

	Clear();
}

void ADoor::SetDoor(Attachable* newDoor) {
	CheckpointChange changed(*this, [this] { return CheckpointFields(m_Door, m_DoorMaterialID); });
	if (m_DoorMaterialDrawn) {
		RTEAssert(m_Door, "Door material drawn without an m_Door! This should've been cleared when the door was!");
		EraseDoorMaterial();
	}

	if (m_Door && m_Door->IsAttached()) {
		RemoveAndDeleteAttachable(m_Door);
	}

	m_Door = newDoor;
	if (m_Door) {
		AddAttachable(m_Door);

		m_HardcodedAttachableUniqueIDsAndSetters.insert({m_Door->GetUniqueID(), [](MOSRotating* parent, Attachable* attachable) {
			                                                 dynamic_cast<ADoor*>(parent)->SetDoor(attachable);
		                                                 }});

		m_Door->SetInheritsRotAngle(false);
		m_DoorMaterialID = m_Door->GetMaterial()->GetIndex();
	}
}

void ADoor::DrawDoorMaterial(bool disallowErasingMaterialBeforeDrawing, bool updateMaterialArea) {
	if (!m_Door || m_DoorMaterialTempErased || !g_SceneMan.GetTerrain() || !g_SceneMan.GetTerrain()->GetMaterialBitmap()) {
		return;
	}

	if (!disallowErasingMaterialBeforeDrawing && m_DoorMaterialDrawn) {
		EraseDoorMaterial(updateMaterialArea);
	}

	m_Door->Draw(g_SceneMan.GetTerrain()->GetMaterialBitmap(), Vector(), g_DrawDoor, true);
	m_LastDoorMaterialPos = m_Door->GetPos();
	m_DoorMaterialDrawn = true;

	if (updateMaterialArea) {
		g_SceneMan.GetTerrain()->AddUpdatedMaterialArea(m_Door->GetBoundingBox());
	}
}

bool ADoor::EraseDoorMaterial(bool updateMaterialArea) {
	if (!g_SceneMan.GetTerrain() || !g_SceneMan.GetTerrain()->GetMaterialBitmap()) {
		return false;
	}

	int fillX = m_LastDoorMaterialPos.GetFloorIntX();
	int fillY = m_LastDoorMaterialPos.GetFloorIntY();

	DrawDoorMaterial(true, updateMaterialArea);
	m_DoorMaterialDrawn = false;

	if (g_SceneMan.GetTerrMatter(fillX, fillY) != g_MaterialAir) {
		floodfill(g_SceneMan.GetTerrain()->GetMaterialBitmap(), fillX, fillY, g_MaterialAir);

		if (m_Door && updateMaterialArea) {
			g_SceneMan.GetTerrain()->AddUpdatedMaterialArea(m_Door->GetBoundingBox());
		}

		return true;
	}

	return false;
}

void ADoor::SetDoorMoveStartSound(SoundContainer* newSound) {
	m_DoorMoveStartSound.reset(newSound);
}

void ADoor::SetDoorMoveSound(SoundContainer* newSound) {
	m_DoorMoveSound.reset(newSound);
}

void ADoor::SetDoorDirectionChangeSound(SoundContainer* newSound) {
	m_DoorDirectionChangeSound.reset(newSound);
}

void ADoor::SetDoorMoveEndSound(SoundContainer* newSound) {
	m_DoorMoveEndSound.reset(newSound);
}

void ADoor::TempEraseOrRedrawDoorMaterial(bool erase) {
	if (!g_SceneMan.GetTerrain() || !g_SceneMan.GetTerrain()->GetMaterialBitmap()) {
		return;
	}

	bool doorMaterialDrawnState = m_DoorMaterialDrawn;
	if (erase && m_DoorMaterialDrawn && !m_DoorMaterialTempErased) {
		m_DoorMaterialTempErased = erase;
		// Each machine updates its paths on its own ticks, so in a match the override must leave the terrain exactly as it found it.
		if (ScenarioRunner::LockstepCoordinatorAttached()) {
			TempEraseDoorMaterialRestorably();
		} else {
			EraseDoorMaterial(false);
		}
		m_DoorMaterialDrawn = doorMaterialDrawnState;
	} else if (!erase && m_DoorMaterialDrawn && m_DoorMaterialTempErased) {
		m_DoorMaterialTempErased = erase;
		if (m_TempErasedDoorMaterial) {
			RestoreTempErasedDoorMaterial();
		} else {
			DrawDoorMaterial(true, false);
		}
		m_DoorMaterialDrawn = doorMaterialDrawnState;
	}
}

void ADoor::TempEraseDoorMaterialRestorably() {
	m_TempErasedDoorMaterial.emplace();
	BITMAP* materialBitmap = g_SceneMan.GetTerrain()->GetMaterialBitmap();
	const int fillX = m_LastDoorMaterialPos.GetFloorIntX();
	const int fillY = m_LastDoorMaterialPos.GetFloorIntY();
	if (!m_Door || fillX < 0 || fillY < 0 || fillX >= materialBitmap->w || fillY >= materialBitmap->h) {
		return;
	}
	const unsigned char material = static_cast<unsigned char>(_getpixel(materialBitmap, fillX, fillY));
	if (material == g_MaterialAir) {
		return;
	}

	// The door's material lies inside its sprite bounds; the fill stops there instead of following connected terrain.
	const float reach = std::ceil(m_Door->GetRadius() * std::max(1.0F, m_Door->GetScale())) + 1.0F;
	const int left = std::max(0, static_cast<int>(std::floor(m_LastDoorMaterialPos.GetX() - reach)));
	const int top = std::max(0, static_cast<int>(std::floor(m_LastDoorMaterialPos.GetY() - reach)));
	const int right = std::min(materialBitmap->w, static_cast<int>(std::ceil(m_LastDoorMaterialPos.GetX() + reach)) + 1);
	const int bottom = std::min(materialBitmap->h, static_cast<int>(std::ceil(m_LastDoorMaterialPos.GetY() + reach)) + 1);
	const int width = right - left;
	const int height = bottom - top;

	BITMAP* footprint = create_bitmap_ex(8, width, height);
	if (!footprint) {
		return;
	}
	clear_to_color(footprint, g_MaterialAir);
	// Drawn on each machine's own ticks, so the silhouette must stay out of the MOID grid.
	const bool renderDraw = g_SceneMan.IsRenderDrawContext();
	g_SceneMan.SetRenderDrawContext(true);
	m_Door->Draw(footprint, Vector(static_cast<float>(left), static_cast<float>(top)) + m_Door->GetPos() - m_LastDoorMaterialPos, g_DrawDoor, true);
	g_SceneMan.SetRenderDrawContext(renderDraw);

	// Only the door's own shape opens: the terrain is copied inside it and walled off with air outside it.
	std::vector<bool> inside(static_cast<size_t>(width) * height);
	for (int y = 0; y < height; ++y) {
		unsigned char* row = footprint->line[y];
		const unsigned char* terrain = materialBitmap->line[top + y] + left;
		for (int x = 0; x < width; ++x) {
			inside[static_cast<size_t>(y) * width + x] = row[x] != g_MaterialAir;
			row[x] = row[x] != g_MaterialAir ? terrain[x] : g_MaterialAir;
		}
	}
	floodfill(footprint, fillX - left, fillY - top, g_MaterialAir);

	m_TempErasedDoorMaterial->material = material;
	for (int y = 0; y < height; ++y) {
		const unsigned char* row = footprint->line[y];
		for (int x = 0; x < width; ++x) {
			if (inside[static_cast<size_t>(y) * width + x] && row[x] != _getpixel(materialBitmap, left + x, top + y)) {
				_putpixel(materialBitmap, left + x, top + y, g_MaterialAir);
				m_TempErasedDoorMaterial->pixels.emplace_back(left + x, top + y);
			}
		}
	}
	destroy_bitmap(footprint);
}

void ADoor::RestoreTempErasedDoorMaterial() {
	BITMAP* materialBitmap = g_SceneMan.GetTerrain()->GetMaterialBitmap();
	for (const auto& [x, y]: m_TempErasedDoorMaterial->pixels) {
		_putpixel(materialBitmap, x, y, m_TempErasedDoorMaterial->material);
	}
	m_TempErasedDoorMaterial.reset();
}

void ADoor::GibThis(const Vector& impactImpulse, MovableObject* movableObjectToIgnore) {
	if (m_Door && m_Door->IsAttached()) {
		EraseDoorMaterial();
		m_Door->DeepCheck(true);
		m_Door->SetPinStrength(0);
	}

	Actor::GibThis(impactImpulse, movableObjectToIgnore);
}

void ADoor::CorrectAttachableAndWoundPositionsAndRotations() const {
	if (m_Door) {
		m_Door->SetParentOffset(m_ClosedByDefault ? m_ClosedOffset : m_OpenOffset);
		m_Door->SetRotAngle(m_Rotation.GetRadAngle() + (m_ClosedByDefault ? m_ClosedAngle : m_OpenAngle) * GetFlipFactor());
	}

	MOSRotating::CorrectAttachableAndWoundPositionsAndRotations();
}

void ADoor::OpenDoor() {
	if (m_DoorState == STOPPED) {
		SharedDoorControls();
		m_DoorState = CLOSING;
		m_ChangedDirectionAfterStop = (m_DoorState == m_DoorStateOnStop) ? false : true;
	}

	if (m_DoorState == CLOSED || m_DoorState == CLOSING) {
		SharedDoorControls();
		m_DoorState = OPENING;
	}

	m_ResetToDefaultStateTimer.Reset();
}

void ADoor::CloseDoor() {
	if (m_DoorState == STOPPED) {
		SharedDoorControls();
		m_DoorState = OPENING;
		m_ChangedDirectionAfterStop = (m_DoorState == m_DoorStateOnStop) ? false : true;
	}

	if (m_DoorState == OPEN || m_DoorState == OPENING) {
		SharedDoorControls();
		m_DoorState = CLOSING;
	}

	m_ResetToDefaultStateTimer.Reset();
}

void ADoor::StopDoor() {
	if (m_DoorState == OPENING || m_DoorState == CLOSING) {
		if (m_DrawMaterialLayerWhenOpen || m_DrawMaterialLayerWhenClosed) {
			DrawDoorMaterial(true);
		}

		m_DoorMoveStopTime = m_DoorMoveTime - m_DoorMoveTimer.GetElapsedSimTimeMS();
		m_DoorStateOnStop = m_DoorState;
		if (m_DoorMoveSound) {
			m_DoorMoveSound->Stop();
		}

		if (m_DoorMoveEndSound) {
			m_DoorMoveEndSound->Play(m_Pos);
		}

		m_DoorState = STOPPED;
	}
}

void ADoor::SharedDoorControls() {
	if (m_DoorState == OPEN || m_DoorState == CLOSED) {
		m_DoorMoveTimer.Reset();
		if (m_DoorMoveStartSound) {
			m_DoorMoveStartSound->Play(m_Pos);
		}

		if (m_DoorMaterialDrawn) {
			EraseDoorMaterial();
		}

		if (m_Door) {
			m_Door->DeepCheck(true);
		}
	} else if (m_DoorState == OPENING || m_DoorState == CLOSING) {
		if (m_DoorMoveSound) {
			m_DoorMoveSound->Stop();
		}

		if (!m_ResumeAfterStop) {
			if (m_DoorDirectionChangeSound) {
				m_DoorDirectionChangeSound->Play(m_Pos);
			}

			m_DoorMoveTimer.SetElapsedSimTimeMS(m_DoorMoveTime - m_DoorMoveTimer.GetElapsedSimTimeMS());
		} else {
			if (m_DoorMoveStartSound) {
				m_DoorMoveStartSound->Play(m_Pos);
			}

			m_DoorMoveTimer.SetElapsedSimTimeMS(m_ChangedDirectionAfterStop ? m_DoorMoveTime - m_DoorMoveStopTime : m_DoorMoveStopTime);
			m_ChangedDirectionAfterStop = false;
			m_ResumeAfterStop = false;
		}
	} else if (m_DoorState == STOPPED) {
		if (m_DoorMaterialDrawn) {
			EraseDoorMaterial();
		}

		m_ResumeAfterStop = true;
	}
}

void ADoor::Update() {
	ZoneScoped;

	if (m_Door) {
		if (m_DoorState != STOPPED && m_Status != Actor::Status::INACTIVE && m_SensorTimer.IsPastSimMS(m_SensorInterval)) {
			UpdateSensors();
		}

		UpdateDoorAttachableActions();

		if (DoorDigAgeMs() >= 0) {
			DigDoorForTest(*this);
		}

		bool shouldDrawDoorMaterial = ((m_DrawMaterialLayerWhenOpen && m_DoorState == OPEN) ||
		                               (m_DrawMaterialLayerWhenClosed && m_DoorState == CLOSED) ||
		                               ((m_DrawMaterialLayerWhenOpen || m_DrawMaterialLayerWhenClosed) && m_DoorState == STOPPED)) &&
		                              m_DoorMaterialRedrawTimer.IsPastSimTimeLimit();
		if (shouldDrawDoorMaterial) {
			DrawDoorMaterial(true);
			m_DoorMaterialRedrawTimer.Reset();
		} else if (m_DoorMaterialDrawn && MatchPathUpdateResealDue(m_Team)) {
			DrawDoorMaterial(true, false);
		}

		if (DoorDigAgeMs() >= 0) {
			NoteDoorResealForTest(*this);
		}
	}

	Actor::Update();

	if (m_SpriteAnimMode == LOOPWHENOPENCLOSE && m_FrameCount > 1 && (m_DoorState == OPENING || m_DoorState == CLOSING) && m_SpriteAnimTimer.IsPastSimMS(m_SpriteAnimDuration)) {
		m_Frame = (m_Frame + 1) % m_FrameCount;
		m_SpriteAnimTimer.Reset();
	}

	// Lose health when door is lost, spinning out of control until grinds to halt
	if (!m_Door && m_Status != DYING && m_Status != DEAD) {
		m_SpriteAnimMode = ALWAYSLOOP;
		m_SpriteAnimDuration = static_cast<int>(Lerp(0, m_MaxHealth, 10.0F, static_cast<float>(m_InitialSpriteAnimDuration), m_Health));

		if (m_DoorMoveSound) {
			if (!m_DoorMoveSound->IsBeingPlayed()) {
				m_DoorMoveSound->Play(m_Pos);
			}
			m_DoorMoveSound->SetPitch(Lerp(10.0F, static_cast<float>(m_InitialSpriteAnimDuration), 2.0F, 1.0F, static_cast<float>(m_SpriteAnimDuration)));
		}

		m_Health -= 0.4F;
	}

	if (m_Status == DEAD) {
		GibThis();
	}
}

void ADoor::UpdateSensors() {
	const Actor* foundActor = nullptr;
	bool anySensorInput = false;

	for (ADSensor& sensor: m_Sensors) {
		foundActor = sensor.SenseActor(m_Pos, m_Rotation, m_HFlipped, m_MOID);
		if (foundActor && foundActor->IsControllable()) {
			anySensorInput = true;

			if (m_Team == Activity::NoTeam) {
				OpenDoor();
				break;
				// If a sensor has found an enemy Actor, close the door and stop looking, so we don't accidentally open it for a friendly Actor.
			} else if (foundActor->GetTeam() != m_Team) {
				CloseDoor();
				break;
			} else if (foundActor->GetTeam() == m_Team) {
				OpenDoor();
			}
		}
	}
	if (!anySensorInput && m_ResetToDefaultStateTimer.IsPastSimMS(m_ResetToDefaultStateDelay)) {
		if (m_ClosedByDefault && m_DoorState == OPEN) {
			CloseDoor();
		} else if (!m_ClosedByDefault && m_DoorState == CLOSED) {
			OpenDoor();
		}
	}
	m_SensorTimer.Reset();
}

void ADoor::UpdateDoorAttachableActions() {
	Vector startOffset;
	Vector endOffset;
	float startAngle;
	float endAngle;

	if (m_DoorState == OPEN || m_DoorState == OPENING) {
		startOffset = m_ClosedOffset;
		endOffset = m_OpenOffset;
		startAngle = m_ClosedAngle;
		endAngle = m_OpenAngle;
	} else if (m_DoorState == CLOSED || m_DoorState == CLOSING) {
		startOffset = m_OpenOffset;
		endOffset = m_ClosedOffset;
		startAngle = m_OpenAngle;
		endAngle = m_ClosedAngle;
	}

	if (m_DoorState == OPEN || m_DoorState == CLOSED) {
		m_Door->SetParentOffset(endOffset);
		m_Door->SetRotAngle(m_Rotation.GetRadAngle() + (endAngle * GetFlipFactor()));
	} else if (m_DoorState == OPENING || m_DoorState == CLOSING) {
		if (m_DoorMoveSound && !m_DoorMoveSound->IsBeingPlayed()) {
			m_DoorMoveSound->Play(m_Pos);
		}

		if (m_DoorMoveTimer.IsPastSimMS(m_DoorMoveTime)) {
			m_ResetToDefaultStateTimer.Reset();

			m_Door->SetParentOffset(endOffset);
			m_Door->SetRotAngle(m_Rotation.GetRadAngle() + (endAngle * GetFlipFactor()));

			if (m_DoorMoveSound) {
				m_DoorMoveSound->Stop();
			}

			if (m_DoorMoveEndSound) {
				m_DoorMoveEndSound->Play(m_Pos);
			}

			if (m_DoorState == OPENING) {
				if (m_DrawMaterialLayerWhenOpen) {
					DrawDoorMaterial();
				}

				m_DoorState = OPEN;
				if (MetricsCollector::IsConstructed() && g_MetricsCollector.EventsEnabled()) g_MetricsCollector.RecordEvent("door_open_completed", this);
			} else if (m_DoorState == CLOSING) {
				if (m_DrawMaterialLayerWhenClosed) {
					DrawDoorMaterial();
				}

				m_DoorState = CLOSED;
				if (MetricsCollector::IsConstructed() && g_MetricsCollector.EventsEnabled()) g_MetricsCollector.RecordEvent("door_close_completed", this);
			}
		} else {
			Vector updatedOffset(Lerp(0, m_DoorMoveTime, startOffset.m_X, endOffset.m_X, m_DoorMoveTimer.GetElapsedSimTimeMS()), Lerp(0, m_DoorMoveTime, startOffset.m_Y, endOffset.m_Y, m_DoorMoveTimer.GetElapsedSimTimeMS()));
			float updatedAngle = Lerp(0, m_DoorMoveTime, Matrix(startAngle), Matrix(endAngle), m_DoorMoveTimer.GetElapsedSimTimeMS()).GetRadAngle();

			m_Door->SetParentOffset(updatedOffset);
			m_Door->SetRotAngle(m_Rotation.GetRadAngle() + (updatedAngle * GetFlipFactor()));

			// Clear away any terrain debris when the door is moving but only after a short delay so it doesn't take a chunk out of the ground.
			if (m_DoorMoveTimer.IsPastSimMS(50)) {
				m_Door->DeepCheck(true);
			}
		}
	}
}

void ADoor::DrawHUD(BITMAP* targetBitmap, const Vector& targetPos, int whichScreen, bool playerControlled) {
	m_HUDStack = -static_cast<int>(m_CharHeight) / 2;

	if (!m_HUDVisible) {
		return;
	}

	// Only draw if the team viewing this is on the same team OR has seen the space where this is located.
	int viewingTeam = g_ActivityMan.GetActivity()->GetTeamOfPlayer(g_ActivityMan.GetActivity()->PlayerOfScreen(whichScreen));
	if (viewingTeam != m_Team && viewingTeam != Activity::NoTeam && (!g_SettingsMan.ShowEnemyHUD() || g_SceneMan.IsUnseen(m_Pos.GetFloorIntX(), m_Pos.GetFloorIntY(), viewingTeam))) {
		return;
	}
}

void ADoor::AdoptPersistedUniqueID() {
	Actor::AdoptPersistedUniqueID();
	if (!m_PersistedADoorRuntime.empty()) {
		if (!LoadADoorRuntime(m_PersistedADoorRuntime)) throw std::runtime_error("could not restore ADoor runtime checkpoint");
		m_PersistedADoorRuntime.clear();
	}
}

void ADoor::DiscardPersistedSnapshotState() {
	Actor::DiscardPersistedSnapshotState();
	m_PersistedADoorRuntime.clear();
}

std::string ADoor::SaveADoorRuntime() const {
	CheckpointWriter archive("ADoorRuntime1");
	archive(m_InitialSpriteAnimDuration, m_SensorTimer, m_SensorInterval, m_DoorState, m_DoorStateOnStop, m_ClosedByDefault, m_OpenOffset);
	archive(m_ClosedOffset, m_OpenAngle, m_ClosedAngle, m_DoorMoveTimer, m_DoorMoveTime, m_ResumeAfterStop, m_ChangedDirectionAfterStop);
	archive(m_DoorMoveStopTime, m_ResetToDefaultStateTimer, m_ResetToDefaultStateDelay, m_DrawMaterialLayerWhenOpen, m_DrawMaterialLayerWhenClosed, m_DoorMaterialID, m_DoorMaterialDrawn);
	archive(m_DoorMaterialTempErased, m_DoorMaterialRedrawTimer, m_LastDoorMaterialPos);
	return archive.Text();
}

bool ADoor::LoadADoorRuntime(std::string_view text, bool validateOnly) {
	try {
		CheckpointReader archive(text, "ADoorRuntime1", validateOnly);
		archive(m_InitialSpriteAnimDuration, m_SensorTimer, m_SensorInterval, m_DoorState, m_DoorStateOnStop, m_ClosedByDefault, m_OpenOffset);
		archive(m_ClosedOffset, m_OpenAngle, m_ClosedAngle, m_DoorMoveTimer, m_DoorMoveTime, m_ResumeAfterStop, m_ChangedDirectionAfterStop);
		archive(m_DoorMoveStopTime, m_ResetToDefaultStateTimer, m_ResetToDefaultStateDelay, m_DrawMaterialLayerWhenOpen, m_DrawMaterialLayerWhenClosed, m_DoorMaterialID, m_DoorMaterialDrawn);
		archive(m_DoorMaterialTempErased, m_DoorMaterialRedrawTimer, m_LastDoorMaterialPos);
		archive.Finish();
		return true;
	} catch (const std::exception&) { return false; }
}
