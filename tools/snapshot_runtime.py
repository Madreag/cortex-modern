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
SCHEMAS["Controller2"] = [*SCHEMAS["Controller1"], ("synced_order_disable_tick", "n")]
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
SCHEMAS["FrameMan2"] = [*SCHEMAS["FrameMan1"], ("fonts", array(4, "o"))]
SCHEMAS["FrameMan3"] = [*SCHEMAS["FrameMan2"], ("palette", "o")]
SCHEMAS["FramePalette1"] = [("file", "o"), *fields("palette default_palette current_palette rgb_table", "s"),
    *fields("black almost_black alpha"), ("prune_timer", TIMER),
    ("caches", array(12, sequence(structure(("key", array(4)), ("bytes", "s"), ("last_access", "n"))))),
    ("selected_mode", "n"), ("selected_key", array(4)), ("external_bytes", "s"), ("blenders", array(7, "s")), ("blend_values", array(5))]
SCHEMAS["MovableMan1"] = fields("splash_ratio max_dropped_items settling_enabled subtraction_enabled sim_update_frame")
SCHEMAS["MovableMan2"] = [*SCHEMAS["MovableMan1"], ("borrowed_references", sequence(structure(("owner", "n"), ("references", sequence("n")))))]
SCHEMAS["SceneMan1"] = [*fields("layer_draw_mode raycast_visualizations pixel_check_visualizations last_updated_screen second_structure_pass"),
    *fields("calc_timer clean_timer", TIMER), ("scrap_compacting_height", "n")]
SCHEMAS["SceneMan2"] = [*SCHEMAS["SceneMan1"], ("materials", "o")]
SCHEMAS["MaterialCatalog1"] = [("count", "n"), ("names", NUMBER_MAP), ("palette", array(256, "o")), *fields("copies presets", sequence("o"))]
SCHEMAS["MaterialReference1"] = fields("kind index")
SCHEMAS["RuntimeGlobals1"] = [*fields("sim_rng render_rng", "s"), *fields("timer movable scene camera", "o")]
SCHEMAS["RuntimeGlobals2"] = [*SCHEMAS["RuntimeGlobals1"], ("frame", "o")]
SCHEMAS["RuntimeGlobals3"] = [*SCHEMAS["RuntimeGlobals2"], *fields("default_activity_type default_activity_name", "s"),
    *fields("in_activity needs_restart needs_resume resuming_from_pause skip_pause_menu start_activity_resumed")]
SCHEMAS["RuntimeGlobals4"] = [*SCHEMAS["RuntimeGlobals3"], ("gui_input", "o")]
SCHEMAS["RuntimeGlobals5"] = [*SCHEMAS["RuntimeGlobals4"], ("audio", "o")]
SCHEMAS["RuntimeGlobals6"] = [*SCHEMAS["RuntimeGlobals4"], *fields("input postprocess audio", "o")]
SCHEMAS["RuntimeGlobals7"] = [*SCHEMAS["RuntimeGlobals4"], *fields("input postprocess primitive audio", "o")]
SCHEMAS["RuntimeGlobals8"] = [*SCHEMAS["RuntimeGlobals4"], *fields("input postprocess primitive music audio", "o")]
SCHEMAS["RuntimeGlobals9"] = [*SCHEMAS["RuntimeGlobals4"], *fields("input postprocess primitive gui_sound music audio", "o")]
SCHEMAS["PrimitiveMan1"] = [("images", sequence("o")), ("vertices", sequence(VECTOR)), ("primitives", sequence("o"))]
SCHEMAS["PrimitiveValue1"] = SCHEMAS["PrimitiveMan1"]
SCHEMAS["GraphicalPrimitive1"] = [("type", "n"), *fields("start end", VECTOR), *fields("draw_radius_squared color player blend_mode"),
    ("blend_amounts", array(4)), ("depth", "n")]
PRIMITIVE_FIELDS = {
    1: fields("thickness"), 2: fields("start_angle end_angle radius thickness"),
    3: fields("guide_a guide_b", VECTOR), 4: [], 5: [], 6: fields("corner_radius"), 7: fields("corner_radius"),
    8: fields("radius"), 9: fields("radius"), 10: fields("horizontal_radius vertical_radius"), 11: fields("horizontal_radius vertical_radius"),
    12: fields("point_a point_b point_c", VECTOR), 13: fields("point_a point_b point_c", VECTOR),
    14: [("vertices", sequence("n"))], 15: [("vertices", sequence("n"))],
    16: [("text", "s"), *fields("small alignment rotation"), ("target_alignment", VECTOR), ("image", "n")],
    17: [*fields("rotation flipped_h flipped_v scale frame icon image"), ("sprite", "o")],
}
SCHEMAS["PostProcessMan1"] = [("suppressed", "n"), ("bitmaps", sequence("o")), ("queues", "o")]
SCHEMAS["PostProcessMan2"] = SCHEMAS["PostProcessMan1"]
POST_EFFECT = structure(*fields("bitmap hash angle strength"), ("position", VECTOR), *fields("attached_moid attached_uid"))
SCHEMAS["PostProcessQueues1"] = [("effects", array(6, sequence(POST_EFFECT))),
    ("screen_glow_boxes", sequence(structure(("corner", VECTOR), *fields("width height")))),
    ("glow_areas", sequence(array(4))), ("glow_slots", array(3, array(2))), ("temps", sequence(array(2)))]
SCHEMAS["InputMapping1"] = [("preset_description", "s"), *fields("key mouse_button direction_mapped joy_button axis direction")]
SCHEMAS["InputScheme1"] = [("active_device", "n"), ("device_id", array(2)), *fields("preset deadzone_type deadzone digital_aim_speed"), ("mappings", array(34, "o"))]
SCHEMAS["Gamepad1"] = [*fields("device_index joystick_id"), *fields("axes digital_axes buttons pressed released", sequence("n"))]
SCHEMAS["UInputMan::Keyboard1"] = [("id", "n"), *fields("states changed pressed released", array(512))]
SCHEMAS["UInputMan::Mouse1"] = [("id", "n"), *fields("states changed pressed released", array(4)),
    *fields("position relative_motion analog_aim", VECTOR), *fields("wheel_change relative_mode")]
SCHEMAS["UInputMan1"] = [*fields("keyboards mice", sequence(structure(("id", "n"), ("state", "o")))),
    *fields("previous_joysticks changed_joysticks", sequence("o")), *fields("skip_special_input joystick_count"), ("text_input", "s"),
    ("override_input", "n"), ("control_schemes", array(4, "o")), *fields("mouse_sensitivity trap_mouse mouse_trap_radius"),
    ("mouse_bounds", array(4)), *fields("last_cursor_device force_disable_multi enable_multi player_devices_known disable_keyboard disable_mouse_moving prepare_enable_mouse_moving")]
SCHEMAS["AudioParameter1"] = fields("index type integer number boolean")
SCHEMAS["AudioEffect1"] = [*fields("index type active bypass"), ("wet_dry", array(3)), ("parameters", sequence("o"))]
SCHEMAS["AudioControl1"] = [*fields("paused muted ramp volume pitch low_pass"), ("reverb", array(4)),
    *fields("mode outputs inputs"), ("mix", sequence("n")), *fields("has_delay_start has_delay_end delay_stops delay_start delay_end"),
    ("fades", sequence(array(2))), ("spatial", "n"), *fields("position velocity cone_orientation", array(3)),
    *fields("minimum_distance maximum_distance"), ("cone", array(3)),
    *fields("direct_occlusion reverb_occlusion spread level doppler custom_distance_filter custom_level center_frequency"), ("effects", sequence("o"))]
SCHEMAS["AudioVoice1"] = [*fields("identity owner"), ("path", "s"), *fields("playing bus priority loops position loop_start loop_end frequency minimum_audible_distance"), ("control", "o")]
SCHEMAS["AudioSample1"] = [("path", "s"), *fields("mode loop_start loop_end frequency minimum_distance maximum_distance priority loops"), ("cone", array(3))]
SCHEMAS["AudioEvent1"] = [*fields("state sound_file_hash channel immobile attenuation_start custom_pan panning_multiplier loops priority affected_by_global_pitch"),
    ("position", VECTOR), *fields("volume pitch fade_out_time")]
SCHEMAS["AudioRuntime1"] = [*fields("enabled next_voice next_sound_container mute_master mute_music mute_sounds mute_on_focus_loss master_volume music_volume sounds_volume global_pitch panning listener_z minimum_panning music_muffled multiplayer"),
    ("player_positions", sequence(VECTOR)), ("listeners", sequence(array(4, array(3)))), ("groups", array(4, "o")),
    *fields("samples voices", sequence("o")), ("minimum_distances", sequence(array(2))), ("events", array(4, sequence("o")))]
SCHEMAS["AudioRuntime2"] = [*SCHEMAS["AudioRuntime1"], ("audibility", sequence("o"))]
SCHEMAS["AudioRuntime3"] = [*SCHEMAS["AudioRuntime2"], *fields("deferred_sound_op_tick deferred_sound_op_ordinal")]
SCHEMAS["CommittedAudibility1"] = fields("object_uid tick phase occurrence ordinal peer frame value")
SCHEMAS["SoundPlayback1"] = [("voices", sequence("o"))]
SCHEMAS["SoundContainer1"] = [("entity", "o"), ("identity", "n"), ("playing_channels", sequence("n")),
    *fields("overlap_mode bus immobile attenuation_start custom_pan panning_multiplier loops properties_current priority affected_by_global_pitch"),
    ("position", VECTOR), *fields("pitch pitch_variation volume faded_out paused music_pre_entry_time music_exit_time")]
SCHEMAS["MusicSample1"] = [("content", "o"), ("backend", "s"), ("offset", VECTOR), *fields("minimum attenuation")]
SCHEMAS["MusicSet1"] = [("cycle", "n"), ("selection", array(2)), *fields("samples subsets", sequence("o"))]
SCHEMAS["MusicSound1"] = [*fields("native set", "o")]
SCHEMAS["MusicSection1"] = [("entity", "o"), ("transitions", sequence("o")), ("last_transition", "n"), ("transition_queue", sequence("n")),
    ("sounds", sequence("o")), ("last_sound", "n"), ("queue", sequence("n")), ("cycle", "n"), ("type", "s")]
SCHEMAS["MusicSong1"] = [*fields("entity fallback", "o"), ("sections", sequence("o"))]
SCHEMAS["MusicMan1"] = [("playing", "n"), *fields("interrupting song", "o"), *fields("next_type current_type", "s"), ("next_section", "n"),
    *fields("previous current", "o"), ("next_sound", array(3)), ("fade_timer", TIMER), ("fade_previous", "n"), ("timer", TIMER), *fields("paused_time return_to_dynamic")]
SCHEMAS["GUISoundSample1"] = SCHEMAS["MusicSample1"]
SCHEMAS["GUISoundSet1"] = SCHEMAS["MusicSet1"]
SCHEMAS["GUISoundContainer1"] = SCHEMAS["MusicSound1"]
SCHEMAS["GUISound1"] = [("sounds", array(27, "o"))]
SCHEMAS["GUISharedInput1"] = [("override_input", "n"), *fields("events states previous_states", array(4, array(3))), *fields("x y", array(4))]
SCHEMAS["GUISharedInput2"] = [*SCHEMAS["GUISharedInput1"], *fields("text_active text_x text_y text_width text_height text_cursor")]
SCHEMAS["Activity1"] = [
    *fields("state paused allows_saving test_activity"), *fields("description scene_name", "s"),
    *fields("max_players min_teams difficulty craft_orbit_at_edge campaign_stage player_count"),
    *fields("active human player_screen view_state", array(4)), ("death_timer", array(4, TIMER)), ("team_names", array(4, "s")),
    ("team_count", "n"), *fields("team_active player_team team_deaths team_ai_skill team_funds team_funds_share funds_changed funds_contribution had_brain brain_evacuated", array(4)),
    ("player_controller", array(4, "o")), ("message_timer", array(4, TIMER)), *fields("saved_encoded saved_strings", STRING_MAP),
    ("saved_numbers", NUMBER_MAP), ("actor_links", array(4, array(3)))]
SCHEMAS["Activity2"] = [*SCHEMAS["Activity1"], ("team_icons", array(4, "o"))]
SCHEMAS["Activity3"] = [*SCHEMAS["Activity1"], ("team_icons", "o")]
SCHEMAS["Icon1"] = [("base", "o"), ("bitmap_file", "o"), ("frame_count", "n"),
                    ("images", sequence("o")), ("indexed_frames", sequence("n")), ("true_color_frames", sequence("n"))]
SCHEMAS["IconSet1"] = [("images", sequence("o")), ("icons", sequence("o"))]
SCHEMAS["IconValues1"] = [("base", "o"), ("bitmap_file", "o"), ("frame_count", "n"),
                         ("indexed_frames", sequence("n")), ("true_color_frames", sequence("n"))]
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
SCHEMAS["GUIEntity2"] = [("value", optional(structure(*fields("kind uid placed_set placed_index"), *fields("class preset module", "s"))))]
SCHEMAS["GUIBitmap1"] = [("value", optional(structure(*fields("depth width height clip left right top bottom"), ("pixels", "s"))))]
SCHEMAS["Bitmap1"] = [*fields("width height depth"), ("pixels", "s")]
SCHEMAS["Bitmap2"] = [*SCHEMAS["Bitmap1"], *fields("clip left top right bottom")]
SCHEMAS["SharedBitmap1"] = [("path", "s"), ("cache_slot", "n"), ("pixels", "o")]
SCHEMAS["GUIImage1"] = [("value", optional(structure(("file", "s"), ("bitmap", "o"))))]
SCHEMAS["GUIImage2"] = [("value", optional(structure(("file", "o"), ("bitmap", "o"))))]
SCHEMAS["GUIFont1"] = [("name", "s"), *fields("height main_color current_color character_cap kerning leading"), ("bitmap", "o"),
    ("characters", array(256, structure(*fields("width height offset")))), ("color_cache", sequence(structure(("color", "n"), ("bitmap", "o")))), ("current_bitmap", "n")]
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
# BuyMenuGUI3 keys the per-module expansion flags by module name; they ride in the tail like version 2's.
SCHEMAS["BuyMenuGUI3"] = SCHEMAS["BuyMenuGUI2"]
SCHEMAS["Entity1"] = [*fields("preset_name copied_from description reader_position", "s"), *fields("original module random_weight"), ("groups", STRINGS)]
SCHEMAS["Matrix1"] = [("angle", "n"), ("flipped", array(2)), ("elements", array(2, array(2))), ("elements_current", "n")]
SCHEMAS["ContentFile1"] = [*fields("path extension stem", "s"), ("is_image", "n"), ("image_info", array(3)),
    *fields("reader_position path_and_reader_position", "s"), *fields("module memory_png")]
SCHEMAS["MovableObjectRuntime1"] = [("entity", "o"), ("position", VECTOR), *fields("gold_value buyable buyable_mode team placed_by_player mo_type mass"),
    *fields("velocity previous_position previous_velocity", VECTOR), *fields("distance_travelled scale global_acceleration air_resistance air_threshold pin_strength rest_threshold"),
    *fields("forces impulses", sequence(array(2, VECTOR))), *fields("age_timer rest_timer", TIMER),
    *fields("lifetime sharpness check_terrain hits_mos"), ("ignore_timer", TIMER),
    *fields("gets_hit ignores_team ignores_atom_groups ignores_atom_groups_when_slower ignores_actors mission_critical squishable updated wrap_double_draw did_wrap moid root_moid moid_footprint ever_added"),
    ("already_hit_by", sequence("n")), *fields("velocity_oscillations settle delete hud_visible traveling requested_synced_update"),
    ("string_values", STRING_MAP), ("number_values", NUMBER_MAP), ("screen_effect_file", "o"),
    *fields("has_screen_effect screen_effect_hash effect_start_time effect_stop_time effect_start_strength effect_stop_strength effect_always_shows effect_angle inherit_effect_angle randomize_effect_angle randomize_effect_every_frame post_effect_enabled "
    "remove_orphan_radius remove_orphan_area remove_orphan_rate collision_damage penetration_damage wound_damage_multiplier apply_wound_collision apply_wound_burst_collision ignore_terrain hit_moid hit_material hit_particle_uid last_collision_frame scripted_update_interval since_scripted_update")]
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
SCHEMAS["ACrabRuntime1"] = [("icon_blink_timer", TIMER), ("stride_frame", "n"), ("paths", array(2, array(2, array(11, "o")))),
    ("aiming", "n"), ("stride_start", array(2)), ("stride_timer", array(2, TIMER)),
    *fields("aim_range_upper_limit aim_range_lower_limit lock_mouse_aim_input"), ("foot_groups", array(8, "s"))]
SCHEMAS["MOSpriteRuntime1"] = [*fields("rotation previous_rotation", "o"), *fields("angular_velocity previous_angular_velocity frame_count"),
    ("sprite_offset", VECTOR), *fields("frame animation_mode animation_duration"), ("animation_timer", TIMER),
    *fields("animation_reversing flipped forced_flip sprite_radius sprite_diameter angular_oscillations settling_material_disabled sprite_modified")]
SCHEMAS["MOSpriteRuntime2"] = [*SCHEMAS["MOSpriteRuntime1"], *fields("sprite_file icon_file", "o"),
    ("images", sequence("o")), ("frames", sequence("n")), ("icon_index", "n")]
SCHEMAS["MOSRotatingRuntime1"] = [*fields("deep_check force_deep_check deep_hardness"), *fields("travel_impulse sprite_center", VECTOR),
    *fields("orient_to_velocity recoiled"), *fields("recoil_force recoil_offset", VECTOR),
    *fields("entry_wound_sound_played exit_wound_sound_played farthest_attachment attachment_mass gib_impulse gib_wound_limit gib_blast "
    "gib_screen_shake wound_impulse_ratio detach_before_gib gib_at_lifetime gib_effect gib_loudness damage_multiplier no_damage_multiplier"), ("flash_timer", TIMER)]
SCHEMAS["MOSRotatingRuntime2"] = [*SCHEMAS["MOSRotatingRuntime1"], *fields("flip_bitmap silhouette_bitmap", "o")]
SCHEMAS["PieMenuRuntime1"] = [("entity", "o"), *fields("direction menu_mode"), ("center", VECTOR), ("rotation", "o"), ("enabled", "n"),
    *fields("animation_timer hover_timer submenu_timer", TIMER), *fields("icon_mode inner_radius background_thickness separator_size transparent background_color border_color selected_color"),
    ("quadrants", array(4, structure(*fields("enabled direction")))), *fields("current_radius cursor_visible cursor_angle cursor_visual_angle redraw submenu_redraw"),
    *fields("background_bitmap rotation_bitmap slices_bitmap", "o")]
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
    if version == "GraphicalPrimitive1":
        if result["type"] not in PRIMITIVE_FIELDS or not 0 <= result["blend_mode"] < 12:
            raise ValueError("invalid graphical primitive type or blend mode")
        result.update({name: reader.value(kind) for name, kind in PRIMITIVE_FIELDS[result["type"]]})
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
    if version in ("Bitmap1", "Bitmap2"):
        width, height, depth = result["width"], result["height"], result["depth"]
        if width < 0 or height < 0 or bool(width) != bool(height) or (width and depth not in (8, 15, 16, 24, 32)):
            raise ValueError("invalid bitmap dimensions")
        if len(result["pixels"]) != width * ((depth + 7) // 8) * height:
            raise ValueError("invalid bitmap pixel count")
        if version == "Bitmap2" and (not 0 <= result["left"] <= result["right"] <= width or not 0 <= result["top"] <= result["bottom"] <= height):
            raise ValueError("invalid bitmap clipping")
    if version == "SharedBitmap1":
        if (result["path"] and result["cache_slot"] not in (0, 1)) or (not result["path"] and result["cache_slot"] != -1):
            raise ValueError("invalid shared bitmap cache reference")
        bitmap = result["pixels"]
        if not isinstance(bitmap, dict) or bitmap.get("version") != "GUIBitmap1" or (result["path"] and bitmap["value"] is None):
            raise ValueError("invalid shared bitmap pixels")
    if version in ("GUIEntity1", "GUIEntity2") and result["value"] is not None:
        reference = result["value"]
        if not reference["class"] or reference["uid"] < 0:
            raise ValueError("invalid GUI entity reference")
        if version == "GUIEntity2":
            kind = reference["kind"]
            if kind not in (0, 1, 2) or (kind == 1) != (reference["uid"] > 0):
                raise ValueError("invalid GUI entity reference kind")
            if (kind == 2 and (not 0 <= reference["placed_set"] < 3 or reference["placed_index"] < 0)) or (kind != 2 and (reference["placed_set"] != -1 or reference["placed_index"] != 0)):
                raise ValueError("invalid GUI placed-entity reference")
    if version == "GUISharedInput2" and (result["text_active"] not in (0, 1) or result["text_width"] < 0 or result["text_height"] < 0):
        raise ValueError("invalid GUI text input state")
    if version == "GUIFont1" and not -2 <= result["current_bitmap"] < len(result["color_cache"]):
        raise ValueError("invalid GUI font bitmap reference")
    if version in ("MusicSet1", "GUISoundSet1"):
        subset, selection = result["selection"]
        if subset not in (0, 1) or selection < -1 or (selection >= 0 and selection >= len(result["subsets" if subset else "samples"])):
            raise ValueError("invalid music selection reference")
    if version == "MusicSection1":
        for pool, last, queue in (("transitions", "last_transition", "transition_queue"), ("sounds", "last_sound", "queue")):
            if result[last] != 0xffffffff and not 0 <= result[last] < len(result[pool]):
                raise ValueError("invalid music section last-sound reference")
            if len(set(result[queue])) != len(result[queue]) or any(not 0 <= reference < len(result[pool]) for reference in result[queue]):
                raise ValueError("invalid music section queue reference")
    if version == "MusicMan1":
        song = result["song"]
        count = len(song["sections"]) if isinstance(song, dict) and song.get("version") == "MusicSong1" else 0
        section = result["next_section"]
        if section != -2 and (not song or not -1 <= section < count):
            raise ValueError("invalid music next-section reference")
        section, bucket, index = result["next_sound"]
        if [section, bucket, index] != [-2, 0, -1]:
            if not song or not -1 <= section < count or bucket not in (0, 1):
                raise ValueError("invalid music next-sound owner")
            owner = song["fallback"] if section == -1 else song["sections"][section]
            if not 0 <= index < len(owner["transitions" if bucket else "sounds"]):
                raise ValueError("invalid music next-sound reference")
    if version == "Activity3" and (not isinstance(result["team_icons"], dict) or result["team_icons"].get("version") != "IconSet1" or len(result["team_icons"]["icons"]) != 4):
        raise ValueError("invalid activity icon set")
    if version in ("PrimitiveMan1", "PrimitiveValue1", "MOSpriteRuntime2", "Icon1", "IconSet1"):
        cache_owners = set()
        for bitmap in result["images"]:
            if not isinstance(bitmap, dict) or bitmap.get("version") != "SharedBitmap1" or bitmap["pixels"]["value"] is None:
                raise ValueError("invalid runtime image pool member")
            if version in ("Icon1", "IconSet1") and bitmap["cache_slot"] >= 0:
                owner = (bitmap["cache_slot"], bitmap["path"])
                if owner in cache_owners:
                    raise ValueError("duplicate icon bitmap cache owner")
                cache_owners.add(owner)
        if version == "MOSpriteRuntime2":
            if any(not 0 <= reference <= len(result["images"]) for reference in [*result["frames"], result["icon_index"]]):
                raise ValueError("invalid sprite image reference")
        elif version in ("Icon1", "IconSet1"):
            for icon in result["icons"] if version == "IconSet1" else [result]:
                if not isinstance(icon, dict) or icon.get("version") != ("IconValues1" if version == "IconSet1" else "Icon1"):
                    raise ValueError("invalid icon value record")
                if any(not 0 <= reference <= len(result["images"])
                       for reference in [*icon["indexed_frames"], *icon["true_color_frames"]]):
                    raise ValueError("invalid icon image reference")
        else:
            if version == "PrimitiveValue1" and len(result["primitives"]) != 1:
                raise ValueError("invalid primitive value count")
            for primitive in result["primitives"]:
                if not isinstance(primitive, dict) or primitive.get("version") != "GraphicalPrimitive1":
                    raise ValueError("invalid graphical primitive record")
                if primitive["type"] in (16, 17) and not 0 <= primitive["image"] <= len(result["images"]):
                    raise ValueError("invalid graphical primitive image reference")
                if primitive["type"] in (14, 15) and any(not 0 <= reference <= len(result["vertices"]) for reference in primitive["vertices"]):
                    raise ValueError("invalid graphical primitive vertex reference")
    return result


# AudioMan drives parameter 1 of a voice's multiband EQ as the distance-muffling lowpass
# (AudioMan.cpp: setParameterFloat(1, lowpassFrequency), "Functionally inactive lowpass filter" at 22000).
_MULTIBAND_EQ, _MULTIBAND_EQ_LOWPASS = 36, 1


_LOCAL_FIELDS = {
    "TimerMan1": set("real_time sim_accumulator sim_updates_since_drawn drawn_sim_update sim_speed pace_accrued pace_trimmed pace_wall_seen pace_cap_lost pace_paused_lost pace_update_calls pace_reset_calls".split()),
    "Controller1": set("input_mode seat_mode player seat_player team next_ignore prev_ignore weapon_next_ignore weapon_prev_ignore pickup_ignore drop_ignore reload_ignore primary_hotkey_ignore".split()),
    "Controller2": set("input_mode seat_mode player seat_player team next_ignore prev_ignore weapon_next_ignore weapon_prev_ignore pickup_ignore drop_ignore reload_ignore primary_hotkey_ignore".split()),
    "Screen1": {name for name, _ in SCHEMAS["Screen1"]},
    "FrameMan1": {"flashed_last_frame", "flash_timer"},
    "FrameMan2": {"flashed_last_frame", "flash_timer"},
    "FrameMan3": {"flashed_last_frame", "flash_timer"},
    # The live Allegro colour table and blend alpha are whatever the last blit selected.
    "FramePalette1": {"selected_key", "alpha"},
    # The FMOD listener is the local camera, and a voice's PCM cursor rides the local device clock.
    "AudioRuntime1": {"player_positions", "listeners"},
    "AudioRuntime2": {"player_positions", "listeners"},
    "AudioRuntime3": {"player_positions", "listeners"},
    "AudioVoice1": {"position"},
    "ActorRuntime1": {"hud_stack"},
    "AEmitterRuntime1": {"average_burst_impulse", "average_impulse"},
    "HDFirearmRuntime1": {"ai_fire_velocity", "ai_bullet_lifetime", "ai_bullet_acceleration"},
}


def project(value, shared=False, snapshot_name=None, path=(), masked=None, local_roles=None, cross_process=False):
    """Return a structural value, projecting only the named local fields and timer anchors."""
    if masked is None:
        masked = []
    if isinstance(value, list):
        return [project(item, shared, snapshot_name, (*path, index), masked, local_roles, cross_process) for index, item in enumerate(value)]
    if not isinstance(value, dict):
        return value
    result = {key: project(item, shared, snapshot_name, (*path, key), masked, local_roles, cross_process) for key, item in value.items()}
    version = value.get("version")

    def mask(key):
        if key in result:
            result[key] = "LOCAL"
            masked.append((*path, key))

    # Timer::m_StartRealTime is a wall-clock reading (TimerMan.h), so two processes never agree on it.
    if (shared or cross_process) and set(value) == {"sim_start", "sim_limit", "real_start", "real_limit"}:
        mask("real_start")
    if shared:
        for key in _LOCAL_FIELDS.get(version, ()):
            mask(key)
        if version in ("RuntimeGlobals1", "RuntimeGlobals2", "RuntimeGlobals3", "RuntimeGlobals4", "RuntimeGlobals5", "RuntimeGlobals6", "RuntimeGlobals7", "RuntimeGlobals8", "RuntimeGlobals9"):
            mask("render_rng")
        # A voice's mixer gain and 3D blend are attenuated against that peer's own listener; the four
        # group buses carry settings, so only a control reached through audio.voices is local.
        if version == "AudioControl1" and len(path) >= 3 and path[-3] == "voices" and path[-1] == "control":
            for key in ("volume", "level"):
                mask(key)
        if version == "AudioEffect1" and value.get("type") == _MULTIBAND_EQ and len(path) >= 5 and path[-5] == "voices":
            for index, parameter in enumerate(result["parameters"]):
                if isinstance(parameter, dict) and parameter.get("index") == _MULTIBAND_EQ_LOWPASS:
                    parameter["number"] = "LOCAL"
                    masked.append((*path, "parameters", index, "number"))
        if version in ("Controller1", "Controller2"):
            for key in ("release_timer", "joy_accel_timer", "key_accel_timer"):
                result[key]["sim_start"] = "LOCAL"
                masked.append((*path, key, "sim_start"))
            if len(path) >= 2 and path[-2:] == ("player_controller", 0):
                mask("team")
        if version in ("Activity1", "Activity2", "Activity3"):
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
                        if item.get("version") in ("GUIEntity1", "GUIEntity2") and item["value"] is not None:
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


def _emit(kind):
    """Minimal zero bytes for a schema kind, for the selftest's synthetic payloads."""
    if kind == "n":
        return b"0 "
    if kind in ("s", "o"):
        return b"0  "
    if kind == "tail":
        return b""
    head = kind[0]
    if head == "optional":
        return b"0 "
    if head == "structure":
        return b"".join(_emit(sub) for _name, sub in kind[1])
    if head == "array":
        return _emit(kind[2]) * kind[1]
    return b"0 "  # sequence: a zero count


def _payload(version):
    tag = version.encode()
    return str(len(tag)).encode() + b" " + tag + b" " + b"".join(_emit(kind) for _name, kind in SCHEMAS[version])


def selftest():
    checks = []

    def check(name, ok):
        checks.append(ok)
        print(f"[snapshot-runtime-selftest] {'PASS' if ok else 'FAIL'} {name}")

    one = decode(_payload("Controller1"))
    raw_two = _payload("Controller2")
    held_two = raw_two[:-2] + b"305 "  # the last field is the synced-order hold tick
    two = decode(held_two)
    check("controller1_decodes", isinstance(one, dict) and one["version"] == "Controller1" and "synced_order_disable_tick" not in one)
    check("controller2_decodes", isinstance(two, dict) and two["version"] == "Controller2" and two["synced_order_disable_tick"] == 305)

    stale = raw_two.replace(b"Controller2", b"Controller1", 1)
    try:
        decode(stale)
        check("controller2_refused_as_controller1", False)
    except ValueError as error:
        check("controller2_refused_as_controller1", "trailing" in str(error))

    masked = []
    projected = project(two, shared=True, masked=masked)
    check("controller2_local_fields_masked", projected["player"] == "LOCAL" and projected["input_mode"] == "LOCAL" and
          projected["synced_order_disable_tick"] == 305 and projected["release_timer"]["sim_start"] == "LOCAL")
    return all(checks)


if __name__ == "__main__":
    raise SystemExit(0 if selftest() else 1)
