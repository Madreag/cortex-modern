#pragma once

#include "CheckpointArchive.h"
#include "fmod/fmod.hpp"
#include "fmod/fmod_errors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace RTE::AudioCheckpoint {

inline void Require(FMOD_RESULT result, const char* operation = "restore audio control") {
	if (result != FMOD_OK) throw std::runtime_error(std::string("audio checkpoint: ") + operation + ": " + FMOD_ErrorString(result));
}

class MixerLock {
public:
	explicit MixerLock(FMOD::System* system) : m_System(system) { if (m_System) Require(m_System->lockDSP()); }
	~MixerLock() { if (m_System) m_System->unlockDSP(); }
	MixerLock(const MixerLock&) = delete;
	MixerLock& operator=(const MixerLock&) = delete;
private:
	FMOD::System* m_System;
};

using Position = std::array<float, 3>;
inline Position Pack(FMOD_VECTOR value) { return {value.x, value.y, value.z}; }
inline FMOD_VECTOR Unpack(const Position& value) { return {value[0], value[1], value[2]}; }
inline bool Finite(float value) { return std::isfinite(value); }
template <class T> bool AllFinite(const T& values) { for (float value: values) if (!Finite(value)) return false; return true; }

struct EffectParameter {
	int index = 0, type = 0, integer = 0;
	float number = 0;
	bool boolean = false;
	template <class Archive> void Fields(Archive& archive) { archive(index, type, integer, number, boolean); }
	std::string SaveCheckpoint() const { CheckpointWriter archive("AudioParameter1"); const_cast<EffectParameter*>(this)->Fields(archive); return archive.Text(); }
	bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
		try { EffectParameter value; CheckpointReader archive(text, "AudioParameter1"); value.Fields(archive); archive.Finish(); if (value.index < 0 || value.index > 1024 || value.type < FMOD_DSP_PARAMETER_TYPE_FLOAT || value.type > FMOD_DSP_PARAMETER_TYPE_BOOL || !Finite(value.number)) return false; if (!validateOnly) *this = value; return true; }
		catch (const std::exception&) { return false; }
	}
};

struct Effect {
	int index = 0, type = 0;
	bool active = false, bypass = false;
	std::array<float, 3> wetDry{};
	std::vector<EffectParameter> parameters;
	template <class Archive> void Fields(Archive& archive) { archive(index, type, active, bypass, wetDry, parameters); }
	std::string SaveCheckpoint() const { CheckpointWriter archive("AudioEffect1"); const_cast<Effect*>(this)->Fields(archive); return archive.Text(); }
	bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
		try { Effect value; CheckpointReader archive(text, "AudioEffect1"); value.Fields(archive); archive.Finish(); if (value.index < 0 || value.index > 64 || !Managed(static_cast<FMOD_DSP_TYPE>(value.type)) || !AllFinite(value.wetDry)) return false; std::set<int> seen; for (const auto& parameter: value.parameters) if (!seen.insert(parameter.index).second) return false; if (!validateOnly) *this = std::move(value); return true; }
		catch (const std::exception&) { return false; }
	}
	static bool Managed(FMOD_DSP_TYPE type) { return type == FMOD_DSP_TYPE_MULTIBAND_EQ || type == FMOD_DSP_TYPE_COMPRESSOR || type == FMOD_DSP_TYPE_LIMITER; }
	static std::vector<Effect> Capture(FMOD::ChannelControl* control) {
		int count; Require(control->getNumDSPs(&count));
		std::vector<Effect> effects;
		for (int index = 0; index < count; ++index) {
			FMOD::DSP* dsp; FMOD_DSP_TYPE type;
			Require(control->getDSP(index, &dsp)); Require(dsp->getType(&type));
			if (!Managed(type)) continue;
			Effect effect; effect.index = index; effect.type = type;
			Require(dsp->getActive(&effect.active)); Require(dsp->getBypass(&effect.bypass));
			Require(dsp->getWetDryMix(&effect.wetDry[0], &effect.wetDry[1], &effect.wetDry[2]));
			int parameters; Require(dsp->getNumParameters(&parameters));
			for (int parameter = 0; parameter < parameters; ++parameter) {
				FMOD_DSP_PARAMETER_DESC* description; Require(dsp->getParameterInfo(parameter, &description));
				EffectParameter value; value.index = parameter; value.type = description->type;
				if (description->type == FMOD_DSP_PARAMETER_TYPE_FLOAT) Require(dsp->getParameterFloat(parameter, &value.number, nullptr, 0));
				else if (description->type == FMOD_DSP_PARAMETER_TYPE_INT) Require(dsp->getParameterInt(parameter, &value.integer, nullptr, 0));
				else if (description->type == FMOD_DSP_PARAMETER_TYPE_BOOL) Require(dsp->getParameterBool(parameter, &value.boolean, nullptr, 0));
				else continue; // Side-chain signal buffers are connections, not mutable effect parameters.
				effect.parameters.push_back(value);
			}
			effects.push_back(std::move(effect));
		}
		return effects;
	}
	static void ApplyActivation(FMOD::ChannelControl* control, const std::vector<Effect>& effects) {
		for (const Effect& effect: effects) {
			FMOD::DSP* dsp; Require(control->getDSP(effect.index, &dsp));
			Require(dsp->setActive(effect.active)); Require(dsp->setBypass(effect.bypass));
		}
	}
	static void Apply(FMOD::System* system, FMOD::ChannelControl* control, const std::vector<Effect>& effects) {
		int count; Require(control->getNumDSPs(&count));
		for (int index = count - 1; index >= 0; --index) {
			FMOD::DSP* dsp; FMOD_DSP_TYPE type;
			Require(control->getDSP(index, &dsp)); Require(dsp->getType(&type));
			if (!Managed(type)) continue;
			if (std::none_of(effects.begin(), effects.end(), [index, type](const Effect& effect) { return effect.index == index && effect.type == type; })) {
				Require(control->removeDSP(dsp)); Require(dsp->release());
			}
		}
		for (const Effect& effect: effects) {
			FMOD::DSP* dsp = nullptr; FMOD_DSP_TYPE type = FMOD_DSP_TYPE_UNKNOWN;
			if (control->getDSP(effect.index, &dsp) != FMOD_OK || dsp->getType(&type) != FMOD_OK || type != effect.type) {
				Require(system->createDSPByType(static_cast<FMOD_DSP_TYPE>(effect.type), &dsp));
				const FMOD_RESULT attached = control->addDSP(effect.index, dsp);
				if (attached != FMOD_OK) { dsp->release(); Require(attached, "insert DSP at checkpoint index"); }
			}
			for (const EffectParameter& parameter: effect.parameters) {
				if (parameter.type == FMOD_DSP_PARAMETER_TYPE_FLOAT) Require(dsp->setParameterFloat(parameter.index, parameter.number));
				else if (parameter.type == FMOD_DSP_PARAMETER_TYPE_INT) Require(dsp->setParameterInt(parameter.index, parameter.integer));
				else Require(dsp->setParameterBool(parameter.index, parameter.boolean));
			}
			Require(dsp->setWetDryMix(effect.wetDry[0], effect.wetDry[1], effect.wetDry[2]));
			Require(dsp->setActive(effect.active)); Require(dsp->setBypass(effect.bypass));
		}
	}
};

struct Control {
	bool paused = false, muted = false, ramp = false;
	float volume = 1, pitch = 1, lowPassGain = 1;
	std::array<float, 4> reverb{};
	unsigned int mode = 0;
	int outputs = 0, inputs = 0;
	std::vector<float> mix;
	bool hasDelayStart = false, hasDelayEnd = false, delayStops = false;
	int64_t delayStart = 0, delayEnd = 0;
	std::vector<std::pair<int64_t, float>> fades;
	bool spatial = false;
	Position position{}, velocity{}, coneOrientation{};
	float minimumDistance = 0, maximumDistance = 0;
	std::array<float, 3> cone{};
	float directOcclusion = 0, reverbOcclusion = 0, spread = 0, level = 0, doppler = 0;
	bool customDistanceFilter = false;
	float customLevel = 0, centerFrequency = 0;
	std::vector<Effect> effects;
	template <class Archive> void Fields(Archive& archive) {
		archive(paused, muted, ramp, volume, pitch, lowPassGain, reverb, mode, outputs, inputs, mix);
		archive(hasDelayStart, hasDelayEnd, delayStops, delayStart, delayEnd, fades);
		archive(spatial, position, velocity, coneOrientation, minimumDistance, maximumDistance, cone, directOcclusion, reverbOcclusion, spread, level, doppler, customDistanceFilter, customLevel, centerFrequency, effects);
	}
	std::string SaveCheckpoint() const { CheckpointWriter archive("AudioControl1"); const_cast<Control*>(this)->Fields(archive); return archive.Text(); }
	bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
		try {
			Control value; CheckpointReader archive(text, "AudioControl1"); value.Fields(archive); archive.Finish();
			if (value.outputs < 0 || value.inputs < 0 || value.outputs > FMOD_MAX_CHANNEL_WIDTH || value.inputs > FMOD_MAX_CHANNEL_WIDTH || value.mix.size() != static_cast<size_t>(value.outputs * value.inputs)) return false;
			if (!AllFinite(value.mix) || !AllFinite(value.reverb) || !AllFinite(value.position) || !AllFinite(value.velocity) || !AllFinite(value.coneOrientation) || !AllFinite(value.cone)) return false;
			for (float number: {value.volume, value.pitch, value.lowPassGain, value.minimumDistance, value.maximumDistance, value.directOcclusion, value.reverbOcclusion, value.spread, value.level, value.doppler, value.customLevel, value.centerFrequency}) if (!Finite(number)) return false;
			int64_t previous = std::numeric_limits<int64_t>::min(); for (const auto& [clock, volume]: value.fades) { if (clock < previous || !Finite(volume)) return false; previous = clock; }
			if (!validateOnly) *this = std::move(value);
			return true;
		} catch (const std::exception&) { return false; }
	}
	static Control Capture(FMOD::ChannelControl* control, bool spatial) {
		Control state;
		Require(control->getPaused(&state.paused)); Require(control->getMute(&state.muted)); Require(control->getVolumeRamp(&state.ramp));
		Require(control->getVolume(&state.volume)); Require(control->getPitch(&state.pitch)); Require(control->getLowPassGain(&state.lowPassGain)); Require(control->getMode(&state.mode));
		for (int index = 0; index < 4; ++index) Require(control->getReverbProperties(index, &state.reverb[index]));
		Require(control->getMixMatrix(nullptr, &state.outputs, &state.inputs));
		if (state.outputs < 0 || state.inputs < 0 || state.outputs > FMOD_MAX_CHANNEL_WIDTH || state.inputs > FMOD_MAX_CHANNEL_WIDTH) throw std::runtime_error("invalid audio mix dimensions");
		state.mix.resize(state.outputs * state.inputs);
		if (!state.mix.empty()) Require(control->getMixMatrix(state.mix.data(), &state.outputs, &state.inputs, state.inputs));
		unsigned long long clock, start, end;
		Require(control->getDSPClock(nullptr, &clock)); Require(control->getDelay(&start, &end, &state.delayStops));
		state.hasDelayStart = start != 0; state.hasDelayEnd = end != 0;
		state.delayStart = state.hasDelayStart ? static_cast<int64_t>(start) - static_cast<int64_t>(clock) : 0;
		state.delayEnd = state.hasDelayEnd ? static_cast<int64_t>(end) - static_cast<int64_t>(clock) : 0;
		unsigned int count = 0; Require(control->getFadePoints(&count, nullptr, nullptr));
		std::vector<unsigned long long> clocks(count); std::vector<float> volumes(count);
		if (count) Require(control->getFadePoints(&count, clocks.data(), volumes.data()));
		for (unsigned int index = 0; index < count; ++index) state.fades.emplace_back(static_cast<int64_t>(clocks[index]) - static_cast<int64_t>(clock), volumes[index]);
		// DSP clocks are local to the backend. Retain the current point and future
		// envelope, so restarting on a younger clock preserves the audible fade.
		if (!state.fades.empty() && state.fades.front().first < 0) {
			auto next = std::upper_bound(state.fades.begin(), state.fades.end(), int64_t{0}, [](int64_t offset, const auto& point) { return offset < point.first; });
			const auto previous = next == state.fades.begin() ? next : std::prev(next);
			float volume = previous->second;
			if (next != state.fades.end() && next->first > previous->first) volume += (next->second - volume) * static_cast<float>(-previous->first) / static_cast<float>(next->first - previous->first);
			state.fades.erase(state.fades.begin(), next); state.fades.insert(state.fades.begin(), {0, volume});
		}
		state.spatial = spatial;
		if (spatial) {
			FMOD_VECTOR position, velocity, direction;
			Require(control->get3DAttributes(&position, &velocity)); Require(control->get3DConeOrientation(&direction));
			state.position = Pack(position); state.velocity = Pack(velocity); state.coneOrientation = Pack(direction);
			Require(control->get3DMinMaxDistance(&state.minimumDistance, &state.maximumDistance)); Require(control->get3DConeSettings(&state.cone[0], &state.cone[1], &state.cone[2]));
			Require(control->get3DOcclusion(&state.directOcclusion, &state.reverbOcclusion)); Require(control->get3DSpread(&state.spread)); Require(control->get3DLevel(&state.level)); Require(control->get3DDopplerLevel(&state.doppler));
			Require(control->get3DDistanceFilter(&state.customDistanceFilter, &state.customLevel, &state.centerFrequency));
		}
		state.effects = Effect::Capture(control);
		return state;
	}
	void Apply(FMOD::System* system, FMOD::ChannelControl* control, bool unpause, bool applyEffects = true) const {
		Require(control->setPaused(true)); Require(control->setMode(mode)); Require(control->setMute(muted)); Require(control->setVolumeRamp(ramp));
		Require(control->setVolume(volume)); Require(control->setPitch(pitch)); Require(control->setLowPassGain(lowPassGain));
		for (int index = 0; index < 4; ++index) {
			float current;
			Require(control->getReverbProperties(index, &current));
			// FMOD creates a send even for zero volume. The master bus cannot
			// send to its own reverb input, so preserve an unchanged send in place.
			if (current != reverb[index]) Require(control->setReverbProperties(index, reverb[index]));
		}
		Require(control->setMixMatrix(mix.empty() ? nullptr : const_cast<float*>(mix.data()), outputs, inputs, inputs));
		unsigned long long clock; Require(control->getDSPClock(nullptr, &clock));
		const auto absolute = [clock](int64_t offset) { return static_cast<unsigned long long>(std::max<int64_t>(0, static_cast<int64_t>(clock) + offset)); };
		Require(control->setDelay(hasDelayStart ? absolute(delayStart) : 0, hasDelayEnd ? absolute(delayEnd) : 0, delayStops));
		Require(control->removeFadePoints(0, std::numeric_limits<unsigned long long>::max()));
		for (const auto& [offset, volume]: fades) Require(control->addFadePoint(absolute(offset), volume));
		if (spatial) {
			const FMOD_VECTOR p = Unpack(position), v = Unpack(velocity);
			FMOD_VECTOR direction = Unpack(coneOrientation);
			Require(control->set3DAttributes(&p, &v)); Require(control->set3DConeOrientation(&direction));
			Require(control->set3DMinMaxDistance(minimumDistance, maximumDistance)); Require(control->set3DConeSettings(cone[0], cone[1], cone[2]));
			Require(control->set3DOcclusion(directOcclusion, reverbOcclusion)); Require(control->set3DSpread(spread)); Require(control->set3DLevel(level)); Require(control->set3DDopplerLevel(doppler));
			Require(control->set3DDistanceFilter(customDistanceFilter, customLevel, centerFrequency));
		}
		if (applyEffects) Effect::Apply(system, control, effects);
		if (unpause) {
			Require(control->setPaused(paused));
			// FMOD activates the DSP chain when a control is unpaused.
			// Restore explicit effect switches after that backend transition.
			if (applyEffects) Effect::ApplyActivation(control, effects);
		}
	}
};

struct Voice {
	int identity = 0;
	uint64_t owner = 0;
	std::string path;
	bool playing = false;
	int bus = 0, priority = 0, loops = 0;
	unsigned int position = 0, loopStart = 0, loopEnd = 0;
	float frequency = 0, minimumAudibleDistance = 0;
	Control control;
	template <class Archive> void Fields(Archive& archive) { archive(identity, owner, path, playing, bus, priority, loops, position, loopStart, loopEnd, frequency, minimumAudibleDistance, control); }
	std::string SaveCheckpoint() const { CheckpointWriter archive("AudioVoice1"); const_cast<Voice*>(this)->Fields(archive); return archive.Text(); }
	bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
		try { Voice value; CheckpointReader archive(text, "AudioVoice1"); value.Fields(archive); archive.Finish(); if (value.identity <= 0 || value.path.empty() || value.bus < 0 || value.bus > 2 || value.priority < 0 || value.priority > 256 || value.loops < -1 || value.loopStart > value.loopEnd || !Finite(value.frequency) || !Finite(value.minimumAudibleDistance)) return false; if (!validateOnly) *this = std::move(value); return true; }
		catch (const std::exception&) { return false; }
	}
	static Voice Capture(int identity, uint64_t owner, const std::string& path, float minimumAudibleDistance, FMOD::Channel* channel, int bus) {
		Voice voice; voice.identity = identity; voice.owner = owner; voice.path = path; voice.minimumAudibleDistance = minimumAudibleDistance; voice.bus = bus;
		if (!channel || channel->isPlaying(&voice.playing) != FMOD_OK || !voice.playing) { voice.playing = false; return voice; }
		Require(channel->getPosition(&voice.position, FMOD_TIMEUNIT_PCM)); Require(channel->getFrequency(&voice.frequency)); Require(channel->getPriority(&voice.priority));
		// A channel that has run out of samples still reports playing, but its length is one past
		// the last frame a restore can seek to.
		FMOD::Sound* current = nullptr; unsigned int length = 0;
		if (channel->getCurrentSound(&current) == FMOD_OK && current && current->getLength(&length, FMOD_TIMEUNIT_PCM) == FMOD_OK && length > 0 && voice.position >= length) voice.position = length - 1;
		Require(channel->getLoopCount(&voice.loops)); Require(channel->getLoopPoints(&voice.loopStart, FMOD_TIMEUNIT_PCM, &voice.loopEnd, FMOD_TIMEUNIT_PCM));
		FMOD_MODE mode; Require(channel->getMode(&mode)); voice.control = Control::Capture(channel, (mode & FMOD_3D) != 0);
		return voice;
	}
	void Apply(FMOD::System* system, FMOD::Channel* channel) const {
		Require(channel->setFrequency(frequency)); Require(channel->setPriority(priority)); Require(channel->setLoopCount(loops));
		Require(channel->setLoopPoints(loopStart, FMOD_TIMEUNIT_PCM, loopEnd, FMOD_TIMEUNIT_PCM)); Require(channel->setPosition(position, FMOD_TIMEUNIT_PCM));
		control.Apply(system, channel, false);
	}
};

struct Sample {
	std::string path;
	unsigned int mode = 0, loopStart = 0, loopEnd = 0;
	float frequency = 0, minimumDistance = 0, maximumDistance = 0;
	int priority = 0, loops = 0;
	std::array<float, 3> cone{};
	template <class Archive> void Fields(Archive& archive) { archive(path, mode, loopStart, loopEnd, frequency, minimumDistance, maximumDistance, priority, loops, cone); }
	std::string SaveCheckpoint() const { CheckpointWriter archive("AudioSample1"); const_cast<Sample*>(this)->Fields(archive); return archive.Text(); }
	bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
		try { Sample value; CheckpointReader archive(text, "AudioSample1"); value.Fields(archive); archive.Finish(); if (value.path.empty() || value.loopStart > value.loopEnd || value.priority < 0 || value.priority > 256 || value.loops < -1 || !Finite(value.frequency) || !Finite(value.minimumDistance) || !Finite(value.maximumDistance) || !AllFinite(value.cone)) return false; if (!validateOnly) *this = std::move(value); return true; }
		catch (const std::exception&) { return false; }
	}
	static Sample Capture(const std::string& path, FMOD::Sound* sound) {
		Sample sample; sample.path = path;
		Require(sound->getMode(&sample.mode)); Require(sound->getLoopPoints(&sample.loopStart, FMOD_TIMEUNIT_PCM, &sample.loopEnd, FMOD_TIMEUNIT_PCM)); Require(sound->getLoopCount(&sample.loops));
		Require(sound->getDefaults(&sample.frequency, &sample.priority)); Require(sound->get3DMinMaxDistance(&sample.minimumDistance, &sample.maximumDistance)); Require(sound->get3DConeSettings(&sample.cone[0], &sample.cone[1], &sample.cone[2]));
		return sample;
	}
	void Apply(FMOD::Sound* sound) const {
		Require(sound->setMode(mode)); Require(sound->setLoopPoints(loopStart, FMOD_TIMEUNIT_PCM, loopEnd, FMOD_TIMEUNIT_PCM)); Require(sound->setLoopCount(loops));
		Require(sound->setDefaults(frequency, priority)); Require(sound->set3DMinMaxDistance(minimumDistance, maximumDistance)); Require(sound->set3DConeSettings(cone[0], cone[1], cone[2]));
	}
};

} // namespace RTE::AudioCheckpoint
