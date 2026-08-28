# mdd_player_trajectory

Predicted-ghost trajectory feature for `player_trajectory.c`: draws where the player's hitbox
will be over the next N simulated frames, as a series of ghost boxes/line, under three
independent, combinable models (passive / strafebot / accel-percent). Re-simulates PM_SlideMove,
PM_StepSlideMove, PM_GroundTrace and friction/acceleration locally each time the seed player state
actually changes, tracing against world geometry, solid (`SOLID_BMODEL`) entities (doors, plats,
trains, buttons) currently in the player's own snapshot, and jump pads/teleporters.

## CVars

### `mdd_player_trajectory_strafe_modes` (bitmask, default `0b010`)

Which simulated model(s) to draw, combinable - `0` means nothing selected (off).

```
0bXXX
  |||
  ||+- 1: passive - "if I released every key right now and just kept jumping"
  |+-- 2: accel - uses your WASD and relative position within accel zone to predict future trajectory
  +--- 4: strafebot - what trajectory a strafebot would have if it kept strafing with your current WASD
```

Passive is the trajectory for no input except jumps, gravity/collision only).
Strafebot is the unachievable for humans, but achieveable theoretically. Optimistic/motivation.
Accel-percent sits between the two - it measures where your current aim sits between the green and 
yellow edges of the cgaz hud accel zone as a percentage and assumes you aim at the same relative position.
In other words, it is trying to predict where your jumps will land with your current input.

### `mdd_player_trajectory_display_type` (bitmask, default `0b11110`)

How each drawn sample is rendered, combinable.

```
0bXXXXX
  |||||
  ||||+- 1: full bounding boxes
  |||+-- 2: bottom squares only (a flat "footprint" square)
  ||+--- 4: jump/teleport-enter frames always drawn as full bboxes, regardless of the two bits
  |               above - so they stay visible even in bottoms-only mode
  |+---- 8: connecting line through every simulated frame (ignores mdd_player_trajectory_step)
  +----- 16: static (world-pinned) bboxes/bottoms instead of flowing
```

Bit 16 (static) only changes behavior while the live player is airborne - grounded, sampling
always falls back to flowing regardless of this bit, since the world-pinned phase only makes
sense while a jump/landing is actually imminent or in progress.

### `mdd_player_trajectory_frames` (count, default `270`, capped at `2000`)

How many simulated frames ahead to predict.

### `mdd_player_trajectory_step` (count, default `10`)

Draw a sample every X simulated frames. Jump and teleport-enter frames are always drawn regardless
of this spacing (see display_type bit 4 above for how they're shaped). The static display-type bit
changes WHICH indices count as "on step", not this spacing itself.

### `mdd_player_trajectory_trace_entities` (0 or 1, default `1`)

Also trace against nearby door/plat/train/button-type entities (anything `SOLID_BMODEL`), not just
static world geometry. Costs more on entity-dense maps - see Performance below. Disable per-map if
needed; world-only tracing is always active regardless of this cvar.

### `mdd_player_trajectory_trace_triggers` (0 or 1, default `1`)

React to jump pads/velocity pads (apply their real push) and teleporters (highlight the touched
frame in `mdd_player_trajectory_rgba_trigger`, stop predicting past it - the destination isn't
visible to cgame proxymod, see Known Limitations below).

### `mdd_player_trajectory_shader_line` (string, default `"railCore"`)

Shader name for the connecting line (display_type bit 8). Re-registered every render frame, so
changes take effect immediately. Point this at a custom shader to change the line's look - see the
"Custom shaders" section below for what that actually requires.

### `mdd_player_trajectory_shader_box` (string, default `"bbox_nocull"`)

Shader name for every drawn box/bottom square. Same re-registration/custom-shader notes as
`_shader_line` above.

### RGBA cvars (`R G B A`, each a float 0-1)

- `mdd_player_trajectory_rgba_passive` (default `.5 .5 .5 .5`) - passive model's samples
- `mdd_player_trajectory_rgba_strafebot` (default `0 .5 0 .5`) - strafebot model's samples
- `mdd_player_trajectory_rgba_accel` (default `.4 .4 0 .4`) - accel-percent model's samples
- `mdd_player_trajectory_rgba_jump` (default `.8 0 0 .8`) - jump frames (any model), and the
  live player's own just-jumped highlight
- `mdd_player_trajectory_rgba_trigger` (default `0 .5 1 .5`) - the teleport-touch frame

### `mdd_player_trajectory_debug` (0-5, default `0`, verbosity levels, not a bitmask)

See "Debug modes" below.

## Debug modes

If you are going to only use this player trajectory feature as a player, you don't need this CVar.
If on the other hand you want to extend or fix this feature, you may need this debug CVar.

`mdd_player_trajectory_debug` selects one mutually-exclusive verbosity level. All are throttled to
at most once every 250ms of real time except mode 3 (fires once per real jump, not on a timer).
Capture with the engine's own `\logfile 2` before setting the cvar, then read the resulting file -
that's how every debug log referenced in this project's history was produced.

| Value | Name | What it prints | When to reach for it |
|---|---|---|---|
| 0 | OFF | nothing | default |
| 1 | FULL | every simulated frame of the current dump, origin/velocity/state | general "what is the ghost actually doing" dump |
| 2 | NEXTJUMP | one line: just the next predicted jump frame's origin | checking the sim's own numbers against where the ghost visually appears on screen (view-angle-dependent beam rendering can make a correct position look wrong) |
| 3 | CHAIN | every intermediate value in the strafebot phi/quantization chain, for one full run, triggered once per real jump | comparing the ghost's per-frame steering math directly against real engine behavior. |
| 4 | LIVE | the first 10 simulated frames' full chain, every render frame regardless of jumps | catching a seed that jumps discontinuously between one render frame's simulation and the next (a live-state problem, not a within-chain one - mode 3 can't show this) |
| 5 | ENTITIES | every gathered `SOLID_BMODEL` entity and trigger (number, modelindex), once per real gather | seeing exactly what's in play on a given map without inferring it from trace counts alone |

Any nonzero mode also enables a `traj-timing` line (worldTraces/entityTraces/allSolid counts, real
milliseconds via `trap_Milliseconds()`) at the same throttled cadence - useful for a rough
per-map performance read without needing a dedicated mode.

## Custom shaders

`mdd_player_trajectory_shader_line`/`_box` take a **shader name**, not an image path -
`trap_R_RegisterShader` looks that name up across every `.shader` script loaded from any pk3, so a
custom look needs both a new image and a `.shader` text block naming it, not just a renamed image
file. Skipping the `.shader` block falls back to a plain opaque diffuse shader - no translucency, no
`cull disable` - which will look visibly wrong for a ghost overlay. Base a new block on the
existing `bbox_nocull`/`railCore` definitions (same `cull`/`blendFunc` keywords), give it a new
name, point its `map` stage at the new image, then set that name as the cvar value. Package the new
`.shader` file and image with the same folder structure (`scripts/`, and wherever the image lives)
directly at the root of the pk3 - an extra wrapping folder is the most common reason a
hand-built pk3 silently fails to register anything from it. The engine's `\shaderlist` console
command dumps every shader it actually parsed - the fastest way to confirm a new shader loaded at
all before troubleshooting anything about how it looks.

## Known limitations

- **Predicting more frames or more strafe modes can negatively impact FPS proportionally**.
- **Entity tracing cost varies a lot by map and by PVS entity count** - see Performance below.
  `mdd_player_trajectory_trace_entities 0` is the escape hatch for maps where it isn't worth it.
- **Moving/rotating entities are not correctly tracked yet** - a gathered entity's position is its
  last motion-segment anchor, not its live interpolated position. A closed door correctly blocks
  the ghost like a wall, but the trajectory won't anticipate it opening as the player approaches,
  matching how a real door only opens once triggered.
- **A ghost wedged in solid geometry truncates its prediction** rather than freezing silently or
  simulating forever - an intentional tradeoff, not full correctness.
- **A ghost stuck repeatedly re-jumping in place against a wallbug (no origin change between
  jumps) can cost noticeably more per recompute** than normal - observed on `0ups-beta2a` next to a
  dead loop. Not something to fix (the sim is correctly reproducing a real, if unwanted, in-game
  wallbug state) - performance returns to normal once the wallbug is escaped, same as it would for
  the real player.
- **Teleporters never continue past the entrance, by design, not as a temporary gap.** A
  teleporter's destination is resolved entirely server-side (`G_PickTarget` matching against a
  `target_position` entity by name) and is never sent to the client in any form - confirmed against
  real Quake 3 reference source, not assumed. The trigger volume itself is networked (so the client
  can show a "hyperspace" screen flash at the right moment), but where it leads to isn't, and never
  will be without a server-side change this project doesn't control. The trajectory highlights the
  touched frame in `mdd_player_trajectory_rgba_trigger` and stops predicting there.
- **No water physics** - the simulation stops early on detecting water rather than running
  ground/air physics that don't apply there.
- **Other players are never traced against** - always an encoded bbox (`SOLID_BBOX`), never
  `SOLID_BMODEL`, so they fall outside entity tracing's scope entirely. Deliberate, not a gap: a
  future position of another player isn't predictable anyway.

## Performance

Entity-aware tracing (world geometry plus `SOLID_BMODEL` movers currently in the player's
snapshot) adds real, measurable cost on top of the pre-existing world-only simulation. The numbers
below are real measurements (`trap_Milliseconds()`, confirmed 1ms-resolution via the engine's own
`timeBeginPeriod(1)` at startup), not estimates - captured via the `traj-timing` debug line
(`mdd_player_trajectory_debug 2` recommended specifically to avoid contaminating the measurement
with mode 1's own per-simulated-frame console-print overhead, which was large enough to distort
earlier measurements before this was understood; mode 5 shows which entities are actually
gathered).

### Reference measurement (2026-08-11)

Map: `dfwc2019-3` ("castle map", entity-dense - doors, plats, decorative brush clutter).
Demo: `dfwc2019-3df.cpm00.31.472Bazz.Netherlands_8731.dm_68`.
Log: `castle_demo_test4_3modes_500frames_debug2.log`.
Settings: 3 active models (worst-case stress test - most players run one model at a time),
`mdd_player_trajectory_frames 500`, `mdd_player_trajectory_trace_entities 1`.

129 samples across the demo: **avg 10.4ms/recompute** (range 6-34ms), averaging **6.6 gathered
entities** (range 5-10, never 0 on this map/path - PVS keeps most of this map's entities visible
throughout). Cost scales roughly linearly with both `mdd_player_trajectory_frames` and the number
of active models (confirmed separately this session) - this reference case (3 models, 500 frames)
is a deliberate stress test, not what most players will run; 1 model at 180-270 frames, sometimes
with entity tracing off entirely, costs correspondingly less.

### Derived per-call cost (order-of-magnitude estimate, not a direct profiler measurement)

Splitting the same 129 samples into a lower-entity-count group (<=5 gathered, n=25, avg 6,941
world traces / 34,705 entity traces / 9.6ms) and a higher one (>=9 gathered, n=9, avg 6,167 world
traces / 56,822 entity traces / 11.7ms) and solving as a simple two-term cost model
(`ms ~= worldTraces * costWorld + entityTraces * costEntity`):

- world trace: **~0.78 microseconds/call**
- entity trace: **~0.12 microseconds/call**

A single entity trace is markedly cheaper than a single world trace - consistent with a world
trace walking the entire map's BSP tree versus an entity trace only walking that one entity's own
small, dedicated inline collision model. Treat the exact ratio as noisy (small groups, and the two
groups aren't perfectly matched for underlying geometry complexity either) - the *direction* (entity
traces are cheaper per call) is the reliable part.

**This does not mean entities are cheap overall.** Every world-trace call site also issues one
entity trace per currently-gathered entity (no culling), so N gathered entities means N times more
entity-trace calls than world-trace calls - the cheaper per-call cost is real but gets outweighed
by the multiplier once several entities are in view, which is why this reference case's total cost
is still dominated by entity tracing despite each individual entity trace being the cheaper
operation.

### Why entity tracing relies solely on PVS, not a distance cull

There is no engine syscall in this codebase's contract for querying an entity's real distance or
bounds (nothing like `trap_CM_ModelBounds` - only a renderer-side model-bounds call exists, a
different subsystem, wrong handle type). Adding one would mean a new syscall number shared between
this repo and the engine repo, an engine-side implementation, and an engine rebuild - real
cross-repo work for a distance cull that isn't guaranteed to even help: correctly measuring
distance to a complex entity's actual edges would need its own tracing/bounds queries before the
real collision traces ever run, which could plausibly cost more than it saves. Simpler and already
correct: the client's own snapshot is already filtered server-side by PVS (a real, per-client,
bandwidth-driven bound, not something this feature built) - relying on that alone, plus the plain
`mdd_player_trajectory_trace_entities` on/off for maps where even PVS-bounded entity tracing costs
too much, is a good-enough, no-extra-engineering answer. Confirmed on two entity-dense maps:
`pro-q3tourney4` (6 doors, 12 moving parts, all visible from one spot) goes from ~50fps to a solid
125fps with the switch off, and on map `nrjt` - the overly-complicated moving entities (door) that
also noticeably impacts the performance.
