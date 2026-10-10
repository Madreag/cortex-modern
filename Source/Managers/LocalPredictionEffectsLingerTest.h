
void MovableMan::CheckPreviewEffectsLingerForSelfTest() {
	static const char* output = std::getenv("CCCP_TEST_EFFECTS_LINGER");
	static bool finished = false;
	if (!output || finished || !g_SceneMan.GetScene() || !g_ActivityMan.GetActivity() ||
	    g_ActivityMan.GetActivity()->GetPresetName() != "Determinism EffectsLinger") {
		return;
	}
	finished = true;
	const long long savedTick = g_TimerMan.GetSimUpdateCount();
	const long long savedTime = g_TimerMan.GetSimTimeTicks();
	const int maxTicks = g_SettingsMan.GetLocalPredictionMaxTicks();
	nlohmann::json result = {{"schema", 1}, {"pass", true}, {"stall", "held simulation clock"}, {"max_prediction_ticks", maxTicks}, {"cases", nlohmann::json::array()}};
	BITMAP* frame = create_bitmap_ex(8, 128, 64);
	BITMAP* reference = create_bitmap_ex(8, 128, 64);
	for (int rate: {8, 60}) {
		for (int shift: {-1, 0, 1}) {
			PreviewEventLedger::Clear();
			const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
			const uint64_t emitter = 1;
			PreviewEventLedger::Arm(tick, UINT64_MAX, {emitter});
			std::array<MovableObject*, 2> spawns{};
			std::array<PreviewEventLedger::Key, 2> keys{};
			std::array<unsigned long, 2> lifetimes{};
			std::array<long, 2> identities{};
			const long long birthTime = g_TimerMan.GetSimTimeTicks();
			bool setup = true;
			for (size_t index = 0; index < spawns.size(); ++index) {
				const Entity* preset = index == 0 ?
					g_PresetMan.GetEntityPreset("Attachable", "Muzzle Flash Pistol", "Base.rte") :
					g_PresetMan.GetEntityPreset("MOPixel", "Explosion Flame Glow", "Base.rte");
				MovableObject* spawn = preset ? dynamic_cast<MovableObject*>(preset->Clone()) : nullptr;
				if (!spawn) {
					setup = false;
					continue;
				}
				spawn->SetPos(Vector(300 + static_cast<float>(index * 64), 100));
				spawn->SetVel(Vector());
				spawn->SetGlobalAccScalar(0);
				spawn->SetRestThreshold(-1);
				spawn->SetToHitMOs(false);
				spawn->SetToGetHitByMOs(false);
				if (index == 0) spawn->SetLifetime(100);
				lifetimes[index] = spawn->GetLifetime();
				spawns[index] = spawn;
				identities[index] = spawn->GetUniqueID();
				keys[index] = {PreviewEventLedger::Projectile, emitter, 0,
					Hash(spawn->GetPresetName() + "@" + std::to_string(spawn->GetModuleID())),
					static_cast<uint64_t>(static_cast<int64_t>(tick) + shift), 0};
				PreviewEventLedger::Insert(keys[index], {});
				MovableObject* ghost = dynamic_cast<MovableObject*>(spawn->Clone());
				setup &= InstallPreviewGhost(ghost, keys[index], tick + 4);
			}
			PreviewEventLedger::Disarm();
			{
				SoundSimulationScope scope(emitter, 0);
				for (MovableObject* spawn: spawns) {
					if (spawn) AddParticle(spawn);
				}
			}
			const bool consumed = PreviewEventLedger::GetLiveEntryCount() == 0;
			size_t adopted = 0;
			for (const PreviewGhostState& ghost: GetPreviewGhostStates()) adopted += ghost.adopted;
			Update();
			const bool initiallyAlive = setup && IsParticle(spawns[0]) && IsParticle(spawns[1]);
			bool clockHeld = true;
			bool worldLifetime = true;
			bool previewLifetime = true;
			bool previewBound = true;
			int firstPixels = 0;
			int latePixels = 0;
			int expiredGhosts = 0;
			int oldGhosts = 0;
			const int ticks = static_cast<int>(std::ceil(600.0 / g_TimerMan.GetDeltaTimeMS())) + maxTicks + 3;
			const Vector target(268, 68);
			for (int step = 0; step <= ticks; ++step) {
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(1000000 / rate);
				const long long heldTick = g_TimerMan.GetSimUpdateCount();
				const long long heldTime = g_TimerMan.GetSimTimeMS();
				for (int repaint = 0; repaint < 2; ++repaint) {
					clear_to_color(frame, 0);
					clear_to_color(reference, 0);
					auto ghosts = std::move(m_PreviewGhosts);
					m_PreviewGhosts.clear();
					Draw(reference, target);
					m_PreviewGhosts = std::move(ghosts);
					Draw(frame, target);
					int pixels = 0;
					for (int y = 0; y < frame->h; ++y) {
						for (int x = 0; x < frame->w; ++x) pixels += getpixel(frame, x, y) != getpixel(reference, x, y);
					}
					if (step == 0) firstPixels = std::max(firstPixels, pixels);
					if (step == ticks) latePixels = std::max(latePixels, pixels);
					for (const PreviewGhost& ghost: m_PreviewGhosts) {
						const bool expired = ghost.object && ghost.object->GetLifetime() &&
							static_cast<double>(g_TimerMan.GetSimTimeTicks() - ghost.object->GetAgeTimerStart()) * 1000.0 /
							g_TimerMan.GetTicksPerSecond() > ghost.object->GetLifetime();
						const bool old = static_cast<uint64_t>(heldTick) > ghost.key.tick &&
							static_cast<uint64_t>(heldTick) - ghost.key.tick > static_cast<uint64_t>(maxTicks);
						expiredGhosts += expired;
						oldGhosts += old;
						previewLifetime &= !expired;
						previewBound &= !old;
					}
					std::this_thread::sleep_until(repaint == 0 ?
						deadline - std::chrono::microseconds(500000 / rate) : deadline);
					clockHeld &= heldTick == g_TimerMan.GetSimUpdateCount() && heldTime == g_TimerMan.GetSimTimeMS();
				}
				for (size_t index = 0; index < spawns.size(); ++index) {
					const double age = static_cast<double>(g_TimerMan.GetSimTimeTicks() - birthTime) * 1000.0 / g_TimerMan.GetTicksPerSecond();
					if (age > lifetimes[index]) worldLifetime &= !IsParticle(FindObjectByUniqueID(identities[index]));
				}
				if (step == ticks) break;
				g_TimerMan.AdvanceSimTickForPreview();
				Update();
				PreviewEventLedger::ExpireForTick(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
			}
			const bool passed = setup && consumed && adopted == 2 && initiallyAlive && clockHeld && worldLifetime &&
				previewLifetime && previewBound && firstPixels > 0 && latePixels == 0 && GetPreviewGhostCount() == 0;
			result["cases"].push_back({{"tps", rate}, {"retime_ticks", shift}, {"setup", setup}, {"ledger_consumed", consumed},
				{"adopted", adopted}, {"canonical_initially_alive", initiallyAlive}, {"clock_held", clockHeld},
				{"world_lifetime", worldLifetime}, {"preview_lifetime", previewLifetime}, {"preview_bound", previewBound},
				{"initial_overlay_pixels", firstPixels}, {"late_overlay_pixels", latePixels}, {"expired_ghost_observations", expiredGhosts},
				{"old_ghost_observations", oldGhosts}, {"remaining_ghosts", GetPreviewGhostCount()}, {"sim_ticks", ticks}, {"pass", passed}});
			result["pass"] = result["pass"].get<bool>() && passed;
		}
	}
	PreviewEventLedger::Clear();
	g_TimerMan.RestoreSimTickAfterPreview(savedTick, savedTime);
	destroy_bitmap(frame);
	destroy_bitmap(reference);
	std::ofstream(output) << result.dump(2) << '\n';
	std::cout << "[effects-linger] " << (result["pass"].get<bool>() ? "PASS" : "FAIL") << " held-clock effects check\n";
}

#pragma once

static bool s_EffectsLingerLockstep = false;

void MovableMan::CheckEffectsLingerForSelfTest() {
	static const char* output = std::getenv("CCCP_TEST_EFFECTS_LINGER");
	static bool finished = false;
	if (!output || finished || !g_SceneMan.GetScene() || !g_ActivityMan.GetActivity() ||
	    g_ActivityMan.GetActivity()->GetPresetName() != "Determinism EffectsLinger") {
		return;
	}
	finished = true;
	const long long savedTick = g_TimerMan.GetSimUpdateCount();
	const long long savedTime = g_TimerMan.GetSimTimeTicks();
	const bool savedFreeRun = g_TimerMan.IsFreeRunSim();
	const int maxTicks = g_SettingsMan.GetLocalPredictionMaxTicks();
	nlohmann::json result = {{"schema", 1}, {"pass", true}, {"stall", "held input with ticks owed"},
		{"max_prediction_ticks", maxTicks}, {"cases", nlohmann::json::array()}};
	BITMAP* frame = create_bitmap_ex(8, 128, 64);
	s_EffectsLingerLockstep = true;
	g_TimerMan.SetFreeRunSim(false);
	for (int rate: {8, 60}) {
		PreviewEventLedger::Clear();
		g_PostProcessMan.ClearScenePostEffects();
		g_PostProcessMan.ClearScreenPostEffects();
		const auto* gunPreset = dynamic_cast<const HDFirearm*>(g_PresetMan.GetEntityPreset("HDFirearm", "Pistol", "Base.rte"));
		const auto* explosionPreset = dynamic_cast<const MovableObject*>(g_PresetMan.GetEntityPreset("MOPixel", "Explosion Flame Glow", "Base.rte"));
		std::unique_ptr<HDFirearm> gun(gunPreset ? dynamic_cast<HDFirearm*>(gunPreset->Clone()) : nullptr);
		MovableObject* explosion = explosionPreset ? dynamic_cast<MovableObject*>(explosionPreset->Clone()) : nullptr;
		const bool setup = gun && gun->GetFlash() && explosion;
		if (!setup) {
			result["cases"].push_back({{"tps", rate}, {"setup", false}, {"pass", false}});
			result["pass"] = false;
			delete explosion;
			continue;
		}
		gun->SetPos(Vector(300, 100));
		gun->SetTeam(0);
		for (int warm = 0; warm < 60; ++warm) g_TimerMan.AdvanceSimTickForPreview();
		explosion->SetPos(Vector(364, 100));
		explosion->SetVel(Vector());
		explosion->SetGlobalAccScalar(0);
		explosion->SetRestThreshold(-1);
		explosion->SetToHitMOs(false);
		explosion->SetToGetHitByMOs(false);
		AddParticle(explosion);
		g_TimerMan.GrantSimUpdates(2);
		g_TimerMan.BeginSimFrame(0);
		g_TimerMan.UpdateSim();
		Update();
		const int roundsBefore = gun->GetRoundInMagCount();
		gun->Activate();
		gun->Update();
		const bool oneShot = gun->FiredFrame() && roundsBefore - gun->GetRoundInMagCount() == 1;
		const size_t flashHash = gun->GetFlash()->GetScreenEffectHash();
		const size_t explosionHash = explosion->GetScreenEffectHash();
		struct Transient {
			long identity;
			unsigned long lifetime;
			long long born;
		};
		std::vector<Transient> transients;
		int ticks = maxTicks + 3;
		const auto collect = [&](const auto& particles) {
			for (const MovableObject* particle: particles) {
				if (particle->GetLifetime()) {
					transients.push_back({particle->GetUniqueID(), particle->GetLifetime(), particle->GetAgeTimerStart()});
					ticks = std::max(ticks, static_cast<int>(std::ceil(particle->GetLifetime() / g_TimerMan.GetDeltaTimeMS())) + maxTicks + 3);
				}
			}
		};
		collect(m_Particles);
		collect(m_AddedParticles);
		bool clockHeld = true;
		bool worldLifetime = true;
		bool flashLifetime = true;
		bool explosionLifetime = true;
		bool previewBound = true;
		bool timerStayedUndrawn = true;
		int initialFlash = 0;
		int initialExplosion = 0;
		int lateGlows = 0;
		int staleFlash = 0;
		int staleExplosion = 0;
		const long long shotTime = g_TimerMan.GetSimTimeTicks();
		const unsigned long explosionLifetimeMS = explosion->GetLifetime();
		const Vector target(268, 68);
		for (int step = 0; step <= ticks; ++step) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(1000000 / rate);
			const long long heldTick = g_TimerMan.GetSimUpdateCount();
			const long long heldTime = g_TimerMan.GetSimTimeTicks();
			const double age = static_cast<double>(heldTime - shotTime) * 1000.0 / g_TimerMan.GetTicksPerSecond();
			timerStayedUndrawn &= !g_TimerMan.DrawnSimUpdate() && g_TimerMan.SimUpdatesSinceDrawn() > 0;
			for (int repaint = 0; repaint < 2; ++repaint) {
				clear_to_color(frame, 0);
				Draw(frame, target);
				gun->Draw(frame, target);
				std::list<PostEffect> effects;
				g_PostProcessMan.GetPostScreenEffectsWrapped(target, frame->w, frame->h, effects);
				int flashes = 0;
				int explosions = 0;
				for (const PostEffect& effect: effects) {
					flashes += effect.m_BitmapHash == flashHash;
					explosions += effect.m_BitmapHash == explosionHash;
				}
				if (step == 0) {
					initialFlash = flashes;

				}
				if (age <= explosionLifetimeMS) initialExplosion = std::max(initialExplosion, explosions);
				if (step > 0) {
					staleFlash += flashes;
					flashLifetime &= !gun->FiredFrame() && flashes == 0;
				}
				if (age > explosionLifetimeMS) {
					staleExplosion += explosions;
					explosionLifetime &= explosions == 0;
				}
				if (step > maxTicks) previewBound &= flashes == 0 && explosions <= (age <= explosionLifetimeMS ? 1 : 0);
				if (step == ticks) lateGlows = flashes + explosions;
				std::this_thread::sleep_until(repaint == 0 ?
					deadline - std::chrono::microseconds(500000 / rate) : deadline);
				clockHeld &= heldTick == g_TimerMan.GetSimUpdateCount() && heldTime == g_TimerMan.GetSimTimeTicks();
			}
			for (const Transient& transient: transients) {
				const double ageMS = static_cast<double>(heldTime - transient.born) * 1000.0 / g_TimerMan.GetTicksPerSecond();
				if (ageMS > transient.lifetime) worldLifetime &= !IsParticle(FindObjectByUniqueID(transient.identity));
			}
			if (step == ticks) break;
			g_TimerMan.Update();
			g_TimerMan.GrantSimUpdates(2);
			g_TimerMan.BeginSimFrame(0);
			g_TimerMan.UpdateSim();
			Update();
			gun->Deactivate();
			gun->Update();
		}
		const bool passed = oneShot && initialFlash == 1 && initialExplosion >= 1 && clockHeld && timerStayedUndrawn &&
			worldLifetime && flashLifetime && explosionLifetime && previewBound && lateGlows == 0 && GetPreviewGhostCount() == 0;
		result["cases"].push_back({{"tps", rate}, {"setup", setup}, {"one_shot", oneShot}, {"transient_mos", transients.size()},
			{"initial_flash_glows", initialFlash}, {"initial_explosion_glows", initialExplosion}, {"clock_held", clockHeld},
			{"timer_stayed_undrawn", timerStayedUndrawn}, {"world_lifetime", worldLifetime}, {"flash_lifetime", flashLifetime},
			{"explosion_lifetime", explosionLifetime}, {"preview_bound", previewBound}, {"stale_flash_observations", staleFlash},
			{"stale_explosion_observations", staleExplosion}, {"late_glows", lateGlows}, {"sim_ticks", ticks},
			{"remaining_ghosts", GetPreviewGhostCount()}, {"pass", passed}});
		result["pass"] = result["pass"].get<bool>() && passed;
	}
	s_EffectsLingerLockstep = false;
	PreviewEventLedger::Clear();
	g_PostProcessMan.ClearScenePostEffects();
	g_PostProcessMan.ClearScreenPostEffects();
	g_TimerMan.RestoreSimTickAfterPreview(savedTick, savedTime);
	g_TimerMan.SetFreeRunSim(savedFreeRun);
	destroy_bitmap(frame);
	std::ofstream(output) << result.dump(2) << '\n';
	std::cout << "[effects-linger] " << (result["pass"].get<bool>() ? "PASS" : "FAIL") << " held-frame glows check\n";
	CheckPreviewEffectsLingerForSelfTest();
	nlohmann::json previews;
	std::ifstream(output) >> previews;
	result["preview_cases"] = previews["cases"];
	result["pass"] = result["pass"].get<bool>() && previews["pass"].get<bool>();
	std::ofstream(output) << result.dump(2) << '\n';
}
