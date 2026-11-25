#!/usr/bin/env python3
"""Builds a small game tree for test/pk3/pk3_sim.c:  make_tree.py <root>
<root>/defrag/{aaa.pk3, zzz-maps.pk3, broken.pk3, notes.txt, maps/loose.bsp, textures/sky_loose/..} and <root>/baseq3/pak0.pk3"""
import os, struct, sys, zipfile

root = sys.argv[1]
os.makedirs(root + "/defrag/maps", exist_ok=True)
os.makedirs(root + "/defrag/textures/sky_loose", exist_ok=True)
os.makedirs(root + "/defrag/system/records", exist_ok=True)
os.makedirs(root + "/baseq3", exist_ok=True)


def bsp(entities, shaders, brushes, sides, models=3, faces=7):
  """shaders: (name, surfaceFlags, contentFlags); brushes: shader index; sides: shader index"""
  lumps = [b""] * 17
  lumps[0] = entities.encode() + b"\0"
  lumps[1] = b"".join(struct.pack("<64sii", n.encode(), s, c) for n, s, c in shaders)
  lumps[7] = b"\0" * (40 * models)
  lumps[8] = b"".join(struct.pack("<iii", 0, 6, sh) for sh in brushes)
  lumps[9] = b"".join(struct.pack("<ii", 0, sh) for sh in sides)
  lumps[13] = b"\0" * (104 * faces)
  out, off, table = b"", 8 + 17 * 8, b""
  for l in lumps:
    table += struct.pack("<ii", off, len(l))
    out += l
    off += len(l)
  return b"IBSP" + struct.pack("<i", 46) + table + out


ENT = """{
"classname" "worldspawn"
"message" "Test Run by kairos"
"music" "music/sonic1.wav"
}
{ "classname" "info_player_deathmatch" "origin" "0 0 24" }
{ "classname" "weapon_rocketlauncher" "origin" "0 0 0" }
{ "classname" "weapon_rocketlauncher" "targetname" "give1" }
{ "classname" "weapon_plasmagun" "targetname" "give1" }
{ "classname" "target_give" "target" "give1" "targetname" "t1" }
{ "classname" "trigger_multiple" "target" "t1" "model" "*1" }
{ "classname" "trigger_push" "model" "*2" "target" "p1" }
{ "classname" "target_position" "targetname" "p1" }
{ "classname" "func_door" "model" "*3" "health" "1" }
{ "classname" "func_door" "model" "*4" }
{ "classname" "target_startTimer" "targetname" "s" }
{ "classname" "target_stopTimer" "targetname" "e" }
{ "classname" "target_checkpoint" }
{ "classname" "target_checkpoint" }
{ "classname" "target_speed" "speed" "800" }
{ "classname" "ammo_rockets" }
{ "classname" "item_quad" }
{ "origin" "1 2 3" }
"""
SH = [("textures/base/wall", 0, 1), ("textures/test/slick", 2, 1), ("textures/liquids/lava", 0, 8),
      ("textures/common/nodamage", 1, 1), ("textures/test/slick2", 2, 1)]
run = bsp(ENT, SH, [0, 0, 2, 2, 2, 1], [0, 0, 1, 1, 1, 4, 3, 0])
frags = bsp('{ "classname" "worldspawn" }\n{ "classname" "target_fragsFilter" "frags" "3" }\n{ "classname" "target_score" }\n'
            '{"classname" "info_player_start"}', [("textures/base/wall", 0, 1)], [0], [0] * 6, models=1)

with zipfile.ZipFile(root + "/defrag/zzz-maps.pk3", "w", zipfile.ZIP_DEFLATED) as z:
  z.writestr("maps/testrun.bsp", run)
  z.writestr("maps/fragmap.bsp", frags)
  z.writestr("levelshots/testrun.jpg", b"x")
  z.writestr("textures/test/slick.tga", b"x")
  z.writestr("textures/test/Rocket_Fire.tga", b"x")
  z.writestr("scripts/testrun.arena", b"x")
with zipfile.ZipFile(root + "/defrag/aaa.pk3", "w") as z:
  z.writestr("maps/testrun.bsp", run)  # an older copy of the same map
  z.writestr("textures/skies/", b"")  # explicit folder entry
  z.writestr("textures/skies/sky1.tga", b"x")
  z.writestr("sound\\weapons\\rocket\\rocklf1a.wav", b"x")  # backslashes
  z.writestr("/leading/slash.txt", b"x")
  for i in range(3000):
    z.writestr("models/many/f%04d.md3" % i, b"")
with zipfile.ZipFile(root + "/baseq3/pak0.pk3", "w", zipfile.ZIP_DEFLATED) as z:
  z.writestr("models/weapons2/rocketl/rocketl.md3", b"x")
  z.writestr("scripts/sky.shader", b"x")
  z.comment = b"a zip comment at the very end " * 20
open(root + "/defrag/broken.pk3", "wb").write(b"PK\x05\x06" + b"\xff" * 40)
open(root + "/defrag/empty.pk3", "wb").write(b"")
open(root + "/defrag/notes.txt", "w").write("x")
open(root + "/defrag/maps/loose.bsp", "wb").write(frags)
open(root + "/defrag/maps/testrun.bsp", "wb").write(run)  # what the stub file system serves
open(root + "/defrag/maps/fragmap.bsp", "wb").write(frags)
open(root + "/defrag/maps/notabsp.bsp", "wb").write(b"hello world, definitely not a bsp" * 10)
open(root + "/defrag/textures/sky_loose/rocket.tga", "w").write("x")
rec = [0x9703, 2528, 5376, 8608, 12080, 15232, 18800] + [0] * 26 + [5]
open(root + "/defrag/system/records/testrun_mdf_vq3.rec", "wb").write(struct.pack("<34i", *rec))
rec = [0x3203, 21216] + [0] * 32
open(root + "/defrag/system/records/testrun_df_cpm.rec", "wb").write(struct.pack("<34i", *rec))
