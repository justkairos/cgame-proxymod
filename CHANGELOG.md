# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]
### Added
- Add support for defrag versions 1.91.29, 1.91.30 and 1.91.31.
- Pitch hud which marks one or more pitch angles, e.g. `mdd_pitch 71 66`.
- Yaw hud which marks one or more yaw angles, e.g. `mdd_yaw 90 270` (`mdd_yaw_ywh`, `mdd_yaw_rgba`).
- Bounding box `mdd_bbox`. It uses shader `bbox_nocull` and draws the full bbox with `1` and only the bottom with `2`.
- Grenade prediction tools that act on your own grenades, driven through the engine's `+forward`/`+moveup`/`+attack`/`+left`/... commands:
  `mdd_grenade_chase` (walk/run to the predicted explosion spot), `mdd_grenade_aim` (turn the view onto it), `mdd_grenade_jump` and `mdd_grenade_shoot` (jump and/or fire in the last frame(s) before it explodes), `mdd_grenade_last` (target the last instead of the next grenade to explode) and `mdd_grenade_lead` (act that many ms earlier). Hold-to-enable variants `+mdd_grenade_chase/aim/jump/shoot` for binds.
- `+mdd_jumpshoot` (jump then fire as soon as grounded and ready), `+mdd_rocketsync` (jump + fire right before your next rocket hits) and `+mdd_rocketstack` (fire so a second rocket lands together with one already in flight), `+mdd_overbounce_jump` (jump right after an overbounce converts your fall speed), `+mdd_overbounce_zero` (kill horizontal velocity in the air for a vertical OB).
- All automated jump/attack input goes through one randomizer: `mdd_input_stagger_min/max` (holds, pair release stagger, jumpshoot press gap, 256-pattern no-repeat history), `mdd_input_debug` shows the real timings. Replaces `mdd_input_time`/`mdd_input_time_randomize` and `mdd_jumpshoot_stagger_min/max`.
- `+mdd_autojump` (jump in the landing usercmd, randomized hold) with `mdd_autojump_stagger_min/max` and `mdd_autojump_debug`; `mdd_jumpshoot_debug` reports the real key timings of a jump-shot.
- `+mdd_grenade_sync` / `mdd_grenade_sync`: jump + shoot timed to a grenade button (first mover the prefired grenade touches), second grenade timed by its own predicted flight (`mdd_grenade_sync_offset`).
- Input replay: `mdd_replay_record/telerecord/stoprecord/start/telestart/stop`, with exact view angle playback.
- Predicted player trajectory `mdd_player_trajectory_strafe_modes`. Draws where the player's hitbox will be over the next N simulated frames under three combinable models (passive/strafebot/accel-percent), tracing world geometry, nearby solid entities, jump pads and teleporters. See `docs/mdd_player_trajectory.md` for the full cvar/cvar-bit reference.
- Export `proxymod_getPs()`, a handle to the cgame's predicted `playerState_t`, so that clients can read it through proxymod.

### Changed
- Don't draw hud when using freecam, `cg_draw2D 0` or `+scores`.

### Fixed
- Correctly position Snap-HUD when roll is not zero. ([#8](https://github.com/Jelvan1/cgame_proxymod/pull/8))
- `entityType_t` (`bg_public.h`) matched an outdated Quake 3 ordering with four entity types modern client engines no longer define, silently shifting every later value (e.g. jump pad/teleporter detection) by 1-4.
- `helpTable[]` in `help.c` had no headroom above the number of modules registering cvar help, so one more module than the array could hold would silently overflow it and corrupt adjacent memory - a real crash risk with no compile-time warning.

## [1.4.0] - 2021-03-16
### Added
- Add support for defrag versions 1.91.24, 1.91.25, 1.91.26 and 1.91.27.
- New cvar `mdd_snap_min_speed`. Like cvar `mdd_cgaz_min_speed`, a minimum velocity is required to show the hud.

### Changed
- Don't show the compass and the timer hud by default.
- Make compass, CampingGaz-HUD and Snap-HUD thinner by default.
- Rename `mdd_cgaz_speed` cvar to `mdd_cgaz_min_speed`.
- Rename `mdd_snap_speed` cvar to `mdd_snap_scale` and change the default accordingly.

## [1.3.1] - 2020-05-05
### Changed
- Only support defrag version 1.91.26.

## [1.3.0] - 2020-04-19
### Added
- Compass which is more accurate/flexible than `df_hud_cgaz`.
- New cvar `mdd_projection`. Determines how angles will be projected on the screen. This affects the compass, CampingGaz-HUD and Snap-HUD.

## [1.2.0] - 2020-04-13
### Added
- New command `mdd_help`. Provides information about cvars.

### Changed
- Invert meaning of `mdd_ammo`'s 2nd bit from the right.
- Extract the *trueness* bits from the `mdd_cgaz` cvar into `mdd_cgaz_trueness`.
- Rename `mdd_snap` cvar to `mdd_snap_trueness`.
- Rename `mdd_snap1` cvar to `mdd_snap`.
- Rename all `mdd_snap1_` cvars to `mdd_snap_`.

### Deprecated
- Wrong snapzones are shown when the *ground* bit is enabled in `mdd_snap_trueness`. Friction is not taken into account and will not happen in the near future.

### Removed
- Don't display second snaphud and remove all its `mdd_snap2` cvars.

## [1.1.3] - 2020-02-10
### Changed
- Draw all vanilla/defrag 2D hud elements (e.g. crosshair) on top of CampingGaz-HUD and Snap-HUD.

### Fixed
- Correctly display infinite ammo (i.e. -1).
- Draw fired grenade paths even after switching weapons.

## [1.1.2] - 2019-11-23
### Changed
- Rename `mdd_time` into `mdd_timer` to align more with the other commands of the timer hud.

## [1.1.1] - 2019-11-22
### Changed
- Don't bloat q3config.cfg and improve cross-platform config compatibility by only archiving cvars that have non-default values.

## [1.1.0] - 2019-11-22
### Added
- New cvar `mdd_fov`. When different from `0`, CampingGaz-HUD and Snap-HUD will use this fov instead of `cg_fov`.

### Removed
- Bounding box trigger display since engines support this now (`scr_triggers_draw 1` and `scr_clips_draw 1`).

## [1.0.0] - 2019-11-21
### Added
- Ammo and jump hud extended from [q3df](https://github.com/q3df/cgame_proxymod).
- Grenade, rocket, timer, trigger huds extended from [krsh732](https://github.com/krsh732/cgame_proxymod).
- Option to filter other player sounds extended from [krsh732](https://github.com/krsh732/cgame_proxymod).
- Initial CampingGaz-HUD which is more accurate/flexible than `df_hud_cgaz`.
- Initial Snap-HUD which is more accurate/flexible than `scr_hud_snap`.

[Unreleased]: ../../compare/v1.4.0...HEAD
[1.4.0]: ../../compare/v1.3.1...v1.4.0
[1.3.1]: ../../compare/v1.3.0...v1.3.1
[1.3.0]: ../../compare/v1.2.0...v1.3.0
[1.2.0]: ../../compare/v1.1.3...v1.2.0
[1.1.3]: ../../compare/v1.1.2...v1.1.3
[1.1.2]: ../../compare/v1.1.1...v1.1.2
[1.1.1]: ../../compare/v1.1.0...v1.1.1
[1.1.0]: ../../compare/v1.0.0...v1.1.0
[1.0.0]: ../../releases/tag/v1.0.0
