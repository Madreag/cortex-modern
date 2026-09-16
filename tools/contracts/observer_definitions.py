# ContractAudit.h is generated from these definitions.
"""Field and visit definitions the observer generator emits into ContractAudit.h."""

EXTRA_INCLUDES_AFTER = {
    "SoundSet.h": ['"GUISound.h"'],
    "TerrainLayerSnapshot.h": [
        '"PathFinder.h"',
        '"UInputMan.h"',
        '"FrameMan.h"',
        '"PostProcessMan.h"',
        '"PrimitiveMan.h"',
    ],
}
EXTRA_STD_INCLUDES_AFTER = {
    "fstream": ["<functional>"],
}

OMIT_MEMBERS = {('RTE::GameActivity', 'm_RollbackDeliveries')}

EXTRA_FIELDS = {
    "ACraft": [
        'Field(path + ".ACraft.m_OffWireHatchTick", object.m_OffWireHatchTick);',
        'Field(path + ".ACraft.m_OffWireHatchOpen", object.m_OffWireHatchOpen);',
    ],
}

EXTRA_VISITS_AFTER = {}

POST_INVENTORY_VISITS = ''

VALUE_AND_FIELD = r'''
template <class T> std::string Value(const T& value, const std::string& path) {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<V, std::string> || std::is_same_v<V, std::string_view>) {
        std::ostringstream out; out << std::quoted(std::string(value)); return out.str();
    } else if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        std::ostringstream out;
        if constexpr (std::is_floating_point_v<V>) {
            out << std::hexfloat << value << " bits:" << std::hex;
            if constexpr (sizeof(V) == 4) out << std::bit_cast<uint32_t>(value);
            else if constexpr (sizeof(V) == 8) out << std::bit_cast<uint64_t>(value);
        }
        else if constexpr (std::is_enum_v<V>) out << static_cast<std::underlying_type_t<V>>(value);
        else out << +value;
        return out.str();
    } else if constexpr (std::is_pointer_v<V>) {
        if (!value) return "null";
        using P = std::remove_pointer_t<V>;
        if constexpr (requires { sizeof(P); } && !std::is_function_v<P>) {
            if constexpr (std::is_base_of_v<MovableObject, P>) return "uid:" + std::to_string(value->GetUniqueID());
            else if constexpr (std::is_base_of_v<Entity, P>) return EntityValue(value, path);
            else if constexpr (requires { Visit(*value, path); }) {
                auto [it, added] = visited.emplace(value, path);
                if (added) Visit(*value, path);
                return "ref:" + it->second;
            }
        }
        gaps.insert(path + " opaque pointer " + typeid(V).name());
        return "opaque:present";
    } else if constexpr (requires { value.get(); }) {
        return Value(value.get(), path);
    } else if constexpr (requires { value.first; value.second; }) {
        return "(" + Value(value.first, path + ".first") + "," + Value(value.second, path + ".second") + ")";
    } else if constexpr (requires { typename V::mapped_type; std::begin(value); std::end(value); }) {
        std::vector<std::pair<std::string, const typename V::value_type*>> entries;
        for (const auto& element: value) entries.emplace_back(Value(element.first, path + ".key"), &element);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string out = "[";
        for (const auto& [key, element]: entries) out += key + ":" + Value(element->second, path + "[key=" + key + "]") + ";";
        return out + "]";
    } else if constexpr (requires { std::begin(value); std::end(value); }) {
        std::vector<std::string> elements;
        size_t index = 0;
        for (const auto& element: value) elements.push_back(Value(element, path + "[" + std::to_string(index++) + "]"));
        if constexpr (requires { typename V::hasher; }) std::sort(elements.begin(), elements.end());
        std::string out = "[";
        for (const auto& element: elements) out += element + ";";
        return out + "]";
    } else if constexpr (requires { Visit(value, path); }) {
        Visit(value, path);
        return "fields";
    } else {
        gaps.insert(path + " opaque value " + typeid(V).name());
        return "opaque";
    }
}
template <class T> void Field(const std::string& path, const T& value) { values[path] = Value(value, path); }
std::string EntityValue(const Entity* entity, const std::string& path);
void VisitEntity(const Entity& entity, const std::string& path);
'''

TAIL = r'''
static void Write(const State& state, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& [key, value]: state) out << key << " = " << value << "\n";
}
static size_t Compare(const State& before, const State& after, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    size_t count = 0;
    auto a = before.begin(), b = after.begin();
    while (a != before.end() || b != after.end()) {
        const bool hasA = a != before.end() && (b == after.end() || a->first <= b->first);
        const bool hasB = b != after.end() && (a == before.end() || b->first <= a->first);
        if (hasA && hasB && a->second == b->second) { ++a; ++b; continue; }
        ++count;
        out << (hasA ? a->first : b->first) << "\n  before " << (hasA ? a->second : "<absent>")
            << "\n  after  " << (hasB ? b->second : "<absent>") << "\n";
        if (hasA) ++a;
        if (hasB) ++b;
    }
    return count;
}
void Visit(const GraphicalPrimitive& object, const std::string& path) {
    Field(path + ".type", object.GetPrimitiveType());
    Field(path + ".start", object.m_StartPos); Field(path + ".end", object.m_EndPos);
    Field(path + ".radius_squared", object.m_DrawRadiusSquared); Field(path + ".color", object.m_Color);
    Field(path + ".player", object.m_Player); Field(path + ".blend_mode", object.m_BlendMode);
    Field(path + ".blend_channels", object.m_ColorChannelBlendAmounts); Field(path + ".depth", object.m_Depth);
    Field(path + ".image_owners", object.m_ImageOwners); Field(path + ".vertex_owners", object.m_VertexOwners);
    if (auto value = dynamic_cast<const LinePrimitive*>(&object)) Field(path + ".thickness", value->m_Thickness);
    if (auto value = dynamic_cast<const ArcPrimitive*>(&object)) {
        Field(path + ".start_angle", value->m_StartAngle); Field(path + ".end_angle", value->m_EndAngle);
        Field(path + ".radius", value->m_Radius); Field(path + ".thickness", value->m_Thickness);
    }
    if (auto value = dynamic_cast<const SplinePrimitive*>(&object)) {
        Field(path + ".guide_a", value->m_GuidePointAPos); Field(path + ".guide_b", value->m_GuidePointBPos);
    }
    if (auto value = dynamic_cast<const RoundedBoxPrimitive*>(&object)) Field(path + ".corner_radius", value->m_CornerRadius);
    if (auto value = dynamic_cast<const RoundedBoxFillPrimitive*>(&object)) Field(path + ".corner_radius", value->m_CornerRadius);
    if (auto value = dynamic_cast<const CirclePrimitive*>(&object)) Field(path + ".radius", value->m_Radius);
    if (auto value = dynamic_cast<const CircleFillPrimitive*>(&object)) Field(path + ".radius", value->m_Radius);
    if (auto value = dynamic_cast<const EllipsePrimitive*>(&object)) {
        Field(path + ".horizontal_radius", value->m_HorizRadius); Field(path + ".vertical_radius", value->m_VertRadius);
    }
    if (auto value = dynamic_cast<const EllipseFillPrimitive*>(&object)) {
        Field(path + ".horizontal_radius", value->m_HorizRadius); Field(path + ".vertical_radius", value->m_VertRadius);
    }
    if (auto value = dynamic_cast<const TrianglePrimitive*>(&object)) {
        Field(path + ".point_a", value->m_PointAPos); Field(path + ".point_b", value->m_PointBPos); Field(path + ".point_c", value->m_PointCPos);
    }
    if (auto value = dynamic_cast<const TriangleFillPrimitive*>(&object)) {
        Field(path + ".point_a", value->m_PointAPos); Field(path + ".point_b", value->m_PointBPos); Field(path + ".point_c", value->m_PointCPos);
    }
    if (auto value = dynamic_cast<const PolygonPrimitive*>(&object)) Field(path + ".vertices", value->m_Vertices);
    if (auto value = dynamic_cast<const PolygonFillPrimitive*>(&object)) Field(path + ".vertices", value->m_Vertices);
    if (auto value = dynamic_cast<const TextPrimitive*>(&object)) {
        Field(path + ".text", value->m_Text); Field(path + ".small", value->m_IsSmall); Field(path + ".alignment", value->m_Alignment);
        Field(path + ".rotation", value->m_RotAngle); Field(path + ".bitmap", value->m_TextBitmap); Field(path + ".target_alignment", value->m_TargetPosAlignment);
    }
    if (auto value = dynamic_cast<const BitmapPrimitive*>(&object)) {
        Field(path + ".bitmap", value->m_Bitmap); Field(path + ".rotation", value->m_RotAngle); Field(path + ".scale", value->m_Scale);
        std::vector<std::string> cacheReferences;
        if (value->m_Bitmap) for (size_t slot = 0; slot < ContentFile::s_LoadedBitmaps.size(); ++slot) {
            for (const auto& [name, bitmap]: ContentFile::s_LoadedBitmaps[slot]) {
                if (bitmap == value->m_Bitmap) cacheReferences.push_back(std::to_string(slot) + ":" + name);
            }
        }
        std::sort(cacheReferences.begin(), cacheReferences.end());
        Field(path + ".bitmap_cache_references", cacheReferences);
        Field(path + ".flip_h", value->m_HFlipped); Field(path + ".flip_v", value->m_VFlipped);
        Field(path + ".sprite_owner", value->m_SpriteOwner); Field(path + ".sprite_frame", value->m_SpriteFrame); Field(path + ".icon_bitmap", value->m_IconBitmap);
    }
}
static State Observe(const std::string& gapPath) {
    ContractAudit audit;
    std::map<long int, MovableObject*> objects;
    { std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex); objects = g_MovableMan.m_KnownObjects; }
    size_t invalid = 0;
    for (const auto& [uid, object]: objects) {
        if (!object || uid <= 0 || object->GetUniqueID() != uid) {
            ++invalid;
            audit.Field("registry.invalid[" + std::to_string(uid) + "]", object ? object->GetUniqueID() : -1);
            continue;
        }
    }
    std::cout << "[contract-audit] registry=" << objects.size() << " invalid=" << invalid << std::endl;
    Write(audit.values, gapPath + ".registry.txt");
    for (const auto& [uid, object]: objects) {
        if (object && uid > 0 && object->GetUniqueID() == uid) audit.VisitEntity(*object, "mo[" + std::to_string(uid) + "]");
    }
    if (const Activity* activity = g_ActivityMan.GetActivity()) audit.VisitEntity(*activity, "activity");
    if (const Activity* activity = g_ActivityMan.GetCheckpointStartActivity()) audit.VisitEntity(*activity, "start_activity");
    if (const Scene* scene = g_SceneMan.GetScene()) audit.Visit(*scene, "scene");
    // RemoveOrphans clears its fixed-size scratch pixels before every search; observe the
    // manager-owned allocation separately so a successful scene transaction cannot lose it.
    BITMAP* const orphanSearch = g_SceneMan.m_pOrphanSearchBitmap;
    audit.Field("scene_manager.orphan_search.present", orphanSearch != nullptr);
    if (orphanSearch) {
        audit.Field("scene_manager.orphan_search.depth", bitmap_color_depth(orphanSearch));
        audit.Field("scene_manager.orphan_search.width", orphanSearch->w);
        audit.Field("scene_manager.orphan_search.height", orphanSearch->h);
    }
    audit.Visit(g_UInputMan, "input");
    audit.Visit(g_FrameMan, "frame");
    audit.Visit(g_PostProcessMan, "postprocess");
    size_t primitiveCount = 0;
    while (const auto* primitive = g_PrimitiveMan.GetCheckpointPrimitive(primitiveCount)) {
        audit.Field("primitives.queue[" + std::to_string(primitiveCount++) + "]", primitive);
    }
    audit.Field("primitives.count", primitiveCount);
    g_GUISound.VisitCheckpointSounds([&audit](size_t index, const SoundContainer& sound) {
        audit.Visit(sound, "gui_sounds[" + std::to_string(index) + "]");
    });
    audit.Field("rng.sim", g_SimRNG.SerializeCheckpoint());
    audit.Field("rng.render", g_RenderRNG.SerializeCheckpoint());
    audit.Field("clock.sim_count", g_TimerMan.GetSimUpdateCount());
    audit.Field("clock.sim_time", g_TimerMan.GetSimTimeTicks());
    audit.Field("clock.real_time", g_TimerMan.GetRealTickCount());
    audit.Field("clock.ticks_per_second", g_TimerMan.GetTicksPerSecond());
    audit.Field("clock.sim_accumulator", g_TimerMan.GetSimAccumulator());
    audit.Field("clock.dt", g_TimerMan.GetDeltaTimeSecs());
    audit.Field("allocator.uid", MovableObject::GetUniqueIDCounter());
    audit.Field("allocator.lua_cursor", g_LuaMan.GetScriptStateCursor());
    audit.Field("checkpoint.restoring", g_MovableMan.IsRestoringSnapshot());
    audit.Field("checkpoint.pending_graph", g_ActivityMan.HasFullScriptGraphToRestore());
    audit.Field("world.actors", g_MovableMan.m_Actors);
    audit.Field("world.items", g_MovableMan.m_Items);
    audit.Field("world.particles", g_MovableMan.m_Particles);
    audit.Field("world.added_actors", g_MovableMan.m_AddedActors);
    audit.Field("world.added_items", g_MovableMan.m_AddedItems);
    audit.Field("world.added_particles", g_MovableMan.m_AddedParticles);
    audit.Field("world.alarms", g_MovableMan.m_AlarmEvents);
    audit.Field("world.added_alarms", g_MovableMan.m_AddedAlarmEvents);
    audit.Field("world.rosters", g_MovableMan.m_ActorRoster);
    audit.Field("world.sort_rosters", g_MovableMan.m_SortTeamRoster);
    audit.Field("world.quarantine", g_MovableMan.m_LockstepJoinQuarantine);
    audit.Field("world.moid_index", g_MovableMan.m_MOIDIndex);
    audit.Field("world.update_number", g_MovableMan.m_SimUpdateFrameNumber);
    audit.Field("world.roster", g_MovableMan.DescribeTeamRosters());
    audit.Field("scene.global_acc", g_SceneMan.GetScene()->GetGlobalAcc());
    TerrainLayerSnapshot terrain;
    if (terrain.Capture()) {
        const auto hash = [](const std::vector<uint8_t>& bytes) {
            uint64_t result = 1469598103934665603ULL;
            for (uint8_t value: bytes) result = (result ^ value) * 1099511628211ULL;
            return result;
        };
        audit.Field("terrain.material", hash(terrain.mat));
        audit.Field("terrain.foreground", hash(terrain.fg));
        audit.Field("terrain.background", hash(terrain.bg));
    } else audit.Field("terrain.capture_failed", true);
    std::ofstream gapsOut(gapPath, std::ios::binary);
    for (const auto& gap: audit.gaps) gapsOut << gap << "\n";
    return std::move(audit.values);
}
// Read-only staged-candidate transaction observer. These values are compared within
// one process before/after a rejected replacement; native identities remain exact.
static State ObservePendingCheckpoint(const std::string& gapPath) {
    ContractAudit audit;
    const auto& pending = g_ActivityMan.m_PendingCheckpoint;
    audit.Field("pending.scene", pending.scene.get());
    audit.Field("pending.activity", pending.activity.get());
    audit.Field("pending.start_activity", pending.startActivity.get());
    audit.Field("pending.has_start_activity", pending.hasStartActivity);
    audit.Field("pending.restart_preset", pending.restartPreset);
    audit.Field("pending.restart_objects", pending.restartObjects);
    audit.Field("pending.restart_units", pending.restartUnits);
    audit.Field("pending.sim_count", pending.simUpdateCount);
    audit.Field("pending.sim_time", pending.simTimeTicks);
    audit.Field("pending.uid_counter", pending.uniqueIDCounter);
    audit.Field("pending.lua_cursor", pending.luaStateCursor);
    audit.Field("pending.join_quarantine", pending.joinQuarantine);
    audit.Field("pending.runtime_globals", pending.runtimeGlobals);
    audit.Field("pending.world_structure", pending.worldStructure);
    audit.Field("pending.scene_runtime", pending.sceneRuntime);
    audit.Field("pending.script_graphs", pending.scriptGraphs);
    audit.Field("pending.sound_registrations", pending.soundRegistrations);
    audit.Field("manager.restores_snapshot", g_ActivityMan.m_RestartRestoresSnapshot);
    audit.Field("manager.needs_restart", g_ActivityMan.m_ActivityNeedsRestart);
    audit.Field("manager.needs_resume", g_ActivityMan.m_ActivityNeedsResume);
    audit.Field("manager.start_resumed", g_ActivityMan.m_StartActivityResumed);
    const auto identity = [&](const std::string& path, const void* pointer) {
        audit.Field("identity." + path, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)));
    };
    identity("scene", pending.scene.get()); identity("activity", pending.activity.get());
    identity("start_activity", pending.startActivity.get()); identity("image_scope", pending.images.get());
    identity("live_activity", g_ActivityMan.m_Activity.get()); identity("configured_start", g_ActivityMan.m_StartActivity.get());
    std::set<const Entity*> walked;
    std::function<void(const Entity*, const std::string&)> native = [&](const Entity* entity, const std::string& path) {
        if (!entity) return;
        identity(path, entity);
        if (!walked.insert(entity).second) return;
        audit.VisitEntity(*entity, path);
        if (const auto* rotating = dynamic_cast<const MOSRotating*>(entity)) {
            size_t index = 0;
            for (const auto* child: rotating->GetAttachables()) native(child, path + ".attachable[" + std::to_string(index++) + "]");
            index = 0;
            for (const auto* child: rotating->GetWoundList()) native(child, path + ".wound[" + std::to_string(index++) + "]");
        }
        if (const auto* actor = dynamic_cast<const Actor*>(entity)) {
            size_t index = 0;
            for (const auto* child: *actor->GetInventory()) native(child, path + ".inventory[" + std::to_string(index++) + "]");
        }
        if (const auto* craft = dynamic_cast<const ACraft*>(entity)) {
            size_t index = 0;
            for (const auto* child: craft->GetCollectedInventory()) native(child, path + ".collected[" + std::to_string(index++) + "]");
        }
    };
    if (pending.scene) {
        native(pending.scene.get(), "candidate.scene");
        for (int set = 0; set < Scene::PLACEDSETSCOUNT; ++set) {
            size_t index = 0;
            for (const auto* entity: *pending.scene->GetPlacedObjects(set)) {
                native(entity, "candidate.placed[" + std::to_string(set) + "][" + std::to_string(index++) + "]");
            }
        }
        for (size_t index = 0; index < Players::MaxPlayerCount; ++index) {
            native(pending.scene->m_ResidentBrains[index], "candidate.brain[" + std::to_string(index) + "]");
        }
    }
    native(pending.activity.get(), "candidate.activity");
    native(pending.startActivity.get(), "candidate.start_activity");
    for (const auto& [owner, sounds]: pending.soundRegistrations) {
        size_t index = 0;
        for (const auto* sound: sounds) native(sound, "candidate.sound[" + std::to_string(owner) + "][" + std::to_string(index++) + "]");
    }
    const auto surface = [&](const std::string& path, const SDL_Surface* image) {
        identity(path, image);
        audit.Field(path + ".present", image != nullptr);
        if (!image) return;
        audit.Field(path + ".format", image->format); audit.Field(path + ".width", image->w);
        audit.Field(path + ".height", image->h); audit.Field(path + ".pitch", image->pitch);
        const auto* format = SDL_GetPixelFormatDetails(image->format);
        uint64_t hash = 1469598103934665603ULL;
        if (format && image->pixels) {
            const size_t rowBytes = static_cast<size_t>(image->w) * format->bytes_per_pixel;
            for (int y = 0; y < image->h; ++y) {
                const auto* row = static_cast<const uint8_t*>(image->pixels) + static_cast<size_t>(y) * image->pitch;
                for (size_t x = 0; x < rowBytes; ++x) hash = (hash ^ row[x]) * 1099511628211ULL;
            }
        } else audit.gaps.insert(path + " missing surface pixels/format");
        audit.Field(path + ".pixels", hash);
    };
    if (pending.images) {
        audit.Field("pending.images.active", pending.images->m_Active);
        audit.Field("pending.images.committed", pending.images->m_Committed);
        audit.Field("pending.images.count", pending.images->m_Entries.size());
        size_t index = 0;
        for (const auto& entry: pending.images->m_Entries) {
            const std::string path = "pending.images[" + std::to_string(index++) + "]";
            audit.Field(path + ".path", entry.path);
            audit.Field(path + ".previous_bitmap", entry.previousBitmap);
            identity(path + ".previous_bitmap", entry.previousBitmap);
            surface(path + ".previous_image", entry.previousImage);
        }
    }
    audit.Field("cache.bitmaps", ContentFile::s_LoadedBitmaps);
    for (size_t slot = 0; slot < ContentFile::s_LoadedBitmaps.size(); ++slot) {
        for (const auto& [path, bitmap]: ContentFile::s_LoadedBitmaps[slot]) identity("cache.bitmap[" + std::to_string(slot) + "][" + path + "]", bitmap);
    }
    for (const auto& [path, image]: ContentFile::s_MemoryPNGs) surface("cache.memory_png[" + path + "]", image);
    std::ofstream gapsOut(gapPath, std::ios::binary);
    for (const auto& gap: audit.gaps) gapsOut << gap << "\n";
    return std::move(audit.values);
}
// End staged-candidate transaction observer.

static State Identity() {
    State state;
    state["activity"] = std::to_string(reinterpret_cast<uintptr_t>(g_ActivityMan.GetActivity()));
    state["scene"] = std::to_string(reinterpret_cast<uintptr_t>(g_SceneMan.GetScene()));
    state["scene_manager.orphan_search"] = std::to_string(reinterpret_cast<uintptr_t>(g_SceneMan.m_pOrphanSearchBitmap));
    state["lua"] = g_MovableMan.DescribeLuaIdentity();
    std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex);
    std::map<const MovableObject*, long> addresses;
    for (const auto& [uid, object]: g_MovableMan.m_KnownObjects) {
        state["uid:" + std::to_string(uid)] = std::to_string(reinterpret_cast<uintptr_t>(object));
        if (object) addresses.emplace(object, uid);
    }
    const auto borrowed = [&](const std::string& name, const MovableObject* target, long capturedUID) {
        const auto found = addresses.find(target);
        state[name] = "address:" + std::to_string(reinterpret_cast<uintptr_t>(target)) +
            " registered_uid:" + std::to_string(found != addresses.end() ? found->second : 0) +
            " captured_uid:" + std::to_string(capturedUID);
    };
    for (const auto& [uid, object]: g_MovableMan.m_KnownObjects) {
        if (!object) continue;
        const std::string prefix = "borrowed[" + std::to_string(uid) + "]";
        borrowed(prefix + ".ignore", object->m_pMOToNotHit, object->m_MOToNotHitUID);
        if (const auto* actor = dynamic_cast<const Actor*>(object)) {
            borrowed(prefix + ".move_target", actor->m_pMOMoveTarget, actor->m_FaithfulMOMoveTargetUID);
            size_t index = 0;
            for (const auto& [position, target]: actor->m_Waypoints) {
                borrowed(prefix + ".waypoint[" + std::to_string(index) + "]", target,
                    index < actor->m_FaithfulWaypointUIDs.size() ? actor->m_FaithfulWaypointUIDs[index] : 0);
                ++index;
            }
        }
    }
    return state;
}
static void SetInputFixture(bool perturbed) {
    // Audit-only synthetic device state; this never sends input to SDL or the desktop.
    auto& input = g_UInputMan;
    UInputMan::Keyboard keyboard;
    keyboard.keyStates[SDL_SCANCODE_SPACE] = !perturbed;
    keyboard.changedKeyStates[SDL_SCANCODE_SPACE] = !perturbed;
    keyboard.pressedSinceSim[SDL_SCANCODE_SPACE] = !perturbed;
    keyboard.releasedSinceSim[SDL_SCANCODE_A] = !perturbed;
    input.m_KeyboardStates[0] = keyboard;
    UInputMan::Mouse mouse;
    mouse.position = perturbed ? Vector(-812.5F, 913.75F) : Vector(147.25F, 285.5F);
    mouse.relativeMotion = Vector(perturbed ? 31.25F : -13.5F, 7.25F);
    mouse.analogAim = Vector(perturbed ? -0.875F : 0.375F, -0.625F);
    mouse.wheelChange = perturbed ? -7.25F : 3.5F;
    mouse.state[0] = mouse.change[0] = mouse.pressedSinceSim[0] = !perturbed;
    mouse.releasedSinceSim[1] = !perturbed;
    input.m_MouseStates[0] = mouse;
    input.m_TextInput = perturbed ? "input fixture advanced" : "input fixture captured";
    input.m_MouseSensitivity = perturbed ? 0.25F : 1.375F;
    input.m_ControlScheme[0].SetKeyMapping(0, perturbed ? SDL_SCANCODE_W : SDL_SCANCODE_Q);
    input.m_ControlScheme[0].SetDigitalAimSpeed(perturbed ? 1.5F : 0.375F);
    Gamepad gamepad(0, 400000001U, 3, 4);
    gamepad.m_Axis[1] = perturbed ? -23456 : 12345;
    gamepad.m_DigitalAxis[1] = perturbed ? -1 : 1;
    gamepad.m_Buttons[2] = gamepad.m_ButtonsPressedSinceSim[2] = !perturbed;
    gamepad.m_ButtonsReleasedSinceSim[3] = !perturbed;
    input.s_PrevJoystickStates[0] = gamepad;
    input.s_ChangedJoystickStates[0] = gamepad;
}
};
'''

ENTITY_VALUE = r'''
inline std::string ContractAudit::EntityValue(const Entity* entity, const std::string& path) {
    auto [it, added] = visited.emplace(entity, path);
    if (added) VisitEntity(*entity, path);
    return "ref:" + it->second;
}
inline void ContractAudit::VisitEntity(const Entity& entity, const std::string& path) {
    values[path + ".class"] = entity.GetClassName();
'''
