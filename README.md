# mDd client Proxymod

[![build](../../workflows/build/badge.svg)](../../actions?query=workflow%3Abuild)

This proxymod helps players to train their abilities in the Quake III Arena modification DeFRaG. It adds hud elements to the standard defrag hud.

## Installation
Quick and easy:
  1. Find the right *cgame* platform specific binary in the [latest release](../../releases/tag/latest). To know which binary to choose, run `\version` in your engine's console. This will tell you *Windows vs Linux vs macOS* and *32-bit vs 64-bit*.

|            | **Windows** | **Linux** | **macOS** |
| :--------: | :---------: | :-------: | :-------: |
| **32-bit** | [cgamex86.dll](../../releases/download/latest/cgamex86.dll) | [cgamei386.so](../../releases/download/latest/cgamei386.so) | |
| **64-bit** | [cgamex86_64.dll](../../releases/download/latest/cgamex86_64.dll) | [cgamex86_64.so](../../releases/download/latest/cgamex86_64.so) | [cgameaarch64.dylib](../../releases/download/latest/cgameaarch64.dylib) (Apple Silicon), [cgamex86_64.dylib](../../releases/download/latest/cgamex86_64.dylib) (Intel) |

  2. Download and copy this binary into the *defrag* folder of your Quake III Arena directory.
  3. Put `seta vm_cgame 0` in your config file (e.g. *defrag/autoexec.cfg*).

You've correctly installed the proxymod if you load your favorite map and you see the following colored text in the console: `[mDd] cgame-proxy X.X.X`.

## Changelog
Please see [CHANGELOG](CHANGELOG.md) for notable changes between [releases](../../releases).

## Configuration
All commands and cvars start with `mdd_` and can be listed by typing this prefix in the console followed by pressing the *tab* key. Information about those cvars can be requested with `mdd_help <cvar>`.

Default values can be shown by typing the cvar into the console followed by pressing *enter* key. There are a few things worth noting:
  * Some cvars end with `_w`, `_xh` or even `_xywh`. Here `w` stands for width, `h` for height and `x` and `y` for the coordinates. Multiple values are separated by spaces.
  E.g., `mdd_ammo_graph_xywh 610 100 24 24`, `mdd_cgaz_yh 180 12`.
  * Some cvars have binary-literals as default values. They start with `0b` followed by a sequence of `1`'s and `0`'s.
  E.g., `mdd_ammo 0b0011`, `mdd_cgaz_trueness 0b110`.
  Note that it's not necessary to have the same total number of `1`'s and `0`'s as there are *different items* for these cvars, or even use the binary representation altogether. You can still use the good old decimal equivalent (or the octal and hexadecimal representation to impress your friends).

## Examples
### `mdd_ammo 0b1101`
  * draw the ammo hud
  * show the ammo even if you don't have the weapon
  * use gun icons instead of ammo icons
  * use 3D models instead of 2D models

So if you want to use 2D models, simply use `mdd_ammo 0b0101`.

### `mdd_cgaz_trueness 0b110`
  * don't show true jump/crouch zones, ignore their influence
  * show true CPM air control zones
  * show true ground zones

In-game help for all of it: the `help/` folder holds one `system/help/<name>.txt` per cvar/command in defrag's own help format. Copy them into `defrag/system/help/` and use `mdd_help <name>` or defrag's `? <name>`; `mdd_help mdd` is the overview.

### Grenade prediction tools
These cvars act on **your own** grenades and only need the proxymod (no engine changes). They work by pressing the engine's own virtual buttons (`+forward`, `+moveup`, `+attack`, `+left`, ...) from the cgame, so your physical keys keep working and nothing touches the mouse.
  * `mdd_grenade_chase 1` — after you fire a grenade, run/walk to the spot where it will explode using movement keys only (within ~2 units horizontally, walking when close).
  * `mdd_grenade_aim 1` — keep the camera aimed at the predicted explosion point (within ~0.25°). This is done through `+left`/`+right`/`+lookup`/`+lookdown` with `cl_yawspeed`/`cl_pitchspeed` set per frame; your original values are restored afterwards.
  * `mdd_grenade_jump 1` — jump in the frame right before the grenade explodes.
  * `mdd_grenade_shoot 1` — fire in the frame right before the grenade explodes. With `mdd_grenade_jump 1` the jump happens one frame earlier so you jump, then shoot, then the grenade goes off.
  * `mdd_grenade_sync 1` / `+mdd_grenade_sync` — jump + shoot timed to a *grenade button* (a func_door with health, which detonates a grenade on contact): tap the key (a latch: before or after the prefire) and you jump before the first grenade touches it while your second shot (grenade, rocket, plasma, anything) is fired so it lands `mdd_grenade_sync_offset` ms (default 0) after the first. The first mover a grenade touches is taken to be the button; if it bounces off instead, the next mover contact on the new trajectory is used; acts exactly once per press and disarms; `mdd_grenade_sync 1` re-arms itself once no such grenade is in the air.
  * `mdd_grenade_last 1` — when several grenades are live, target the one that explodes *last* instead of the one that explodes *next*.
  * `mdd_grenade_lead X` — perform the jump/shot `X` ms earlier than the predicted explosion (default `8`, one server frame at sv_fps 125; negative is allowed). Your ping is already compensated for: the usercmd is timed so the *server* runs it just before the explosion.
  * `mdd_grenade_jump_lead X` — extra lead for the jump only (default `24` ms). A late shot still fires but a late jump is lost because the blast has already lifted you, so the jump is biased early; the shot stays tight.
  * `mdd_grenade_debug 1` — print to the console when a grenade starts being tracked and when the jump/shot is pressed, with the timing numbers.

Each of the first four also exists as a hold-to-enable button command, so you can leave the cvars at `0` and drive them from binds: `+mdd_grenade_chase`, `+mdd_grenade_aim`, `+mdd_grenade_jump`, `+mdd_grenade_shoot`. A feature is active while its cvar is `1` *or* its key is held. Multiple can share a key, e.g. `bind shift "+mdd_grenade_jump;+mdd_grenade_shoot"`.

The explosion point is predicted by replaying the server's missile code (bounces, the 0.65 velocity loss and the "come to rest" check) against the world geometry; players and movers are not considered. The explosion time is `trTime + 50 + 2500` ms, i.e. exactly what the server uses.

All automated jump/attack presses are held for `mdd_input_time` ms (default `32`) plus or minus a random `mdd_input_time_randomize` ms (default `12`), so they show up on input huds in demos without looking machine-made. The press itself always lands in the very next usercmd; only the release is affected.

### Jump / rocket tools
Hold-to-enable buttons in the same spirit, for your own rockets:
  * `+mdd_jumpshoot` — while held, as soon as you are on the ground and your weapon is ready, jump and fire (rocket jump, plasma hop, ...). The four key events are staggered like a human: `+moveup`, then `+attack`, `-moveup`, `-attack`, each a random `mdd_jumpshoot_stagger_min`..`mdd_jumpshoot_stagger_max` ms (default 8..64) after the previous one; the last 256 patterns are remembered and never repeated; set both to 0 for the tightest timing (consecutive usercmds). No two automated key events are ever sent in the same usercmd, so a demo never shows a `+moveup;+attack`-style simultaneous press or release. Re-arms every time you land while the key stays held. Also handles upward jump pads: predicts the touch and the rocket flight and times the shot so the rocket impact lands `mdd_jumpshoot_pad_delay` ms relative to the touch (negative = before); falls back to the first usercmd after the launch. `mdd_jumpshoot_debug 1` prints the real usercmd times of all four key events of a jump-shot; `mdd_rocket_debug 1` prints the peak upward speed after every pad launch for tuning.
  * `+mdd_rocketsync` — while held, times a jump + shot so the shot is the last usercmd before your next rocket hits (`mdd_rocketsync_lead`, default `8`, and `mdd_rocketsync_jump_lead`, default `24`, work like the grenade leads).
  * `+mdd_rocketstack` — while held with the rocket launcher out and one of your rockets still in flight, fires at the exact moment a rocket launched from where you are would hit its surface at the same time as the existing one. Typical use: prefire two rockets down a drop, fall past them, land, hold `+mdd_rocketsync` for the triple. `mdd_rocketstack_offset` (ms, default `0`) shifts the moment.
  * `mdd_rocket_debug 1` — console timing output for the three above.
  * `+mdd_autojump` — while held, jump in the landing usercmd (predicted one cmd ahead) or whenever grounded; each press held a random `mdd_autojump_stagger_min`..`mdd_autojump_stagger_max` ms (default 8..64). `mdd_autojump_debug 1` prints the real press/release usercmd times.
  * `+mdd_overbounce_jump` — while held, if you land with an overbounce, jump on the usercmd right after the one that converted your fall speed (a jump any earlier would cancel the OB). Only fires for OBs that keep you on the floor (horizontal/"go" OBs); a vertical OB launches you anyway. `mdd_overbounce_debug 1` prints what it saw.
  * `+mdd_overbounce_zero` — while held in the air, uses the movement keys (walking, and ducking on CPM, for the last few units) to bring your horizontal velocity to exactly 0 so the landing is a vertical OB. Works best on VQ3 physics; on CPM the side-strafe air acceleration overshoots near zero, so it only uses forward/back close in and may leave a sideways unit or two.

### Input replay
Records your usercmds (movement keys, buttons, weapon, view angles) and plays them back through the engine's own buttons. Files go to `defrag/replays/<name>.mdd` (plain text).
  * `mdd_replay_record <name>` — runs defrag's `set saveposname <name>; savepos`, stores the resulting start position in the replay and records from there (no cheats needed for this). Stand still when you start for an exact playback.
  * `mdd_replay_telerecord <name>` — starts recording with the first usercmd after the next teleporter.
  * `mdd_replay_stoprecord` — stops and writes the file.
  * `mdd_replay_start <name>` — teleports you to the stored start position (`vstr` → `placeplayer`, which needs cheats) and plays the input back from the moment the teleport shows up.
  * `mdd_replay_telestart <name>` — plays the input back starting with the first usercmd after you exit the *same* teleporter the replay was recorded from (no cheats needed). The recording stores the exit position; teleports that land elsewhere (other teleporters, respawns) are ignored. Stays armed until you stop it.
  * `mdd_replay_stop` — stops playback (it also stops by itself at the end).
  * Both `*start` commands take several names (`mdd_replay_telestart a b c`, up to 16) and play them back to back; only the first replay of a chain gets the start synchronisation. With `sv_cheats 0`, `mdd_replay_start` skips `placeplayer` and just plays the input from where you are.
  * `mdd_replay_debug 1` — console output about sync points.

View angles are reproduced exactly (to the network short) by pulsing `+left/+right/+lookup/+lookdown` for 1 ms with `cl_yawspeed`/`cl_pitchspeed` set per frame, which makes the turn independent of frame time; your cvars are restored afterwards. Keep your hands off mouse and keys while it plays (keys you are still holding after a teleporter are cancelled out as far as possible, but let go as soon as you can), and don't hold other `+mdd_` automation at the same time. Record and play at `com_maxfps 125` (so your usercmds sit on the server's 8 ms grid; at e.g. 133 fps the server drops and doubles commands and partial key frames get distorted) with `timescale 1`, on `pmove_fixed 1` / `g_synchronousClients 0` (also offline: with `g_synchronousClients 1` the server samples one of your commands per server frame at its own pace, which is not reproducible). With `pmove_fixed` the playback matches inputs per 8 ms grid slot exactly the way the server rounds them, so frame-time jitter between the two sessions doesn't shift anything. On a remote server the first ~ping ms after a teleport are already sent before the proxymod can see the teleport, so those are your own input; a local server is exact.

## Building
This C/C++ proxymod project uses CMake to control the building process. Both GitHub Actions and Visual Studio Code build tasks are available and basically do the following steps:
1. Generate input files for a native build system.
   To generate standard Makefiles for a 32-bit release build on Windows with gcc, do:
   ```
   $ cmake                                         \
       -S <repo_path>                              \
       -B <build_path>                             \
       -DBINARY_NAME=cgamex86                      \
       -DCMAKE_BUILD_TYPE=Release                  \
       -DCMAKE_C_COMPILER=gcc                      \
       -DCMAKE_CXX_COMPILER=g++                    \
       -DCMAKE_C_FLAGS="${CMAKE_C_FLAGS} -m32"     \
       -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS} -m32"
   ```
   To generate Ninja files for a 64-bit debug build on Linux with clang, do:
   ```
   $ cmake                                         \
       -S <repo_path>                              \
       -B <build_path>                             \
       -G Ninja                                    \
       -DBINARY_NAME=cgamex86_64                   \
       -DCMAKE_BUILD_TYPE=Debug                    \
       -DCMAKE_C_COMPILER=clang                    \
       -DCMAKE_CXX_COMPILER=clang++                \
       -DCMAKE_C_FLAGS="${CMAKE_C_FLAGS} -m64"     \
       -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS} -m64"
   ```
   Note that the binary name for a 32-bit Windows build is `cgamex86`, while `cgamei386` on Linux. The binary name for a 64-bit build, however, is `cgamex86_64` on both Windows and Linux.
2. Build the source code.
   ```
   $ cmake --build <build_path>
   ```
3. Optionally, you can strip and directly install the new binary into the *defrag* folder of your Quake III Arena directory by specifying an install prefix.
   ```
   $ cmake --install <build_path> --prefix <quake3_path>/defrag --strip
   ```
4. Profit.
