#include "Round.h"
#include "CheckpointNativeSnapshot.h"
#include "SoundContainer.h"
#include "MovableObject.h"
#include "PresetMan.h"
#include "MOPixel.h"

using namespace RTE;

ConcreteClassInfo(Round, Entity, 500);

Round::Round(const Round& source, CheckpointNativeSnapshot& snapshot) :
	Entity(source, snapshot),
	m_Particle(snapshot.Freeze(source.m_Particle)),
	m_ParticleCount(snapshot.Freeze(source.m_ParticleCount)),
	m_FireVel(snapshot.Freeze(source.m_FireVel)),
	m_InheritsFirerVelocity(snapshot.Freeze(source.m_InheritsFirerVelocity)),
	m_Separation(snapshot.Freeze(source.m_Separation)),
	m_LifeVariation(snapshot.Freeze(source.m_LifeVariation)),
	m_Shell(snapshot.Freeze(source.m_Shell)),
	m_ShellVel(snapshot.Freeze(source.m_ShellVel)),
	m_FireSound(snapshot.Freeze(source.m_FireSound)),
	m_AILifeTime(snapshot.Freeze(source.m_AILifeTime)),
	m_AIFireVel(snapshot.Freeze(source.m_AIFireVel)),
	m_AIPenetration(snapshot.Freeze(source.m_AIPenetration)),
	m_CheckpointInitialized(snapshot.Freeze(source.m_CheckpointInitialized)) {
}

Entity* Round::FreezeCheckpointNative(CheckpointNativeSnapshot& snapshot) const {
	if (&GetClass() != &m_sClass) return Entity::FreezeCheckpointNative(snapshot);
	return snapshot.Make(*this);
}

Round::Round() {
	Clear();
}

Round::~Round() {
	if (IsFrozenCheckpointNative()) return;
	Destroy(true);
}

void Round::Clear() {
	CheckpointChange changed(*this, [this] { return CheckpointFields(m_Particle, m_ParticleCount, m_FireVel, m_InheritsFirerVelocity, m_Separation, m_LifeVariation, m_Shell, m_ShellVel, m_FireSound, m_AILifeTime, m_AIFireVel, m_AIPenetration); }, m_CheckpointInitialized);
	m_CheckpointInitialized = true;
	m_Particle = 0;
	m_ParticleCount = 0;
	m_FireVel = 0;
	m_InheritsFirerVelocity = false;
	m_Separation = 0;
	m_LifeVariation = 0;
	m_Shell = 0;
	m_ShellVel = 0;
	m_FireSound.Reset();
	m_AILifeTime = 0;
	m_AIFireVel = -1;
	m_AIPenetration = -1;
}

int Round::Create() {
	if (Entity::Create() < 0) {
		return -1;
	}

	if (m_AILifeTime == 0) {
		const MovableObject* bullet = GetNextParticle();
		if (bullet) {
			m_AILifeTime = bullet->GetLifetime();
		}
	}
	if (m_AIFireVel < 0) {
		m_AIFireVel = m_FireVel;
	}

	if (m_AIPenetration < 0) {
		const MovableObject* bullet = GetNextParticle();
		m_AIPenetration = (bullet && dynamic_cast<const MOPixel*>(bullet)) ? bullet->GetMass() * bullet->GetSharpness() * m_AIFireVel : 0;
	}

	return 0;
}

int Round::Create(const Round& reference) {
	Entity::Create(reference);

	m_Particle = reference.m_Particle;
	m_ParticleCount = reference.m_ParticleCount;
	m_InheritsFirerVelocity = reference.m_InheritsFirerVelocity;
	m_FireVel = reference.m_FireVel;
	m_Separation = reference.m_Separation;
	m_LifeVariation = reference.m_LifeVariation;
	m_Shell = reference.m_Shell;
	m_ShellVel = reference.m_ShellVel;
	m_FireSound = reference.m_FireSound;
	m_AILifeTime = reference.m_AILifeTime;
	m_AIFireVel = reference.m_AIFireVel;
	m_AIPenetration = reference.m_AIPenetration;

	return 0;
}

int Round::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Entity::ReadProperty(propName, reader));

	MatchProperty("Particle", {
		m_Particle = dynamic_cast<const MovableObject*>(g_PresetMan.GetEntityPreset(reader));
		RTEAssert(m_Particle, "Stream suggests allocating an unallocable type in Round::Create!");
	});
	MatchProperty("ParticleCount", { reader >> m_ParticleCount; });
	MatchProperty("FireVelocity", { reader >> m_FireVel; });
	MatchProperty("InheritsFirerVelocity", { reader >> m_InheritsFirerVelocity; });
	MatchProperty("Separation", { reader >> m_Separation; });
	MatchProperty("LifeVariation", { reader >> m_LifeVariation; });
	MatchProperty("Shell", { m_Shell = dynamic_cast<const MovableObject*>(g_PresetMan.GetEntityPreset(reader)); });
	MatchProperty("ShellVelocity", { reader >> m_ShellVel; });
	MatchProperty("FireSound", { reader >> m_FireSound; });
	MatchProperty("AILifeTime", { reader >> m_AILifeTime; });
	MatchProperty("AIFireVel", { reader >> m_AIFireVel; });
	MatchProperty("AIPenetration", { reader >> m_AIPenetration; });

	EndPropertyList;
}

int Round::Save(Writer& writer) const {
	Entity::Save(writer);

	writer.NewProperty("Particle");
	writer << m_Particle;
	writer.NewProperty("ParticleCount");
	writer << m_ParticleCount;
	writer.NewProperty("FireVelocity");
	writer << m_FireVel;
	writer.NewPropertyWithValue("InheritsFirerVelocity", m_InheritsFirerVelocity);
	writer.NewProperty("Separation");
	writer << m_Separation;
	writer.NewProperty("LifeVariation");
	writer << m_LifeVariation;
	writer.NewProperty("Shell");
	writer << m_Shell;
	writer.NewProperty("ShellVelocity");
	writer << m_ShellVel;
	writer.NewProperty("FireSound");
	writer << m_FireSound;
	writer.NewProperty("AILifeTime");
	writer << m_AILifeTime;
	writer.NewProperty("AIFireVel");
	writer << m_AIFireVel;
	writer.NewProperty("AIPenetration");
	writer << m_AIPenetration;

	return 0;
}

MovableObject* Round::PopNextParticle() {
	MovableObject* tempParticle = (m_ParticleCount > 0) ? dynamic_cast<MovableObject*>(m_Particle->Clone()) : 0;
	m_ParticleCount--;
	return tempParticle;
}
