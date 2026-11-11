# Single source for the fork's help: every cvar, button and console command added on top of upstream.
# tools/gen_help.py turns this into help/<name>.txt (DeFRaG help format) and docs/reference.html.
#
# Entry fields
#   name      cvar / command name without the leading +
#   kind      "cvar" | "button" | "cvar+button" | "command"
#   group     one of GROUPS
#   usage     list of syntax lines
#   default   default value (cvars)
#   summary   one line
#   sections  list of (title, text); text paragraphs separated by a blank line
#   examples  list of (console text, what it does)
#   related   list of names
#   file      help file this entry is documented in when it shares one (still gets its own stub file)

GROUPS = [
  ("grenade", "Grenade tools"),
  ("rocket", "Jump and rocket tools"),
  ("rjob", "Rocket jump overbounce"),
  ("overbounce", "Overbounce and jumping"),
  ("replay", "Input replay"),
  ("input", "Input timing"),
  ("hud", "HUD"),
  ("trajectory", "Player trajectory"),
]

BUTTON_NOTE = (
  "This is a hold button like +forward or +attack: bind it to a key and it is active while the key is held. Several "
  "can share one key, e.g. bind shift \"+mdd_grenade_chase;+mdd_grenade_aim\".\n\n"
  "Everything the proxymod does goes through the engine's own buttons (+forward, +moveup, +attack, +left, ...), so "
  "the server sees ordinary input and your physical keys keep working. Automated jump and attack presses are held a "
  "random mdd_input_stagger_min..max ms, a jump + attack pair is never sent in the same usercmd, and no timing "
  "pattern repeats within 256 pairs."
)

GRENADE_NOTE = (
  "All grenade tools act on YOUR OWN grenades, tracked from the moment they leave the launcher. The explosion time "
  "is the launch time + 50 + 2500 ms, exactly what the server uses. The explosion point is predicted by replaying "
  "the server's missile code (bounces, the 0.65 velocity loss, the come-to-rest check) against the world.\n\n"
  "Each feature is active while its cvar is 1 OR its +mdd_grenade_<feature> button is held. With several grenades "
  "out, mdd_grenade_last picks which one is targeted."
)

REPLAY_NOTE = (
  "Replays record your usercmds (movement keys, buttons, weapon, view angles) and play them back through the "
  "engine's own buttons. Files are plain text in defrag/replays/<name>.mdd.\n\n"
  "For exact playback: com_maxfps 125 when recording and playing, timescale 1, pmove_fixed 1 with "
  "g_synchronousClients 0 (a local server with these settings is the ideal case; g_synchronousClients 1 samples your "
  "commands at the server's pace and is NOT reproducible). Hands off mouse and keys while it plays and don't hold "
  "other +mdd_ buttons. Your cl_yawspeed / cl_pitchspeed / cl_anglespeedkey are restored when it stops.\n\n"
  "View angles are reproduced exactly by 1 ms pulses of the keyboard-look buttons. With pmove_fixed, inputs are "
  "matched per 8 ms slot the way the server rounds them. On a remote server the first ~ping ms after a teleport are "
  "already sent before the proxymod can see the teleport, so those are your own input."
)

RJOB_NOTE = "Part of +mdd_rocketjump_overbounce; see that entry for the whole feature."
TRAJ_NOTE = (
  "Part of the predicted player trajectory by akuzmichov (merged from github.com/akuzmichov/cgame-proxymod, branch "
  "player_trajectory). The full reference is docs/mdd_player_trajectory.md in the repository."
)

ENTRIES = []


def E(**kw):
  ENTRIES.append(kw)


# --------------------------------------------------------------------------------------------- grenade
E(name="mdd_grenade_chase", kind="cvar+button", group="grenade", default="0",
  usage=["mdd_grenade_chase <0|1>", "bind <key> +mdd_grenade_chase"],
  summary="Walks you onto the spot where your grenade will explode.",
  sections=[
    ("Information",
     "When active after you fire a grenade, the proxymod walks or runs you onto the spot where the grenade will "
     "explode, using movement keys only (forward / back / moveleft / moveright, walking when close). It predicts how "
     "far you still slide after the keys are released, so it stops within about 2 units of the spot. Your view is not "
     "touched; the keys are chosen relative to wherever you are looking.\n\n"
     "Chase stops once mdd_grenade_jump has jumped, so it doesn't fight you in the air. If the explosion point is next "
     "to a wall you may end up pushing into the wall; that is harmless."),
    ("Notes", GRENADE_NOTE),
  ],
  examples=[
    ("bind mouse4 +mdd_grenade_chase", "Hold mouse4 after firing a grenade to be walked under it."),
    ("mdd_grenade_chase 1", "Always chase every grenade you fire."),
    ("bind shift \"+mdd_grenade_chase;+mdd_grenade_aim;+mdd_grenade_jump;+mdd_grenade_shoot\"",
     "One key for the whole grenade jump: walk to it, look at it, jump and fire your weapon right before it blows."),
  ],
  related=["mdd_grenade_aim", "mdd_grenade_jump", "mdd_grenade_shoot", "mdd_grenade_last"])

E(name="mdd_grenade_aim", kind="cvar+button", group="grenade", default="0",
  usage=["mdd_grenade_aim <0|1>", "bind <key> +mdd_grenade_aim"],
  summary="Keeps your view on the predicted explosion point of your grenade.",
  sections=[
    ("Information",
     "When active after you fire a grenade, the proxymod keeps your view aimed at the predicted explosion point "
     "(within about 0.25 degrees). It uses +left / +right / +lookup / +lookdown with cl_yawspeed and cl_pitchspeed set "
     "per frame; your original values are restored when it stops. Mouse input still adds on top and is corrected the "
     "next frame."),
    ("Notes", GRENADE_NOTE),
  ],
  examples=[
    ("bind mouse5 +mdd_grenade_aim", "Hold mouse5 to look at where your grenade is going to explode."),
    ("bind shift \"+mdd_grenade_aim;+mdd_grenade_shoot\"",
     "Look at the explosion point and fire a rocket into it at the last moment."),
  ],
  related=["mdd_grenade_chase", "mdd_grenade_jump", "mdd_grenade_shoot"])

E(name="mdd_grenade_jump", kind="cvar+button", group="grenade", default="0",
  usage=["mdd_grenade_jump <0|1>", "bind <key> +mdd_grenade_jump"],
  summary="Jumps in the last usercmd before your grenade explodes.",
  sections=[
    ("Information",
     "When active, jumps right before your grenade explodes. With mdd_grenade_shoot also active the order is jump, "
     "shot, explosion."),
    ("Timing",
     "The usercmd is timed so the SERVER runs it right before the explosion: your ping is subtracted automatically, "
     "mdd_grenade_lead (default 8 = one server frame) is subtracted on top, and mdd_grenade_jump_lead (default 24) "
     "moves only the jump earlier still. A late jump is lost (the blast has already lifted you), a late shot still "
     "fires, which is why the jump gets the extra margin. You can't jump while holding your own jump key."),
    ("Notes", GRENADE_NOTE),
  ],
  examples=[
    ("bind space +mdd_grenade_jump", "Hold space after firing a grenade at your feet: you jump just before it blows."),
    ("bind shift \"+mdd_grenade_jump;+mdd_grenade_shoot\"", "Grenade + rocket jump on one key."),
    ("mdd_grenade_jump_lead 32", "Jumps getting lost on a laggy server: jump a little earlier."),
  ],
  related=["mdd_grenade_shoot", "mdd_grenade_lead", "mdd_grenade_jump_lead", "mdd_grenade_debug"])

E(name="mdd_grenade_shoot", kind="cvar+button", group="grenade", default="0",
  usage=["mdd_grenade_shoot <0|1>", "bind <key> +mdd_grenade_shoot"],
  summary="Fires your current weapon in the last usercmd before your grenade explodes.",
  sections=[
    ("Information",
     "When active, fires whatever weapon you are holding right before your grenade explodes. With mdd_grenade_jump "
     "also active you jump first. Typical use: rocket + grenade jump."),
    ("Timing",
     "Ping is compensated automatically; mdd_grenade_lead (default 8 ms) shifts it earlier. If the weapon is still "
     "reloading at the fire time, +attack is held down until it fires (the earliest shot possible); otherwise it is a "
     "single tap."),
    ("Notes", GRENADE_NOTE),
  ],
  examples=[
    ("bind mouse2 +mdd_grenade_shoot",
     "Fire a grenade, switch to the rocket launcher, aim at your feet and hold mouse2: the rocket goes out as the "
     "grenade explodes."),
    ("bind shift \"+mdd_grenade_jump;+mdd_grenade_shoot\"", "Jump and shot together on one key."),
    ("mdd_grenade_lead 16", "Shot arriving too late: fire one server frame earlier."),
  ],
  related=["mdd_grenade_jump", "mdd_grenade_lead", "mdd_grenade_sync", "mdd_grenade_debug"])

E(name="mdd_grenade_sync", kind="cvar+button", group="grenade", default="0",
  usage=["mdd_grenade_sync <0|1>", "bind <key> +mdd_grenade_sync"],
  summary="One jump + shot timed to your grenade hitting a grenade button.",
  sections=[
    ("Information",
     "Jump + shot timed to a grenade button. A grenade button is a func_door with health: a grenade that touches it "
     "explodes on contact instead of bouncing. Typical use: prefire a grenade at the button, then tap the key (before "
     "or after the prefire). You jump just before the first grenade hits the button and your second shot is fired so "
     "that it lands at the same moment.\n\n"
     "It is mdd_grenade_jump + mdd_grenade_shoot, except that the explosion time is the predicted contact with the "
     "button rather than the 2.5 s fuse, and the shot is timed by its own predicted flight instead of a fixed lead."),
    ("Timing",
     "Jump: mdd_grenade_lead + mdd_grenade_jump_lead ms before the contact, ping compensated.\n\n"
     "Shot: fired so that it lands mdd_grenade_sync_offset ms (default 0) after the first grenade touches the button. "
     "Any weapon works: a second grenade must reach the same button (it only explodes on contact there); a rocket, "
     "plasma or bfg shot is timed by its flight to whatever it hits first; hitscan weapons fire right at the moment. "
     "If a grenade fired now would not reach the button nothing is fired and it keeps re-checking. If the weapon is "
     "still reloading at the fire time, +attack is held until it fires."),
    ("Notes",
     "The key is a latch, not a hold: tapping it arms it, it acts once on the next grenade of yours that will touch a "
     "mover, then disarms. The grenade fired by the sync shot is never itself a target. mdd_grenade_sync 1 re-arms by "
     "itself once none of your grenades are in the air.\n\n"
     "The proxymod cannot see which movers take damage, so the first mover your grenade touches is taken to be the "
     "button. If the grenade bounces off it instead, the prediction is redone from the new trajectory. "
     "mdd_grenade_debug 1 prints \"ent N touches mover M\". mdd_grenade_chase and mdd_grenade_aim target the button "
     "contact point while sync applies."),
  ],
  examples=[
    ("bind mouse4 +mdd_grenade_sync",
     "Fire a grenade at the grenade button, tap mouse4, keep looking at the button: one jump and one more grenade, "
     "both blasts together."),
    ("mdd_grenade_sync 1", "No key: every grenade you throw at a button gets the sync."),
    ("mdd_grenade_sync_offset 8", "Second blast one server frame after the first."),
  ],
  related=["mdd_grenade_sync_offset", "mdd_grenade_jump", "mdd_grenade_shoot", "mdd_grenade_debug"])

E(name="mdd_grenade_sync_offset", kind="cvar", group="grenade", default="0",
  usage=["mdd_grenade_sync_offset <ms>"],
  summary="How long after the first grenade touches the button the sync shot should land.",
  sections=[("Information",
             "mdd_grenade_sync: how many ms after the first grenade touches the button the second shot should land. "
             "Default 0 (same moment). Negative = second shot first.")],
  examples=[("mdd_grenade_sync_offset 0", "Both at the same moment (default)."),
            ("mdd_grenade_sync_offset 8", "Second shot one server frame later."),
            ("mdd_grenade_sync_offset -8", "Second shot one server frame earlier.")],
  related=["mdd_grenade_sync"])

E(name="mdd_grenade_last", kind="cvar", group="grenade", default="0",
  usage=["mdd_grenade_last <0|1>"],
  summary="With several grenades out: target the one that explodes next (0) or last (1).",
  sections=[("Information",
             "With several of your grenades alive: 0 (default) targets the grenade that explodes NEXT, 1 targets the "
             "one that explodes LAST. Affects chase, aim, jump and shoot.")],
  examples=[("mdd_grenade_last 1", "Fire three grenades, act on the last one."),
            ("bind f \"toggle mdd_grenade_last\"", "Flip between next and last with a key.")],
  related=["mdd_grenade_chase", "mdd_grenade_aim", "mdd_grenade_jump", "mdd_grenade_shoot"])

E(name="mdd_grenade_lead", kind="cvar", group="grenade", default="8",
  usage=["mdd_grenade_lead <ms>"],
  summary="How many ms before the explosion the grenade jump / shot is performed.",
  sections=[("Information",
             "Perform the grenade jump / shot this many ms earlier than the predicted explosion (default 8 = one "
             "server frame at sv_fps 125; negative is allowed). Your ping is already compensated separately, this is "
             "the fine adjustment.")],
  examples=[("mdd_grenade_lead 8", "Default: one server frame before the blast."),
            ("mdd_grenade_lead 16", "Shots arriving after the explosion: go one frame earlier."),
            ("mdd_grenade_debug 1", "Print the timing of every jump and shot while you tune it.")],
  related=["mdd_grenade_jump_lead", "mdd_grenade_jump", "mdd_grenade_shoot", "mdd_grenade_debug"])

E(name="mdd_grenade_jump_lead", kind="cvar", group="grenade", default="24",
  usage=["mdd_grenade_jump_lead <ms>"],
  summary="Extra lead for the grenade jump only.",
  sections=[("Information",
             "Extra lead for the grenade JUMP only (default 24 ms). A usercmd that reaches the server after the "
             "explosion still fires a shot, but a jump is lost because the blast has already lifted you, so the jump "
             "is biased early while the shot stays tight. Raise it if jumps get lost on a laggy server.")],
  examples=[("mdd_grenade_jump_lead 24", "Default."),
            ("mdd_grenade_jump_lead 40", "Unstable ping: give the jump more margin.")],
  related=["mdd_grenade_lead", "mdd_grenade_jump"])

E(name="mdd_grenade_debug", kind="cvar", group="grenade", default="0",
  usage=["mdd_grenade_debug <0|1|2>"],
  summary="Console output for the grenade tools.",
  sections=[("Information",
             "1: prints when a grenade starts being tracked, which mover it will touch, and when the jump / shot is "
             "pressed, with cmd time, explosion time, ping, lead and frame step. Use it to tune mdd_grenade_lead.\n\n"
             "2: also prints why a sync shot is being held back.")],
  examples=[("mdd_grenade_debug 1", "Watch the timing lines while tuning the leads."),
            ("mdd_grenade_debug 2; condump grenade", "Capture everything about a sync that didn't fire.")],
  related=["mdd_grenade_lead", "mdd_grenade_sync"])

# --------------------------------------------------------------------------------------------- rocket
E(name="mdd_jumpshoot", kind="button", group="rocket",
  usage=["bind <key> +mdd_jumpshoot"],
  summary="Jump and fire as soon as you are on the ground with the weapon ready; fires off jump pads too.",
  sections=[
    ("Information",
     "While held: as soon as you are on the ground and your weapon is ready, it jumps and fires - what "
     "bind x \"+moveup;+attack\" does, but it waits for the ground and the reload for you and spreads the key events "
     "like a human. Rocket jumps, plasma hops, P-overbounces. It re-arms every time you land again while the key "
     "stays held, so holding it chains plasma hops. If the jump doesn't take (e.g. you are holding your own jump key) "
     "it retries after 150 ms."),
    ("Timing",
     "The four key events go out as +moveup, then +attack a random mdd_input_stagger_min..max ms later, then -moveup, "
     "then -attack a random stagger after it. The holds are random too and no pattern repeats within the last 256 "
     "jump-shots. No two automated key events are ever in the same usercmd. mdd_jumpshoot_debug 1 prints the rolled "
     "gaps and the real usercmd times of all four events."),
    ("Jump pads",
     "When you are about to hit a pad that sends you up, it fires without a jump. It predicts when you enter the pad "
     "and when a rocket fired now would hit next to it, and fires so the rocket impact lands mdd_jumpshoot_pad_delay ms "
     "relative to the touch (negative = before it). Both times come from your real velocity against the real trigger "
     "brush, so the value carries over between pads and approach speeds. If no such shot is possible (not looking at "
     "the pad) it fires in the usercmd right after the launch. mdd_rocket_debug 1 prints the peak upward speed after "
     "every pad launch."),
    ("Notes", BUTTON_NOTE),
  ],
  examples=[
    ("bind mouse2 +mdd_jumpshoot", "Hold mouse2 with the rocket launcher, looking down: a rocket jump the moment you can."),
    ("bind mouse2 +mdd_jumpshoot", "Hold it with the plasma gun against a wall to chain plasma hops."),
    ("mdd_jumpshoot_pad_delay -8", "Falling onto a jump pad: have the rocket land one usercmd before you touch it."),
    ("mdd_jumpshoot_debug 1", "Check the randomized key timing of every jump-shot."),
  ],
  related=["mdd_jumpshoot_pad_delay", "mdd_jumpshoot_debug", "mdd_input_stagger_min", "mdd_rocketjump_overbounce"])

E(name="mdd_jumpshoot_pad_delay", kind="cvar", group="rocket", default="8",
  usage=["mdd_jumpshoot_pad_delay <ms>"],
  summary="Where the rocket impact should land relative to touching a jump pad.",
  sections=[("Information",
             "+mdd_jumpshoot on jump pads: the rocket impact is timed to land this many ms after the predicted touch "
             "of the pad's trigger; negative values put it before the touch. The touch time (your box against the "
             "trigger brush, with your real velocity and gravity) and the rocket's flight are both predicted every "
             "frame, so one value works across pads and approach speeds. Timing is in 8 ms usercmd steps."),
            ("Notes",
             "-8 has given the best height in testing. mdd_rocket_debug 1 prints \"peak upward speed after the pad "
             "launch\" so you can compare values.")],
  examples=[("mdd_jumpshoot_pad_delay -8", "Rocket lands one usercmd before the touch (best height so far)."),
            ("mdd_jumpshoot_pad_delay 8", "Default: one usercmd after the touch."),
            ("mdd_rocket_debug 1", "Print the peak upward speed after each pad launch to compare settings.")],
  related=["mdd_jumpshoot", "mdd_rocket_debug"])

E(name="mdd_jumpshoot_debug", kind="cvar", group="rocket", default="0",
  usage=["mdd_jumpshoot_debug <0|1>"],
  summary="Prints the real key timing of every ground jump-shot.",
  sections=[("Information",
             "+mdd_jumpshoot ground jump-shots: prints the gap that was rolled and, once both keys are back up, what "
             "was actually sent: the usercmd times of +moveup, +attack, -moveup, -attack, how long each key was held, "
             "the gap between the two presses and the gap between the two releases. Use it to confirm the stagger and "
             "randomization for a demo. Pad shots are reported by mdd_rocket_debug.")],
  examples=[("mdd_jumpshoot_debug 1", "Hold +mdd_jumpshoot for a while and read the [input] lines."),
            ("mdd_input_stagger_min 8; mdd_input_stagger_max 16; mdd_jumpshoot_debug 1",
             "Narrow the stagger and confirm the holds really stay within one or two usercmds.")],
  related=["mdd_jumpshoot", "mdd_input_debug", "mdd_input_stagger_min"])

E(name="mdd_rocketsync", kind="button", group="rocket",
  usage=["bind <key> +mdd_rocketsync"],
  summary="Jump + shot timed to your next rocket hitting the world.",
  sections=[
    ("Information",
     "While held: times a jump + shot so the shot is the last usercmd before YOUR NEXT ROCKET hits the world (floor "
     "or wall), with the jump before it. Same timing model as the grenade tools: ping compensated, "
     "mdd_rocketsync_lead (default 8) and mdd_rocketsync_jump_lead (default 24).\n\n"
     "Typical use: prefire rockets down a drop, land, hold this: you jump and fire right as they hit."),
    ("Notes", BUTTON_NOTE),
  ],
  examples=[
    ("bind mouse4 +mdd_rocketsync", "Prefire a rocket down a shaft, fall after it, hold mouse4 before landing."),
    ("bind shift \"+mdd_rocketstack;+mdd_rocketsync\"",
     "Stack a rocket on the way down and sync the jump + shot at the bottom with one key."),
    ("mdd_rocketsync_jump_lead 32", "Jump getting lost: give it more lead."),
  ],
  related=["mdd_rocketsync_lead", "mdd_rocketsync_jump_lead", "mdd_rocketstack", "mdd_rocket_debug"])

E(name="mdd_rocketsync_lead", kind="cvar", group="rocket", default="8",
  usage=["mdd_rocketsync_lead <ms>"],
  summary="How many ms before the rocket impact the sync shot is fired.",
  sections=[("Information",
             "Perform the +mdd_rocketsync shot this many ms before the rocket impact (default 8). Ping is compensated "
             "separately.")],
  examples=[("mdd_rocketsync_lead 8", "Default."), ("mdd_rocketsync_lead 16", "Shot arriving late: one frame earlier.")],
  related=["mdd_rocketsync", "mdd_rocketsync_jump_lead"])

E(name="mdd_rocketsync_jump_lead", kind="cvar", group="rocket", default="24",
  usage=["mdd_rocketsync_jump_lead <ms>"],
  summary="Extra lead for the rocket-sync jump only.",
  sections=[("Information",
             "Extra lead for the +mdd_rocketsync JUMP only (default 24), same reasoning as mdd_grenade_jump_lead: a "
             "late jump is lost, a late shot still fires.")],
  examples=[("mdd_rocketsync_jump_lead 24", "Default."), ("mdd_rocketsync_jump_lead 40", "More margin on a laggy server.")],
  related=["mdd_rocketsync", "mdd_rocketsync_lead"])

E(name="mdd_rocketstack", kind="button", group="rocket",
  usage=["bind <key> +mdd_rocketstack"],
  summary="Fires a rocket at the moment it would explode together with one already in flight.",
  sections=[
    ("Information",
     "While held, with the rocket launcher out and one of your rockets still in flight: every frame the proxymod "
     "predicts where and when a rocket fired RIGHT NOW would hit, and fires at the moment that impact time equals the "
     "existing rocket's impact time, so both explode together. On a fall that moment is when you pass the rocket. If "
     "you are slower than the rocket (900 ups) the moment never comes and nothing happens."),
    ("Tuning", "mdd_rocketstack_offset (ms, default 0) shifts the moment; mdd_rocket_debug 1 prints the numbers."),
    ("Notes", BUTTON_NOTE),
  ],
  examples=[
    ("bind mouse5 +mdd_rocketstack",
     "Prefire two rockets down a drop, fall past them with mouse5 held: a third is stacked onto the first one you reach."),
    ("bind shift \"+mdd_rocketstack;+mdd_rocketsync\"", "Stack on the way down, then jump + shoot as they land."),
  ],
  related=["mdd_rocketstack_offset", "mdd_rocketsync", "mdd_rocket_debug"])

E(name="mdd_rocketstack_offset", kind="cvar", group="rocket", default="0",
  usage=["mdd_rocketstack_offset <ms>"],
  summary="Shifts the rocket-stack firing moment.",
  sections=[("Information", "Shift the +mdd_rocketstack firing moment (default 0; positive fires later).")],
  examples=[("mdd_rocketstack_offset 0", "Default: impacts at the same time."),
            ("mdd_rocketstack_offset 8", "Stacked rocket one usercmd later.")],
  related=["mdd_rocketstack"])

E(name="mdd_rocket_debug", kind="cvar", group="rocket", default="0",
  usage=["mdd_rocket_debug <0|1|2>"],
  summary="Console output for jumpshoot, rocketsync and rocketstack.",
  sections=[("Information",
             "1: timing output for +mdd_jumpshoot, +mdd_rocketsync and +mdd_rocketstack: rocket tracking, impact "
             "times, when keys were pressed, and the peak upward speed after every jump-pad launch.\n\n"
             "2: also dumps the jump-pad entity that was touched.")],
  examples=[("mdd_rocket_debug 1", "Compare peak upward speed between mdd_jumpshoot_pad_delay values."),
            ("mdd_rocket_debug 1", "See when +mdd_rocketsync pressed jump and attack against the impact time.")],
  related=["mdd_jumpshoot", "mdd_rocketsync", "mdd_rocketstack", "mdd_jumpshoot_pad_delay"])

# --------------------------------------------------------------------------------------------- rjob
E(name="mdd_rocketjump_overbounce", kind="button", group="rjob",
  usage=["bind <key> +mdd_rocketjump_overbounce"],
  summary="One rocket jump at your current view, chosen so that you land on an overbounce.",
  sections=[
    ("Information",
     "One rocket jump per key press, aimed where you are looking, chosen so that you land on an overbounce. Be on the "
     "ground with the rocket launcher ready and press the key. Jump and shot go through the usual randomized input "
     "layer.\n\n"
     "You don't have to stand still: the jump can only go out a few frames after the key press, so the plan is made "
     "for where you will be by then (sliding to a stop or running on flat ground is carried forward pmove by pmove) "
     "and checked against where you really are in the frame the jump is sent. If that check fails it plans again, up "
     "to three times."),
    ("How it chooses",
     "At a fixed view the only free choice is how many pmove slots lie between the jump and the shot. Each one gives "
     "one knockback and therefore one landing, and only about one landing in twenty ends inside the 0.25 unit ground "
     "trace, so on its own an overbounce exists about a third of the time. The pitch may therefore also be nudged by "
     "up to mdd_rocketjump_overbounce_nudge degrees right before the shot; it is turned back afterwards.\n\n"
     "Every candidate is simulated against the real map the way the server runs it: jump, muzzle point, 50 ms "
     "prestep, rocket impact, splash knockback (whole points only), velocity snapped every pmove, free fall until the "
     "landing. Of all candidates that overbounce, the highest one is used, typically 10-15 units below the best plain "
     "rocket jump. If none overbounces, the best plain rocket jump at your exact view is done and a line says so.\n\n"
     "Aiming straight down (the engine stops the view at 87.89 degrees) is the hardest case: the view can only be "
     "tilted one way and the blast stays under your feet where the knockback barely changes. Use "
     "mdd_rocketjump_overbounce_nudge 4 there."),
    ("Debug",
     "With mdd_rocketjump_overbounce_debug 1 every use prints the chosen candidate and checks it against the server: "
     "how many pmoves after the jump the shot went out, which usercmd the blast followed, the blast point and "
     "knockback, the velocity after the blast (MATCH / MISMATCH) and the landing, ending in one RESULT line. The same "
     "plus full detail is appended to rjob.log in the mod folder."),
    ("Limits",
     "VQ3 physics, com_maxfps 125, pmove_fixed 1 / pmove_msec 8 (or g_synchronousClients 1). The jump is held back a "
     "frame or two until the shot can go out with a usercmd that ends exactly on the planned slot.\n\n"
     "The blast timing is certain when the rocket lands within 45 units of the muzzle (roughly pitch 65 and steeper "
     "with a short delay); those candidates are preferred. Shallower shots are marked \"timing depends on the frame "
     "phase\" and can miss more often.\n\n"
     "Don't move the mouse between the key press and the shot, and don't steer onto ground of a different height "
     "during the flight. Only normal overbounces are targeted, not sticky ones. Movers are not simulated. Online the "
     "server timing can shift by a usercmd, which misses."),
  ],
  examples=[
    ("bind mouse4 +mdd_rocketjump_overbounce", "Look at the floor in front of you with the rocket launcher, tap mouse4."),
    ("mdd_rocketjump_overbounce_nudge 4", "Aiming straight down: allow a bigger view correction."),
    ("mdd_rocketjump_overbounce_nudge 0", "Never touch the view: only the jump-to-shot delay is varied."),
    ("bind mouse4 \"+mdd_rocketjump_overbounce\"; bind space +mdd_overbounce_jump",
     "Land the overbounce, then hold space to jump out of it on the first possible usercmd."),
    ("mdd_rocketjump_overbounce_debug 1", "See the RESULT line and write rjob.log."),
  ],
  related=["mdd_rocketjump_overbounce_nudge", "mdd_rocketjump_overbounce_delay_max",
           "mdd_rocketjump_overbounce_explode_offset", "mdd_rocketjump_overbounce_debug", "mdd_overbounce_jump"])

E(name="mdd_rocketjump_overbounce_nudge", kind="cvar", group="rjob", default="2",
  usage=["mdd_rocketjump_overbounce_nudge <degrees>"],
  summary="Largest pitch change allowed right before the shot.",
  sections=[("Information",
             "+mdd_rocketjump_overbounce: the largest pitch change it may make right before the shot to reach an "
             "overbounce; the view is turned back afterwards. Default 2. 0 = never touch the view (an overbounce is "
             "then reachable only about a third of the time). Aiming straight down needs more: 4 is a good value "
             "there."),
            ("Notes", RJOB_NOTE)],
  examples=[("mdd_rocketjump_overbounce_nudge 2", "Default: an overbounce is found on most presses at normal angles."),
            ("mdd_rocketjump_overbounce_nudge 4", "For straight-down rocket jumps."),
            ("mdd_rocketjump_overbounce_nudge 0", "Exact view only.")],
  related=["mdd_rocketjump_overbounce", "mdd_rocketjump_overbounce_delay_max"])

E(name="mdd_rocketjump_overbounce_delay_max", kind="cvar", group="rjob", default="6",
  usage=["mdd_rocketjump_overbounce_delay_max <1..12>"],
  summary="Longest jump-to-shot delay tried, in pmove slots.",
  sections=[("Information",
             "+mdd_rocketjump_overbounce: the longest jump-to-shot delay tried, in pmove slots (8 ms each). Default 6. "
             "Longer delays give more candidates but lower jumps."),
            ("Notes", RJOB_NOTE)],
  examples=[("mdd_rocketjump_overbounce_delay_max 6", "Default."),
            ("mdd_rocketjump_overbounce_delay_max 12", "More candidates when nothing is reachable, at the cost of height.")],
  related=["mdd_rocketjump_overbounce", "mdd_rocketjump_overbounce_nudge"])

E(name="mdd_rocketjump_overbounce_explode_offset", kind="cvar", group="rjob", default="0",
  usage=["mdd_rocketjump_overbounce_explode_offset <usercmds>"],
  summary="Calibration: shifts which usercmd the blast is assumed to follow for far-landing rockets.",
  sections=[("Information",
             "+mdd_rocketjump_overbounce: shifts which of your usercmds the blast is assumed to follow when the rocket "
             "lands beyond its first step. Default 0. Only for calibration: leave it alone unless the debug output "
             "shows \"blast seen after cmd N (planned M)\" consistently one off in the same direction."),
            ("Notes", RJOB_NOTE)],
  examples=[("mdd_rocketjump_overbounce_explode_offset 0", "Default."),
            ("mdd_rocketjump_overbounce_explode_offset 1", "Debug shows the blast always one usercmd later than planned."),
            ("mdd_rocketjump_overbounce_explode_offset -1", "Debug shows it always one usercmd earlier.")],
  related=["mdd_rocketjump_overbounce", "mdd_rocketjump_overbounce_debug"])

E(name="mdd_rocketjump_overbounce_debug", kind="cvar", group="rjob", default="0",
  usage=["mdd_rocketjump_overbounce_debug <0|1>"],
  summary="Console checks, RESULT line and rjob.log for every use.",
  sections=[("Information",
             "+mdd_rocketjump_overbounce: 1 prints the chosen candidate, the checks against the server and a RESULT "
             "line for every use, and appends the full detail to rjob.log in the mod folder: the starting state and "
             "cvars, every usercmd sent (time, pmove slot, keys, view), the frames around the jump, and every server "
             "playerstate of the whole flight next to the predicted one."),
            ("Notes", RJOB_NOTE)],
  examples=[("mdd_rocketjump_overbounce_debug 1", "Turn it on, use the key a few times, read the RESULT lines."),
            ("mdd_rocketjump_overbounce_debug 0", "Off again: no console output, no log file.")],
  related=["mdd_rocketjump_overbounce", "mdd_rocketjump_overbounce_explode_offset"])

# --------------------------------------------------------------------------------------------- overbounce
E(name="mdd_overbounce_jump", kind="button", group="overbounce",
  usage=["bind <key> +mdd_overbounce_jump"],
  summary="Jumps on the first usercmd after an overbounce landing gave you its speed.",
  sections=[
    ("Information",
     "While held: if you land with an overbounce, jumps in the usercmd right after the one that converted your fall "
     "speed into overbounce speed. Jumping any earlier would cancel the overbounce, so this is the earliest possible "
     "overbounce jump."),
    ("How it decides",
     "An overbounce landing is detected from the predicted player state: grounded while the z velocity is still large "
     "negative and still decreasing (a normal or slope landing always raises it). The next time the velocity changes "
     "the overbounce has happened; if you are still on the floor it taps jump. On tall overbounces the leftover "
     "upward velocity briefly flags you as jumping away; it waits up to 150 ms for the ground flag to return. Only "
     "horizontal (go) overbounces get a jump; a vertical one launches you anyway. mdd_overbounce_debug 1 prints what "
     "it saw."),
    ("Notes", BUTTON_NOTE),
  ],
  examples=[
    ("bind space +mdd_overbounce_jump", "Walk off a ledge at an overbounce height holding space: you jump with the full speed."),
    ("bind space \"+mdd_overbounce_jump;+mdd_autojump\"", "Overbounce jump when there is one, a normal landing jump otherwise."),
    ("mdd_overbounce_debug 1", "See whether a landing was recognised as an overbounce."),
  ],
  related=["mdd_overbounce_zero", "mdd_overbounce_debug", "mdd_autojump", "mdd_rocketjump_overbounce"])

E(name="mdd_overbounce_zero", kind="button", group="overbounce",
  usage=["bind <key> +mdd_overbounce_zero"],
  summary="Brings your horizontal velocity to exactly 0 in the air, for a vertical overbounce.",
  sections=[
    ("Information",
     "While held in the air: uses the movement keys to bring your horizontal velocity to EXACTLY 0 so the landing is "
     "a vertical overbounce. A vertical overbounce needs horizontal speed below 1 at the overbounce frame. Velocities "
     "are snapped to integers, so the last step is done walking (1.29 ups per frame: 1 -> 0) instead of running "
     "(2.56: 1 -> -2 -> 1 forever). On CPM it also ducks for the last few units.\n\n"
     "Works best on VQ3. On CPM the side-strafe air acceleration can't settle near zero, so it only uses forward / "
     "back close in and may leave a sideways unit if you aren't looking roughly along your velocity."),
    ("Notes", BUTTON_NOTE),
  ],
  examples=[
    ("bind mouse5 +mdd_overbounce_zero", "Fall from an overbounce height holding mouse5 to bounce straight back up."),
    ("bind mouse5 \"+mdd_overbounce_zero\"", "After a straight-down +mdd_rocketjump_overbounce, hold it during the fall to keep the bounce vertical."),
  ],
  related=["mdd_overbounce_jump", "mdd_overbounce_debug"])

E(name="mdd_overbounce_debug", kind="cvar", group="overbounce", default="0",
  usage=["mdd_overbounce_debug <0|1>"],
  summary="Console output for the overbounce buttons.",
  sections=[("Information",
             "Console output for +mdd_overbounce_jump: pending overbounce, overbounce speed, when the jump was pressed "
             "or why not.")],
  examples=[("mdd_overbounce_debug 1", "Check why an overbounce jump didn't happen.")],
  related=["mdd_overbounce_jump", "mdd_overbounce_zero"])

E(name="mdd_autojump", kind="button", group="overbounce",
  usage=["bind <key> +mdd_autojump"],
  summary="Presses jump every time you land, in the landing usercmd itself.",
  sections=[
    ("Information",
     "While held, presses jump every time you are on the ground or about to land, so the jump goes out in the landing "
     "usercmd itself (what a frame-perfect manual jump does). Works in VQ3, where holding your own jump key does "
     "nothing, and in CPM."),
    ("Timing",
     "The press is sent as soon as you are grounded, or when the next usercmd is predicted to land. Each press is "
     "held for a random mdd_autojump_stagger_min..mdd_autojump_stagger_max ms (default 8..64), never shorter than "
     "needed to still be down when the landing usercmd runs. One jump per landing; being airborne re-arms it. If a "
     "jump didn't take (right after spawning the engine refuses jumps until attack is released) it tries again every "
     "100 ms.\n\n"
     "If you are hovering a fraction above walkable ground without being grounded (sticky overbounce) it keeps "
     "pressing jump until one lands. With the Flight powerup, as a free spectator or in noclip it simply holds jump "
     "down for as long as the key and that state last."),
    ("Notes",
     "mdd_autojump_debug 1 prints every press and, once released, the exact usercmd times. Movers (func_plat etc.) "
     "are not part of the landing prediction: on those the jump goes out one usercmd after landing.\n\n" + BUTTON_NOTE),
  ],
  examples=[
    ("bind space +mdd_autojump", "Hold space to bunny-hop: every landing is jumped on its own usercmd."),
    ("bind space \"+mdd_overbounce_jump;+mdd_autojump\"", "Also catch overbounce landings."),
    ("mdd_autojump_stagger_min 16; mdd_autojump_stagger_max 48", "Keep the jump key down 2-6 usercmds per press."),
    ("bind space +mdd_autojump", "In spectator or noclip the same key climbs."),
  ],
  related=["mdd_autojump_stagger_min", "mdd_autojump_stagger_max", "mdd_autojump_debug", "mdd_overbounce_jump"])

E(name="mdd_autojump_stagger_min", kind="cvar", group="overbounce", default="8",
  usage=["mdd_autojump_stagger_min <ms>", "mdd_autojump_stagger_max <ms>"],
  summary="Shortest hold of an autojump press.",
  sections=[("Information",
             "+mdd_autojump: the hold time of every jump press is drawn at random from "
             "mdd_autojump_stagger_min..mdd_autojump_stagger_max ms. Defaults 8 and 64. The hold is extended if needed "
             "so the key is still down in the landing usercmd.")],
  examples=[("mdd_autojump_stagger_min 8; mdd_autojump_stagger_max 64", "Defaults: 1 to 8 usercmds."),
            ("mdd_autojump_stagger_min 24; mdd_autojump_stagger_max 24", "Always exactly three usercmds (no randomization)."),
            ("mdd_autojump_debug 1", "Print the rolled and the real hold of every press.")],
  related=["mdd_autojump_stagger_max", "mdd_autojump", "mdd_autojump_debug"])

E(name="mdd_autojump_stagger_max", kind="cvar", group="overbounce", default="64",
  usage=["mdd_autojump_stagger_min <ms>", "mdd_autojump_stagger_max <ms>"],
  summary="Longest hold of an autojump press.",
  sections=[("Information",
             "+mdd_autojump: the hold time of every jump press is drawn at random from "
             "mdd_autojump_stagger_min..mdd_autojump_stagger_max ms. Defaults 8 and 64. The hold is extended if needed "
             "so the key is still down in the landing usercmd.")],
  examples=[("mdd_autojump_stagger_max 64", "Default."),
            ("mdd_autojump_stagger_max 32", "Shorter taps: at most four usercmds.")],
  related=["mdd_autojump_stagger_min", "mdd_autojump", "mdd_autojump_debug"])

E(name="mdd_autojump_debug", kind="cvar", group="overbounce", default="0",
  usage=["mdd_autojump_debug <0|1>"],
  summary="Prints every autojump press and its real hold.",
  sections=[("Information",
             "+mdd_autojump: prints every jump press (grounded, hovering just above the ground, or predicted landing; "
             "fall speed; rolled hold) and, after the release, the usercmd times of +moveup / -moveup and the real "
             "hold length. Also prints retries and the spectator / noclip / flight hold.")],
  examples=[("mdd_autojump_debug 1", "Check the randomization, or why a jump was retried.")],
  related=["mdd_autojump"])

# --------------------------------------------------------------------------------------------- replay
E(name="mdd_replay_record", kind="command", group="replay",
  usage=["mdd_replay_record <name>"],
  summary="Records your input into a replay, starting from a saved position.",
  sections=[
    ("Information",
     "Starts recording your input into replay <name>. First it runs defrag's \"set saveposname <name>; savepos\" and "
     "stores the resulting start position in the replay, so mdd_replay_start can put you back there; your own "
     "saveposname is restored afterwards. savepos itself needs no cheats. Stand still when you start for an exact "
     "playback; stop with mdd_replay_stoprecord.\n\n"
     "Name: any printable characters except spaces and / \\ : * ? \" < > | ; (it is also used as a cvar name by "
     "savepos). Defrag's autorecord demo names work as-is."),
    ("Notes", REPLAY_NOTE),
  ],
  examples=[
    ("mdd_replay_record jump1", "Start recording from where you stand."),
    ("mdd_replay_stoprecord", "Stop and write defrag/replays/jump1.mdd."),
    ("bind F5 \"mdd_replay_record run\"; bind F6 mdd_replay_stoprecord; bind F7 \"mdd_replay_start run\"",
     "Record, stop and play back on three keys."),
  ],
  related=["mdd_replay_stoprecord", "mdd_replay_start", "mdd_replay_telerecord"])

E(name="mdd_replay_telerecord", kind="command", group="replay",
  usage=["mdd_replay_telerecord <name>"],
  summary="Records a replay starting with the first usercmd after the next teleporter.",
  sections=[
    ("Information",
     "Starts recording replay <name> with the first usercmd after you exit the NEXT teleporter. The teleporter's "
     "destination is stored in the replay so mdd_replay_telestart only triggers on that same exit. The commands that "
     "were already in flight when the teleport showed up are pulled from the client's command buffer, so nothing is "
     "lost. No cheats involved. Stop with mdd_replay_stoprecord."),
    ("Notes", REPLAY_NOTE),
  ],
  examples=[
    ("mdd_replay_telerecord room2", "Arm it, then walk into the teleporter: recording starts on the other side."),
    ("mdd_replay_stoprecord", "Stop and save."),
    ("mdd_replay_telestart room2", "Later: arm playback and go through the same teleporter."),
  ],
  related=["mdd_replay_telestart", "mdd_replay_stoprecord", "mdd_replay_record"])

E(name="mdd_replay_stoprecord", kind="command", group="replay",
  usage=["mdd_replay_stoprecord"],
  summary="Stops recording and writes the replay file.",
  sections=[("Information",
             "Stops the current recording and writes defrag/replays/<name>.mdd. Recording also stops by itself after "
             "10 minutes (buffer full).")],
  examples=[("mdd_replay_stoprecord", "Finish the replay started with mdd_replay_record or mdd_replay_telerecord."),
            ("bind F6 mdd_replay_stoprecord", "On a key.")],
  related=["mdd_replay_record", "mdd_replay_telerecord"])

E(name="mdd_replay_start", kind="command", group="replay",
  usage=["mdd_replay_start <name> [<name> ...]"],
  summary="Puts you at a replay's start position and plays its input.",
  sections=[
    ("Information",
     "Loads replay <name>, puts you at its stored start position (placeplayer, a server command that needs cheats) "
     "and starts feeding the recorded input as soon as the teleport shows up in a snapshot. Stops by itself at the end "
     "or with mdd_replay_stop. If cheats are off, placeplayer can't run, so the input is simply played from wherever "
     "you are standing, immediately. A replay recorded with telerecord started this way also just plays from here."),
    ("Chaining",
     "Several names can be given (up to 16): they play back to back, the next one's first usercmd following the "
     "previous one's last. Only the FIRST replay of a chain gets the start synchronisation; the rest just continue "
     "the input stream."),
    ("Notes", REPLAY_NOTE),
  ],
  examples=[
    ("mdd_replay_start jump1", "Go back to where jump1 was recorded and replay it (needs cheats for the position)."),
    ("mdd_replay_start part1 part2 part3", "Play three replays back to back."),
    ("devmap mymap; mdd_replay_start jump1", "Load the map with cheats so the start position can be restored."),
  ],
  related=["mdd_replay_record", "mdd_replay_stop", "mdd_replay_telestart", "mdd_replay_debug"])

E(name="mdd_replay_telestart", kind="command", group="replay",
  usage=["mdd_replay_telestart <name> [<name> ...]"],
  summary="Plays a replay starting when you exit the teleporter it was recorded from.",
  sections=[
    ("Information",
     "Loads replay <name> and plays it back starting with the first usercmd after you exit the SAME teleporter it was "
     "recorded from. Teleports landing elsewhere (other teleporters, respawns) are ignored and it stays armed. No "
     "cheats needed. Meant for replays made with mdd_replay_telerecord; a replay made with mdd_replay_record has no "
     "exit position stored and triggers on the next teleport of any kind. Stop with mdd_replay_stop."),
    ("Chaining",
     "Several names can be given (up to 16): they play back to back; only the first waits for the teleporter."),
    ("Notes", REPLAY_NOTE),
  ],
  examples=[
    ("mdd_replay_telestart room2", "Arm it and walk into the teleporter; the recorded input takes over on the other side."),
    ("mdd_replay_telestart room2 room3", "Chain a second replay straight after the first."),
    ("mdd_replay_stop", "Disarm it again."),
  ],
  related=["mdd_replay_telerecord", "mdd_replay_stop", "mdd_replay_start"])

E(name="mdd_replay_stop", kind="command", group="replay",
  usage=["mdd_replay_stop"],
  summary="Stops or disarms playback.",
  sections=[("Information",
             "Stops a running (or armed) playback, releases all keys and restores your cvars. Playback also stops by "
             "itself when the replay ends.")],
  examples=[("mdd_replay_stop", "Take control back."), ("bind F8 mdd_replay_stop", "Panic key.")],
  related=["mdd_replay_start", "mdd_replay_telestart"])

E(name="mdd_replay_debug", kind="cvar", group="replay", default="0",
  usage=["mdd_replay_debug <0|1>"],
  summary="Console output about replay sync points.",
  sections=[("Information",
             "Console output about replay sync points: when the teleport was seen, which serverTime recorded cmd 0 maps "
             "to, how many ms were skipped, grid vs time-based matching.")],
  examples=[("mdd_replay_debug 1", "Check why a playback started late or drifted.")],
  related=["mdd_replay_start", "mdd_replay_telestart"])

# --------------------------------------------------------------------------------------------- input
E(name="mdd_input_stagger_min", kind="cvar", group="input", default="8",
  usage=["mdd_input_stagger_min <ms>", "mdd_input_stagger_max <ms>"],
  summary="Lower end of the randomizer behind every automated jump and shot.",
  sections=[
    ("Information",
     "mdd_input_stagger_min..mdd_input_stagger_max is the one randomizer behind every automated jump and shot "
     "(+mdd_jumpshoot, +mdd_rocketsync, +mdd_rocketstack, mdd_grenade_jump / shoot / sync, +mdd_overbounce_jump, pad "
     "shots). Defaults 8 and 64.\n\n"
     "Values are whole usercmds: the range is rounded up to usercmd counts (8..64 = 1..8 usercmds at 125 fps) and "
     "every count is equally likely. Every automated key press is held for a random value from this range. When jump "
     "and attack are pressed as a pair (within 150 ms of each other), the second one is released a random value from "
     "this range AFTER the first one's release, and the press gap of +mdd_jumpshoot is drawn from it too. Two "
     "automated key events are never put in the same usercmd.\n\n"
     "The (press gap, first hold, release gap) pattern of every pair is compared with the last 256 patterns and "
     "re-rolled, so no two jump + shots in a demo look alike."),
    ("Notes", "Replaces the old mdd_input_time / mdd_input_time_randomize. mdd_input_debug 1 prints the real usercmd "
              "times of every pair."),
  ],
  examples=[("mdd_input_stagger_min 8; mdd_input_stagger_max 64", "Defaults."),
            ("mdd_input_stagger_min 8; mdd_input_stagger_max 16", "Tight: everything within one or two usercmds."),
            ("mdd_input_debug 1", "Print what was really sent.")],
  related=["mdd_input_stagger_max", "mdd_input_debug", "mdd_jumpshoot"])

E(name="mdd_input_stagger_max", kind="cvar", group="input", default="64",
  usage=["mdd_input_stagger_min <ms>", "mdd_input_stagger_max <ms>"],
  summary="Upper end of the randomizer behind every automated jump and shot.",
  sections=[("Information",
             "Upper end of the range every automated key hold, release stagger and jump-shot press gap is drawn from. "
             "See mdd_input_stagger_min for the full description. Default 64 (8 usercmds at 125 fps).")],
  examples=[("mdd_input_stagger_max 64", "Default."),
            ("mdd_input_stagger_max 32", "Faster key releases: at most four usercmds.")],
  related=["mdd_input_stagger_min", "mdd_input_debug"])

E(name="mdd_input_debug", kind="cvar", group="input", default="0",
  usage=["mdd_input_debug <0|1|2>"],
  summary="Prints the real usercmd times of automated key presses.",
  sections=[("Information",
             "1: after every automated jump + attack pair has been released, prints the usercmd times of the four "
             "events, how long each key was held, the press gap and the release gap (and the gap that was rolled), "
             "whatever feature sent them.\n\n"
             "2: also prints every single automated tap (pad shots, autojump, lone jumps) with its press and release "
             "times.")],
  examples=[("mdd_input_debug 1", "Verify the stagger of jump + shot pairs before recording a demo."),
            ("mdd_input_debug 2", "Include single taps.")],
  related=["mdd_input_stagger_min", "mdd_jumpshoot_debug"])

# --------------------------------------------------------------------------------------------- hud
E(name="mdd_yaw", kind="cvar", group="hud", default="",
  usage=["mdd_yaw <angle> [<angle> ...]"],
  summary="Marks yaw angles on screen with vertical lines, like mdd_pitch does for pitch.",
  sections=[("Information",
             "Marks one or more absolute yaw angles with a vertical line where that heading projects on screen, the "
             "yaw counterpart of mdd_pitch. Empty = off. Position and size come from mdd_yaw_ywh, the colour from "
             "mdd_yaw_rgba.")],
  examples=[("mdd_yaw 90 270", "Mark north and south."),
            ("mdd_yaw 0 90 180 270", "Mark all four axis directions, e.g. to find the quadrant for a rocket or plasma overbounce."),
            ("mdd_yaw \"\"", "Off.")],
  related=["mdd_yaw_ywh", "mdd_yaw_rgba"])

E(name="mdd_yaw_ywh", kind="cvar", group="hud", default="236 1 8",
  usage=["mdd_yaw_ywh <y> <width> <height>"],
  summary="Position and size of the mdd_yaw lines.",
  sections=[("Information",
             "Where the mdd_yaw lines are drawn: y of the top edge, line width and line height, in the 640x480 virtual "
             "screen. Default 236 1 8 (a short tick at the crosshair).")],
  examples=[("mdd_yaw_ywh 236 1 8", "Default tick at crosshair height."),
            ("mdd_yaw_ywh 0 1 480", "Full-height lines."),
            ("mdd_yaw_ywh 200 2 80", "Thicker, taller marks.")],
  related=["mdd_yaw", "mdd_yaw_rgba"])

E(name="mdd_yaw_rgba", kind="cvar", group="hud", default=".8 .8 .8 .8",
  usage=["mdd_yaw_rgba <r> <g> <b> <a>"],
  summary="Colour of the mdd_yaw lines.",
  sections=[("Information", "Colour and opacity of the mdd_yaw lines, each 0..1. Default .8 .8 .8 .8.")],
  examples=[("mdd_yaw_rgba 1 0 0 1", "Solid red."), ("mdd_yaw_rgba .8 .8 .8 .4", "Faint grey.")],
  related=["mdd_yaw", "mdd_yaw_ywh"])

# --------------------------------------------------------------------------------------------- trajectory
E(name="mdd_player_trajectory_strafe_modes", kind="cvar", group="trajectory", default="0b010",
  usage=["mdd_player_trajectory_strafe_modes <0bXXX>"],
  summary="Which predicted trajectories to draw (also the on/off switch).",
  sections=[("Information",
             "Bitmask of the simulated models to draw; 0 = off.\n\n"
             "1 (0b001) passive: no input except jumps, gravity and collision only.\n\n"
             "2 (0b010) accel: assumes you keep aiming at the same relative position inside the accel zone as now, "
             "i.e. where your jumps will land with your current input.\n\n"
             "4 (0b100) strafebot: ideal strafing with your current keys; the theoretical best."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_strafe_modes 0b010", "Default: your current-input prediction."),
            ("mdd_player_trajectory_strafe_modes 0b110", "Yours and the strafebot's, to compare."),
            ("mdd_player_trajectory_strafe_modes 0", "Off.")],
  related=["mdd_player_trajectory_display_type", "mdd_player_trajectory_frames"])

E(name="mdd_player_trajectory_display_type", kind="cvar", group="trajectory", default="0b11110",
  usage=["mdd_player_trajectory_display_type <0bXXXXX>"],
  summary="How the predicted trajectory is drawn.",
  sections=[("Information",
             "Bitmask.\n\n1 full bounding boxes.\n\n2 bottom squares only (a flat footprint).\n\n"
             "4 jump and teleport-enter frames always as full boxes.\n\n"
             "8 a connecting line through every simulated frame.\n\n"
             "16 static, world-pinned samples instead of flowing ones (only while you are airborne)."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_display_type 0b11110", "Default: footprints, jump boxes, line, static."),
            ("mdd_player_trajectory_display_type 0b01000", "Line only."),
            ("mdd_player_trajectory_display_type 0b00101", "Full boxes.")],
  related=["mdd_player_trajectory_strafe_modes", "mdd_player_trajectory_step"])

E(name="mdd_player_trajectory_frames", kind="cvar", group="trajectory", default="270",
  usage=["mdd_player_trajectory_frames <count>"],
  summary="How many frames ahead to predict.",
  sections=[("Information", "Number of simulated frames (8 ms each). Default 270, capped at 2000. Cost grows with it."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_frames 270", "Default, a little over two seconds."),
            ("mdd_player_trajectory_frames 125", "One second: cheaper.")],
  related=["mdd_player_trajectory_step", "mdd_player_trajectory_trace_entities"])

E(name="mdd_player_trajectory_step", kind="cvar", group="trajectory", default="10",
  usage=["mdd_player_trajectory_step <count>"],
  summary="Draw a sample every N simulated frames.",
  sections=[("Information",
             "Draw a sample every N simulated frames. Jump and teleport-enter frames are always drawn. The connecting "
             "line ignores this."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_step 10", "Default."), ("mdd_player_trajectory_step 5", "Denser samples.")],
  related=["mdd_player_trajectory_frames", "mdd_player_trajectory_display_type"])

E(name="mdd_player_trajectory_trace_entities", kind="cvar", group="trajectory", default="1",
  usage=["mdd_player_trajectory_trace_entities <0|1>"],
  summary="Also collide the prediction with doors, plats and other brush entities.",
  sections=[("Information",
             "1: also trace against the solid brush entities in your snapshot (doors, plats, trains, buttons), not just "
             "the world. Costs frame rate on entity-heavy maps; set 0 there. Moving entities are traced where they "
             "were last seen."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_trace_entities 0", "Frame rate drops on a map full of doors: world only."),
            ("mdd_player_trajectory_trace_entities 1", "Default.")],
  related=["mdd_player_trajectory_trace_triggers", "mdd_player_trajectory_frames"])

E(name="mdd_player_trajectory_trace_triggers", kind="cvar", group="trajectory", default="1",
  usage=["mdd_player_trajectory_trace_triggers <0|1>"],
  summary="Apply jump pads and stop at teleporters in the prediction.",
  sections=[("Information",
             "1: jump pads apply their real push to the prediction, and a teleporter highlights the frame that touches "
             "it and ends the prediction there (the destination isn't known to the client)."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_trace_triggers 1", "Default."),
            ("mdd_player_trajectory_trace_triggers 0", "Ignore pads and teleporters.")],
  related=["mdd_player_trajectory_trace_entities", "mdd_player_trajectory_rgba_trigger"])

for _n, _d, _what in (("passive", ".5 .5 .5 .5", "the passive model's samples"),
                      ("strafebot", "0 .5 0 .5", "the strafebot model's samples"),
                      ("accel", ".4 .4 0 .4", "the accel model's samples"),
                      ("jump", ".8 0 0 .8", "jump frames of any model"),
                      ("trigger", "0 .5 1 .5", "the frame that touches a teleporter")):
  E(name="mdd_player_trajectory_rgba_" + _n, kind="cvar", group="trajectory", default=_d,
    usage=["mdd_player_trajectory_rgba_%s <r> <g> <b> <a>" % _n],
    summary="Colour of " + _what + ".",
    sections=[("Information", "Colour and opacity of %s, each 0..1. Default %s." % (_what, _d)), ("Notes", TRAJ_NOTE)],
    examples=[("mdd_player_trajectory_rgba_%s %s" % (_n, _d), "Default."),
              ("mdd_player_trajectory_rgba_%s 1 1 1 .3" % _n, "Faint white.")],
    related=["mdd_player_trajectory_strafe_modes", "mdd_player_trajectory_display_type"])

E(name="mdd_player_trajectory_shader_line", kind="cvar", group="trajectory", default="railCore",
  usage=["mdd_player_trajectory_shader_line <shader>"],
  summary="Shader used for the connecting line.",
  sections=[("Information",
             "Shader NAME (not an image path) for the connecting line. Takes effect immediately. A custom look needs a "
             ".shader script in a pk3 that defines the name."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_shader_line railCore", "Default.")],
  related=["mdd_player_trajectory_shader_box", "mdd_player_trajectory_display_type"])

E(name="mdd_player_trajectory_shader_box", kind="cvar", group="trajectory", default="bbox_nocull",
  usage=["mdd_player_trajectory_shader_box <shader>"],
  summary="Shader used for the boxes and footprints.",
  sections=[("Information",
             "Shader NAME (not an image path) for every drawn box and footprint. Takes effect immediately. A custom "
             "look needs a .shader script in a pk3 that defines the name."),
            ("Notes", TRAJ_NOTE)],
  examples=[("mdd_player_trajectory_shader_box bbox_nocull", "Default.")],
  related=["mdd_player_trajectory_shader_line", "mdd_player_trajectory_display_type"])

E(name="mdd_player_trajectory_debug", kind="cvar", group="trajectory", default="0",
  usage=["mdd_player_trajectory_debug <0..5>"],
  summary="Developer output for the trajectory prediction.",
  sections=[("Information",
             "Verbosity level, not needed for normal use. 1 every simulated frame, 2 the next predicted jump frame, "
             "3 the strafebot steering chain once per jump, 4 the first 10 frames every render frame, 5 the gathered "
             "entities and triggers. Any nonzero level also prints a timing line. Capture with \\logfile 2."),
            ("Notes", TRAJ_NOTE)],
  examples=[("logfile 2; mdd_player_trajectory_debug 2", "Log the timing line to see what the prediction costs on this map."),
            ("mdd_player_trajectory_debug 0", "Off.")],
  related=["mdd_player_trajectory_frames", "mdd_player_trajectory_trace_entities"])
