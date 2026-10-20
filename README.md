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

### Grenade prediction tools
These cvars act on **your own** grenades and only need the proxymod (no engine changes). They work by pressing the engine's own virtual buttons (`+forward`, `+moveup`, `+attack`, `+left`, ...) from the cgame, so your physical keys keep working and nothing touches the mouse.
  * `mdd_grenade_chase 1` — after you fire a grenade, run/walk to the spot where it will explode using movement keys only (within ~2 units horizontally, walking when close).
  * `mdd_grenade_aim 1` — keep the camera aimed at the predicted explosion point (within ~0.25°). This is done through `+left`/`+right`/`+lookup`/`+lookdown` with `cl_yawspeed`/`cl_pitchspeed` set per frame; your original values are restored afterwards.
  * `mdd_grenade_jump 1` — jump in the frame right before the grenade explodes.
  * `mdd_grenade_shoot 1` — fire in the frame right before the grenade explodes. With `mdd_grenade_jump 1` the jump happens one frame earlier so you jump, then shoot, then the grenade goes off.
  * `mdd_grenade_last 1` — when several grenades are live, target the one that explodes *last* instead of the one that explodes *next*.
  * `mdd_grenade_lead X` — perform the jump/shot `X` ms earlier than the predicted explosion (default `0`, negative is allowed). Your ping is already compensated for: the usercmd is timed so the *server* runs it just before the explosion.
  * `mdd_grenade_debug 1` — print to the console when a grenade starts being tracked and when the jump/shot is pressed, with the timing numbers.

Each of the first four also exists as a hold-to-enable button command, so you can leave the cvars at `0` and drive them from binds: `+mdd_grenade_chase`, `+mdd_grenade_aim`, `+mdd_grenade_jump`, `+mdd_grenade_shoot`. A feature is active while its cvar is `1` *or* its key is held. Multiple can share a key, e.g. `bind shift "+mdd_grenade_jump;+mdd_grenade_shoot"`.

The explosion point is predicted by replaying the server's missile code (bounces, the 0.65 velocity loss and the "come to rest" check) against the world geometry; players and movers are not considered. The explosion time is `trTime + 50 + 2500` ms, i.e. exactly what the server uses.

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
