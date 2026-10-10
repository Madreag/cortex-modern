#include "Gib.h"
#include "CheckpointNativeSnapshot.h"
#include "Vector.h"
#include "CheckpointProperties.h"
#include "PresetMan.h"
#include "MovableObject.h"
#include "MovableMan.h"
#include "MOSRotating.h"

using namespace RTE;

const std::string Gib::c_ClassName = "Gib";

Gib::Gib(const Gib& source, CheckpointNativeSnapshot& snapshot) :
	m_GibParticle(snapshot.Freeze(source.m_GibParticle)),
	m_PersistedParticleUniqueID(snapshot.Freeze(source.m_PersistedParticleUniqueID)),
	m_Offset(snapshot.Freeze(source.m_Offset)),
	m_Count(snapshot.Freeze(source.m_Count)),
	m_Spread(snapshot.Freeze(source.m_Spread)),
	m_MinVelocity(snapshot.Freeze(source.m_MinVelocity)),
	m_MaxVelocity(snapshot.Freeze(source.m_MaxVelocity)),
	m_LifeVariation(snapshot.Freeze(source.m_LifeVariation)),
	m_InheritsVel(snapshot.Freeze(source.m_InheritsVel)),
	m_InheritsAngularVel(snapshot.Freeze(source.m_InheritsAngularVel)),
	m_IgnoresTeamHits(snapshot.Freeze(source.m_IgnoresTeamHits)),
	m_SpreadMode(snapshot.Freeze(source.m_SpreadMode)),
	m_CheckpointOwner(nullptr) {
}

Gib::Gib() {
	Clear();
}

Gib::~Gib() {
	Destroy();
}

void Gib::TouchCheckpoint() {
	if (m_CheckpointOwner) m_CheckpointOwner->TouchCheckpoint();
}

void Gib::Clear() {
	CheckpointChange changed(*this, [this] { return CheckpointFields(m_GibParticle, m_PersistedParticleUniqueID, m_Offset, m_Count, m_Spread, m_MinVelocity, m_MaxVelocity, m_LifeVariation, m_InheritsVel, m_InheritsAngularVel, m_IgnoresTeamHits, m_SpreadMode); }, m_CheckpointOwner != nullptr);
	m_GibParticle = nullptr;
	m_PersistedParticleUniqueID = 0;
	m_Offset.Reset();
	m_Count = 1;
	m_Spread = 0.1F;
	m_MinVelocity = 0;
	m_MaxVelocity = 0;
	m_LifeVariation = 0.1F;
	m_InheritsVel = 1.0F;
	m_InheritsAngularVel = 1.0F;
	m_IgnoresTeamHits = false;
	m_SpreadMode = SpreadMode::SpreadRandom;
}

int Gib::Create(const Gib& reference) {
	m_GibParticle = reference.m_GibParticle;
	m_PersistedParticleUniqueID = reference.m_PersistedParticleUniqueID;
	if (MovableObject::IsFaithfulClone() && m_GibParticle && !m_GibParticle->IsOriginalPreset()) {
		m_PersistedParticleUniqueID = m_GibParticle->GetUniqueID();
		m_GibParticle = nullptr;
	}
	m_Offset = reference.m_Offset;
	m_Count = reference.m_Count;
	m_Spread = reference.m_Spread;
	m_MinVelocity = reference.m_MinVelocity;
	m_MaxVelocity = reference.m_MaxVelocity;
	m_LifeVariation = reference.m_LifeVariation;
	m_InheritsVel = reference.m_InheritsVel;
	m_InheritsAngularVel = reference.m_InheritsAngularVel;
	m_IgnoresTeamHits = reference.m_IgnoresTeamHits;
	m_SpreadMode = reference.m_SpreadMode;

	return 0;
}

int Gib::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("GibParticle", {
		m_GibParticle = dynamic_cast<const MovableObject*>(g_PresetMan.GetEntityPreset(reader));
		RTEAssert(m_GibParticle, "Stream suggests allocating an unallocable type in Gib::Create!");
		m_PersistedParticleUniqueID = 0;
	});
	MatchProperty("SpecialBehaviour_ParticleUniqueID", { reader >> m_PersistedParticleUniqueID; m_GibParticle = nullptr; });
	MatchProperty("SpecialBehaviour_ClearParticle", { bool clear; reader >> clear; if (clear) SetParticlePreset(nullptr); });
	MatchProperty("Offset", { reader >> m_Offset; });
	MatchProperty("Count", { reader >> m_Count; });
	MatchProperty("Spread", { reader >> m_Spread; });
	MatchProperty("MinVelocity", { reader >> m_MinVelocity; });
	MatchProperty("MaxVelocity", { reader >> m_MaxVelocity; });
	MatchProperty("LifeVariation", { reader >> m_LifeVariation; });
	MatchProperty("InheritsVel", { reader >> m_InheritsVel; });
	MatchProperty("InheritsAngularVel", { reader >> m_InheritsAngularVel; });
	MatchProperty("IgnoresTeamHits", { reader >> m_IgnoresTeamHits; });
	MatchProperty("SpreadMode", { m_SpreadMode = static_cast<SpreadMode>(std::stoi(reader.ReadPropValue())); });

	EndPropertyList;
}

int Gib::Save(Writer& writer) const {
	Serializable::Save(writer);

	const long particleID = m_GibParticle && !m_GibParticle->IsOriginalPreset() ? m_GibParticle->GetUniqueID() : m_PersistedParticleUniqueID;
	if (particleID > 0) {
		writer.NewPropertyWithValue("SpecialBehaviour_ParticleUniqueID", particleID);
	} else if (m_GibParticle && m_GibParticle->IsOriginalPreset()) {
		writer.NewProperty("GibParticle");
		writer.ObjectStart(m_GibParticle->GetClassName());
		const Entity* preset = m_GibParticle->GetPresetForCopy();
		writer.NewPropertyWithValue("CopyOf", preset ? preset->GetModuleAndPresetName() : m_GibParticle->GetModuleAndPresetName());
		writer.ObjectEnd();
	} else {
		writer.NewPropertyWithValue("SpecialBehaviour_ClearParticle", true);
	}
	WriteCapturedProperties(writer,
		CheckpointProperty<"Offset">(m_Offset),
		CheckpointProperty<"Count">(m_Count),
		CheckpointProperty<"Spread">(m_Spread),
		CheckpointProperty<"MinVelocity">(m_MinVelocity),
		CheckpointProperty<"MaxVelocity">(m_MaxVelocity),
		CheckpointProperty<"LifeVariation">(m_LifeVariation),
		CheckpointProperty<"InheritsVel">(m_InheritsVel),
		CheckpointProperty<"InheritsAngularVel">(m_InheritsAngularVel),
		CheckpointProperty<"IgnoresTeamHits">(m_IgnoresTeamHits),
		CheckpointProperty<"SpreadMode">(m_SpreadMode));

	return 0;
}

void Gib::ResolveParticlePreset() {
	if (m_PersistedParticleUniqueID > 0) {
		if (const MovableObject* particle = g_MovableMan.FindObjectByUniqueID(m_PersistedParticleUniqueID)) {
			SetParticlePreset(particle);
		}
	}
}
