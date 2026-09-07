"""Read versioned CheckpointArchive payloads without constructing native objects.

Schemas name the serialized fields in their C++ order. Unclassified nested records and
tails stay byte-exact. Shared projection is explicit; full comparisons project nothing.
"""

import re


def fields(names, kind="n"):
    return [(name, kind) for name in names.split()]


def array(size, kind="n"):
    return ("array", size, kind)


def sequence(kind):
    return ("sequence", kind)


def structure(*items):
    return ("structure", items)


def optional(kind):
    return ("optional", kind)


VECTOR = array(2)
TIMER = structure(*fields("sim_start sim_limit real_start real_limit"))
STRINGS = sequence("s")
STRING_MAP = sequence(structure(("key", "s"), ("value", "s")))
NUMBER_MAP = sequence(structure(("key", "s"), ("value", "n")))
SCHEMAS = {}
SCHEMAS["Controller1"] = [
    ("states", array(57)), *fields("analog_move analog_aim analog_cursor", VECTOR),
    *fields("disabled wire_tick wire_scheme_valid wire_device_class wire_digital_aim_speed input_mode seat_mode player seat_player team "
            "next_ignore prev_ignore weapon_next_ignore weapon_prev_ignore pickup_ignore drop_ignore reload_ignore primary_hotkey_ignore"),
    *fields("release_timer joy_accel_timer key_accel_timer", TIMER), ("mouse_movement", VECTOR), ("cursor_angle_limits", array(3))]
SCHEMAS["TimerMan1"] = fields("ticks_per_second real_time sim_time sim_update_count sim_accumulator delta_time delta_time_seconds delta_buffer "
    "sim_updates_since_drawn drawn_sim_update sim_speed time_scale sim_paused sim_time_frozen free_run_sim "
    "pace_accrued pace_trimmed pace_wall_seen pace_cap_lost pace_paused_lost pace_update_calls pace_reset_calls")
SCHEMAS["Screen1"] = [("team", "n"), *fields("offset prev_offset delta_offset scroll_target", VECTOR),
    ("scroll_timer", TIMER), *fields("scroll_speed target_x_wrapped target_y_wrapped"), ("seam_cross_count", array(2)),
    ("occlusion", VECTOR), ("shake_magnitude", "n")]
SCHEMAS["CameraMan1"] = [*fields("shake_strength shake_decay max_shake_time default_gib_shake default_recoil_shake max_recoil_shake"),
    ("screens", array(4, "o"))]
SCHEMAS["FrameMan1"] = [*fields("horizontal_split vertical_split two_player_vertical_split"), ("screen_text", array(4, "s")),
    *fields("text_centered text_duration", array(4)), ("text_duration_timer", array(4, TIMER)), ("text_blinking", array(4)),
    ("text_blink_timer", TIMER), *fields("hud_disabled flash_color flashed_last_frame", array(4)), ("flash_timer", array(4, TIMER))]
SCHEMAS["MovableMan1"] = fields("splash_ratio max_dropped_items settling_enabled subtraction_enabled sim_update_frame")
SCHEMAS["SceneMan1"] = [*fields("layer_draw_mode raycast_visualizations pixel_check_visualizations last_updated_screen second_structure_pass"),
    *fields("calc_timer clean_timer", TIMER), ("scrap_compacting_height", "n")]
SCHEMAS["RuntimeGlobals1"] = [*fields("sim_rng render_rng", "s"), *fields("timer movable scene camera", "o")]
SCHEMAS["RuntimeGlobals2"] = [*SCHEMAS["RuntimeGlobals1"], ("frame", "o")]
SCHEMAS["RuntimeGlobals3"] = [*SCHEMAS["RuntimeGlobals2"], *fields("default_activity_type default_activity_name", "s"),
    *fields("in_activity needs_restart needs_resume resuming_from_pause skip_pause_menu start_activity_resumed")]
SCHEMAS["RuntimeGlobals4"] = [*SCHEMAS["RuntimeGlobals3"], ("gui_input", "o")]
SCHEMAS["GUISharedInput1"] = [("override_input", "n"), *fields("events states previous_states", array(4, array(3))), *fields("x y", array(4))]
SCHEMAS["Activity1"] = [
    *fields("state paused allows_saving test_activity"), *fields("description scene_name", "s"),
    *fields("max_players min_teams difficulty craft_orbit_at_edge campaign_stage player_count"),
    *fields("active human player_screen view_state", array(4)), ("death_timer", array(4, TIMER)), ("team_names", array(4, "s")),
    ("team_count", "n"), *fields("team_active player_team team_deaths team_ai_skill team_funds team_funds_share funds_changed funds_contribution had_brain brain_evacuated", array(4)),
    ("player_controller", array(4, "o")), ("message_timer", array(4, TIMER)), *fields("saved_encoded saved_strings", STRING_MAP),
    ("saved_numbers", NUMBER_MAP), ("actor_links", array(4, array(3)))]
SCHEMAS["GameActivity1"] = [
    ("base", "o"), ("cpu_team", "n"), ("team_is_cpu", array(4)), *fields("observation_target death_view_target", array(4, VECTOR)),
    ("actor_select_timer", array(4, TIMER)), *fields("actor_cursor landing_zone", array(4, VECTOR)),
    *fields("ai_return_craft next_order_y_offset lua_lock_actor lua_lock_actor_mode banner_repeats ready_to_start", array(4)),
    ("landing_zone_area", array(4, "o")), ("brain_lz_width", array(4)), ("objectives", sequence("o")), ("team_tech", array(4, "s")),
    ("team_tech_switch_enabled", array(4)), *fields("starting_gold fog_enabled require_clear_orbit default_fog default_clear_orbit default_deploy "
    "default_gold_cake default_gold_easy default_gold_medium default_gold_hard default_gold_nuts default_gold_max "
    "fog_switch_enabled deploy_switch_enabled gold_switch_enabled clear_orbit_switch_enabled buy_enabled"),
    ("lz_cursor_width", array(4)), ("delivery_delay", "n"), *fields("cursor_timer game_timer game_over_timer", TIMER),
    *fields("game_over_period winner_team"), ("network_names", array(4, "s")),
    ("player_ui", array(4, structure(*fields("buy editor inventory banner_red banner_yellow", "o"))))]
SCHEMAS["GameActivity2"] = [("values", "o"), ("players", array(4, structure(("marked_actor", "n"),
    ("purchases", sequence(array(3, "s"))), ("strategic_menu", "s")))),
    ("deliveries", array(4, sequence(structure(*fields("ordered_by_player"), ("landing_zone", VECTOR),
    *fields("order_y_offset delay"), ("timer", TIMER), ("craft", "s")))))]
SCHEMAS["InventoryMenuGUI1"] = [("mode", "n"), ("center", VECTOR), ("enabled_state", "n"), ("enable_timer", TIMER),
    *fields("actor_is_human carousel_empty_boxes carousel_transparent carousel_color"), ("carousel_border_size", VECTOR), *fields("carousel_border_color carousel_direction"),
    ("carousel_timer", TIMER), *fields("display_only show_empty_rows"), *fields("cursor previous_cursor", VECTOR),
    ("equipment_set", "n"), *fields("repeat_start_timer repeat_timer", TIMER), ("box_size", VECTOR), ("show_information", "n")]
SCHEMAS["InventoryMenuGUI2"] = [("initialized", "n"), *SCHEMAS["InventoryMenuGUI1"], ("actor", "o"),
    ("equipment", sequence(array(2, "o"))), ("carousel_boxes", array(5, "o")), ("exiting_box", "o"),
    *fields("carousel_bitmap carousel_background", "o"), ("button_names", array(4, "s")), ("icons", array(3, "o")),
    ("selected", optional(structure(("button", "s"), ("object", "o"), *fields("inventory_index equipped_index dragged drag_hold")))),
    ("inventory_buttons", sequence(structure(("object", "o"), ("button", "s")))), ("controls", optional("o"))]
SCHEMAS["CarouselItemBox1"] = [("value", optional(structure(("item", "o"), ("equipped", "n"),
    *fields("full_size current_size position icon_center", VECTOR), ("sides", array(2)))))]
SCHEMAS["GUIEntity1"] = [("value", optional(structure(("uid", "n"), *fields("class preset module", "s"))))]
SCHEMAS["GUIBitmap1"] = [("value", optional(structure(*fields("depth width height clip left right top bottom"), ("pixels", "s"))))]
SCHEMAS["FontChar1"] = fields("width height offset")
SCHEMAS["FlyingChar1"] = fields("character move_state position start_position show_position hide_position speed")
SCHEMAS["GUIBanner1"] = [("font_characters", array(2, array(256, "o"))), *fields("character_cap font_height kerning"),
    ("text", "s"), ("characters", sequence("o")), ("target_size", VECTOR), *fields("position_y fly_speed fly_spacing animation_mode animation_state"),
    *fields("total_timer display_timer spacing_timer frame_timer", TIMER)]
SCHEMAS["GUIBanner2"] = [*SCHEMAS["GUIBanner1"], ("fonts", array(2, "o"))]
SCHEMAS["BuyMenuGUI1"] = [*fields("enabled focus focus_change category speed list_item dragged_item dragging last_hovered"),
    ("category_item", array(8)), *fields("meta_player native_tech foreign_cost"), ("blink_timer", TIMER), ("blink_mode", "n"),
    *fields("menu_timer repeat_start_timer repeat_timer", TIMER), *fields("selecting_equipment last_equipment_tab last_main_tab "
    "last_equipment_scroll last_main_scroll first_main_tab last_main_tab_bound first_equipment_tab last_equipment_tab_bound "
    "selected_loadout purchase_made delivery_width enforce_max_passengers enforce_max_mass only_owned"),
    *fields("allowed always_allowed prohibited owned", NUMBER_MAP)]
SCHEMAS["BuyMenuGUI2"] = [("initialized", "n"), *SCHEMAS["BuyMenuGUI1"], ("tail", "tail")]
SCHEMAS["Entity1"] = [*fields("preset_name copied_from description reader_position", "s"), *fields("original module random_weight"), ("groups", STRINGS)]
SCHEMAS["Matrix1"] = [("angle", "n"), ("flipped", array(2)), ("elements", array(2, array(2))), ("elements_current", "n")]
SCHEMAS["ContentFile1"] = [*fields("path extension stem", "s"), ("is_image", "n"), ("image_info", array(3)),
    *fields("reader_position path_and_reader_position", "s"), *fields("module memory_png")]
SCHEMAS["LimbPath1"] = [("entity", "o"), ("start", VECTOR), *fields("start_segment_count disabled_collision_segment segment_progress "
    "travel_speed segment_threshold base_speed_multiplier current_speed_multiplier"), *fields("base_scale current_scale", VECTOR), ("push_force", "n"),
    *fields("joint_position joint_velocity", VECTOR), ("rotation", "o"), *fields("rotation_offset position_offset", VECTOR), ("time_left", "n"),
    *fields("path_timer segment_timer", TIMER), *fields("total_length regular_length segment_done ended flipped"), ("segments", sequence(VECTOR)), ("cursor", "n")]
SCHEMAS["ActorRuntime1"] = [*fields("player_controllable status health max_health previous_health"), ("last_second_timer", TIMER),
    *fields("last_second_position recent_movement", VECTOR), ("travel_impulse_damage", "n"), ("stable_recover_timer", TIMER),
    ("stable_velocity", VECTOR), ("stable_recover_delay", "n"), *fields("heartbeat new_control_timer death_timer", TIMER),
    *fields("gold_carried gold_picked can_run crouch_walk_multiplier aim_state aim_range aim_angle aim_distance"),
    *fields("aim_timer sharp_aim_timer", TIMER), *fields("sharp_aim_delay sharp_aim_progress sharp_aim_maxed"),
    *fields("pointing_target seen_target_position", VECTOR), ("alarm_timer", TIMER), ("last_alarm_position", VECTOR),
    *fields("sight_distance perceptiveness pain_threshold reveal_unseen character_height"), *fields("holster_offset reload_offset view_point", VECTOR),
    *fields("max_inventory_mass off_wire_aim_tick off_wire_aim off_wire_flip_tick off_wire_flip"), ("hotkey_activated", array(2)),
    *fields("hud_stack deployment_id passenger_slots ai_dig_strength base_mass ai_mode waypoint_cursor draw_waypoints"),
    *fields("move_target previous_path_target move_vector", VECTOR), *fields("update_path move_proximity movement_state organic mechanical limb_forces_disabled"),
    *fields("team_icon controller_icon", "s")]
SCHEMAS["AHumanRuntime1"] = [*fields("look_aim_ratio activate_background trigger_pulled reload_offhand"), ("icon_blink_timer", TIMER),
    *fields("arms_state prone_state"), ("prone_timer", TIMER), *fields("max_crouch_shift crouch_amount crouch_override"),
    ("paths", array(2, array(11, "o"))), ("rotation_targets", array(11)), ("aiming", "n"), ("arm_climbing", array(2)),
    *fields("stride_frame stride_start"), *fields("stride_timer throw_timer", TIMER), ("throw_prep_time", "n"), ("sharp_aim_revert_timer", TIMER),
    *fields("front_arm_flail back_arm_flail"), ("equip_hud_timer", TIMER), ("walk_angle", array(2, "o")), ("walk_offset", VECTOR),
    *fields("arm_swing_rate device_arm_sway"), ("owned_groups", array(6, "s")), ("deferred_equips", STRINGS)]
SCHEMAS["MOSpriteRuntime1"] = [*fields("rotation previous_rotation", "o"), *fields("angular_velocity previous_angular_velocity frame_count"),
    ("sprite_offset", VECTOR), *fields("frame animation_mode animation_duration"), ("animation_timer", TIMER),
    *fields("animation_reversing flipped forced_flip sprite_radius sprite_diameter angular_oscillations settling_material_disabled sprite_modified")]
SCHEMAS["MOSRotatingRuntime1"] = [*fields("deep_check force_deep_check deep_hardness"), *fields("travel_impulse sprite_center", VECTOR),
    *fields("orient_to_velocity recoiled"), *fields("recoil_force recoil_offset", VECTOR),
    *fields("entry_wound_sound_played exit_wound_sound_played farthest_attachment attachment_mass gib_impulse gib_wound_limit gib_blast "
    "gib_screen_shake wound_impulse_ratio detach_before_gib gib_at_lifetime gib_effect gib_loudness damage_multiplier no_damage_multiplier"), ("flash_timer", TIMER)]
SCHEMAS["HeldDeviceRuntime1"] = [*fields("type activated"), ("hotkey_activated", array(2)), ("activation_timer", TIMER),
    ("hotkey_timer", array(2, TIMER)), *fields("one_handed dual_wieldable"), *fields("stance sharp_stance support_offset", VECTOR),
    *fields("support_while_reload sharp_aim max_sharp_length supportable supported support_available unpickable"), ("seen_by_player", array(4)),
    ("grip_multiplier", "n"), ("blink_timer", TIMER), *fields("loudness explosive held_collisions visual_recoil")]
SCHEMAS["HDFirearmRuntime1"] = [*fields("reload_end_offset reload_sound_played rate_of_fire activation_delay deactivation_delay reloading done_reloading "
    "base_reload_time full_auto ignore_self reloadable one_handed_reload_time dual_reloadable reload_angle one_handed_reload_angle"),
    *fields("last_fire_timer reload_timer", TIMER), *fields("muzzle_offset eject_offset magazine_offset", VECTOR),
    *fields("shake sharp_shake no_support spread shell_angle shell_spread shell_angular_velocity shell_velocity recoil_shake ai_fire_velocity "
    "ai_bullet_lifetime ai_bullet_acceleration fired_once fire_frame fired_last_frame already_clicked rounds_fired manual_animation legacy_unflipped")]
SCHEMAS["AEmitterRuntime1"] = [*fields("enabled was_emitting emit_count count_limit negative_throttle positive_throttle throttle ignore_self burst_scale "
    "burst_damage damage_multiplier burst_triggered burst_spacing"), ("burst_timer", TIMER), ("play_burst_sound", "n"), ("emit_angle", "o"),
    ("emission_offset", VECTOR), ("emit_damage", "n"), ("last_emit_timer", TIMER),
    *fields("flash_scale average_burst_impulse average_impulse loudness flash_burst_only sustain_sound sound_follows_emitter")]
SCHEMAS["Emission1"] = [("entity", "o"), *fields("ppm burst_size accumulator spread min_velocity max_velocity life_variation pushes_emitter inherits_velocity inherits_angular_velocity"),
    *fields("start_timer stop_timer", TIMER), ("offset", VECTOR), ("particle_count", "n")]
SCHEMAS["SLBackground1"] = [("bitmap_file", "s"), *fields("frame_count frame animation_mode animation_duration animation_reversing"),
    ("animation_timer", TIMER), *fields("manual_animation scroll_x scroll_y"), ("scroll_step", VECTOR), ("scroll_interval", "n"),
    ("scroll_timer", TIMER), ("auto_offset", VECTOR), *fields("fill_left fill_right fill_up fill_down ignore_autoscale clear_color bitmap_updated masked wrap_x wrap_y"),
    *fields("origin offset", VECTOR), ("z_order", "n"), *fields("scroll_info scroll_ratio scale scaled_dimensions", VECTOR),
    ("drawings", sequence(array(4))), ("frames", sequence("o")), ("main_index", "n"), *fields("main_bitmap back_bitmap", "o")]
SCHEMAS["SLBackground2"] = [("bitmap_file", "o"), *SCHEMAS["SLBackground1"][1:]]
SCHEMAS["TerrainLayer1"] = [*fields("width height masked wrap_x wrap_y"), *fields("origin offset scroll_info scroll_ratio scale scaled_dimensions", VECTOR),
    ("z_order", "n"), *fields("entity content", "o")]
SCHEMAS["TerrainMetadata1"] = [*fields("width height"), ("terrain_layers", array(3, "o")), ("unseen", array(4, "o")),
    ("updated_material_areas", sequence(array(4))), *fields("orbit_direction layer_to_draw"),
    *fields("seen_pixels cleaned_pixels", array(4, sequence(VECTOR))), ("unseen_pixel_size", array(4, VECTOR)),
    ("scan_scheduled", array(4)), ("material_copy", "s")]
SCHEMAS["SceneRuntime1"] = [("entity", "o"), *fields("location location_offset", VECTOR),
    *fields("metagame_playable revealed owned_by_team round_income"), *fields("build_budget build_budget_ratio", array(4)),
    *fields("auto_designed total_investment pathfinding_updated"), ("partial_path_timer", TIMER), ("navigable_areas", STRINGS),
    ("navigable_areas_current", "n"), ("gravity", VECTOR), ("assembly_counts", NUMBER_MAP), ("preview_file", "o"),
    ("metascene_parent", "s"), *fields("metagame_internal saved_game_internal"), ("assemblies", STRING_MAP),
    ("pathfinders", sequence("o")), ("backgrounds", sequence(array(2, "o"))), ("brains", array(4, "s")),
    ("placed", array(3, STRINGS)), ("deployments", STRINGS), ("preview", "o"), ("terrain_metadata", "o")]


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def number(self):
        end = self.data.find(b" ", self.pos)
        if end < 0 or not re.fullmatch(rb"-?\d+", self.data[self.pos:end]):
            raise ValueError(f"invalid runtime checkpoint number at byte {self.pos}")
        value = int(self.data[self.pos:end])
        self.pos = end + 1
        return value

    def string(self):
        size = self.number()
        end = self.pos + size
        if size < 0 or end >= len(self.data) or self.data[end:end + 1] != b" ":
            raise ValueError("invalid runtime checkpoint string length")
        value, self.pos = self.data[self.pos:end], end + 1
        return value

    def value(self, spec):
        if spec == "n":
            return self.number()
        if spec in ("s", "o"):
            value = self.string()
            return decode(value) if spec == "o" and value else value
        if spec == "tail":
            value, self.pos = self.data[self.pos:], len(self.data)
            return value
        if spec[0] == "optional":
            present = self.number()
            if present not in (0, 1):
                raise ValueError("invalid runtime checkpoint presence flag")
            return self.value(spec[1]) if present else None
        if spec[0] == "structure":
            return {name: self.value(kind) for name, kind in spec[1]}
        if spec[0] == "array":
            count, kind = spec[1:]
        else:
            count, kind = self.number(), spec[1]
        if not 0 <= count <= len(self.data) - self.pos:
            raise ValueError("invalid runtime checkpoint count")
        return [self.value(kind) for _ in range(count)]


def decode(data):
    reader = Reader(data)
    version = reader.string().decode("ascii")
    if version not in SCHEMAS:
        return data
    result = {"version": version, **{name: reader.value(kind) for name, kind in SCHEMAS[version]}}
    if reader.pos != len(data):
        raise ValueError(f"trailing {version} runtime checkpoint data at byte {reader.pos}")
    if version == "GUIBitmap1" and result["value"] is not None:
        bitmap = result["value"]
        if bitmap["depth"] not in (8, 15, 16, 24, 32) or not 0 < bitmap["width"] <= 32768 or not 0 < bitmap["height"] <= 32768:
            raise ValueError("invalid GUI bitmap dimensions")
        if len(bitmap["pixels"]) != bitmap["width"] * ((bitmap["depth"] + 7) // 8) * bitmap["height"]:
            raise ValueError("invalid GUI bitmap pixel count")
        if not 0 <= bitmap["left"] <= bitmap["right"] <= bitmap["width"] or not 0 <= bitmap["top"] <= bitmap["bottom"] <= bitmap["height"]:
            raise ValueError("invalid GUI bitmap clipping")
    return result


_LOCAL_FIELDS = {
    "TimerMan1": set("real_time sim_accumulator sim_updates_since_drawn drawn_sim_update sim_speed pace_accrued pace_trimmed pace_wall_seen pace_cap_lost pace_paused_lost pace_update_calls pace_reset_calls".split()),
    "Controller1": set("input_mode seat_mode player seat_player team next_ignore prev_ignore weapon_next_ignore weapon_prev_ignore pickup_ignore drop_ignore reload_ignore primary_hotkey_ignore".split()),
    "Screen1": {name for name, _ in SCHEMAS["Screen1"]},
    "FrameMan1": {"flashed_last_frame", "flash_timer"},
    "ActorRuntime1": {"hud_stack"},
    "AEmitterRuntime1": {"average_burst_impulse", "average_impulse"},
    "HDFirearmRuntime1": {"ai_fire_velocity", "ai_bullet_lifetime", "ai_bullet_acceleration"},
}


def project(value, shared=False, snapshot_name=None, path=(), masked=None, local_roles=None):
    """Return a structural value, projecting only the named local fields and timer anchors."""
    if masked is None:
        masked = []
    if isinstance(value, list):
        return [project(item, shared, snapshot_name, (*path, index), masked, local_roles) for index, item in enumerate(value)]
    if not isinstance(value, dict):
        return value
    result = {key: project(item, shared, snapshot_name, (*path, key), masked, local_roles) for key, item in value.items()}
    version = value.get("version")

    def mask(key):
        if key in result:
            result[key] = "LOCAL"
            masked.append((*path, key))

    if shared:
        for key in _LOCAL_FIELDS.get(version, ()):
            mask(key)
        if set(value) == {"sim_start", "sim_limit", "real_start", "real_limit"}:
            mask("real_start")
        if version in ("RuntimeGlobals1", "RuntimeGlobals2", "RuntimeGlobals3", "RuntimeGlobals4"):
            mask("render_rng")
        if version == "Controller1":
            for key in ("release_timer", "joy_accel_timer", "key_accel_timer"):
                result[key]["sim_start"] = "LOCAL"
                masked.append((*path, key, "sim_start"))
            if len(path) >= 2 and path[-2:] == ("player_controller", 0):
                mask("team")
        if version == "Activity1":
            for key in ("player_team", "team_funds_share", "funds_contribution", "human", "actor_links"):
                result[key][0] = "LOCAL"
                masked.append((*path, key, 0))
        if version == "GameActivity1":
            for key in ("observation_target", "death_view_target", "actor_cursor", "landing_zone"):
                result[key][0] = "LOCAL"
                masked.append((*path, key, 0))
        if version in ("InventoryMenuGUI1", "InventoryMenuGUI2") and path[-3:] == ("player_ui", 0, "inventory"):
            mask("center")
            if local_roles:
                def remap(item, location):
                    if isinstance(item, dict):
                        if item.get("version") == "GUIEntity1" and item["value"] is not None:
                            reference = item["value"]
                            if reference["uid"] in local_roles:
                                reference["uid"] = local_roles[reference["uid"]]
                                masked.append((*location, "value", "uid"))
                        for key, child in item.items():
                            remap(child, (*location, key))
                    elif isinstance(item, list):
                        for index, child in enumerate(item):
                            remap(child, (*location, index))
                remap(result, path)
        if version in ("SLBackground1", "SLBackground2"):
            for key in ("offset", "auto_offset"):
                mask(key)
            result["scroll_timer"]["sim_start"] = "LOCAL"
            masked.append((*path, "scroll_timer", "sim_start"))
            bitmap = result["back_bitmap"]
            if isinstance(bitmap, dict) and bitmap.get("version") == "GUIBitmap1" and bitmap["value"] is not None:
                bitmap["value"]["pixels"] = "LOCAL"
                masked.append((*path, "back_bitmap", "value", "pixels"))
        if version == "TerrainLayer1" and len(path) >= 3 and path[-3:-1] == ("terrain_metadata", "terrain_layers"):
            mask("offset")
    if version == "SceneRuntime1" and isinstance(result["entity"], dict):
        entity = result["entity"]
        if entity.get("preset_name") == snapshot_name:
            entity["preset_name"] = b"SNAPSHOT"
    return result
