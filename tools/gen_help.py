#!/usr/bin/env python3
"""Generates help/<name>.txt (DeFRaG help format, CRLF) and docs/reference.html from tools/helpdata.py.

  python3 tools/gen_help.py [--fragment out.html]

--fragment also writes the page without the <html> wrapper (for publishing as a hosted page)."""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import helpdata as H

PROJECT = "kairos' freestyle tools"
DATES = json.load(open(os.path.join(HERE, "help_dates.json")))
TRAJ_WRITTEN, TRAJ_MERGED = "2026-08-28", "2026-10-27"
KIND_TITLE = {"cvar": "CVAR", "button": "BUTTON", "cvar+button": "CVAR / BUTTON", "command": "COMMAND"}


def shown(e):
  return ("+" if e["kind"] == "button" else "") + e["name"]


def added(e):
  if e["group"] == "trajectory":
    return "%s (written %s by akuzmichov)" % (TRAJ_MERGED, TRAJ_WRITTEN)
  return DATES[e["name"]]


def box(text, ch):
  width = max(len(l) for l in text) if isinstance(text, list) else len(text)
  lines = text if isinstance(text, list) else [text]
  return [ch * width] + lines + [ch * width]


def render(title, usage, sections):
  out = box(title, "=") + [""]
  out += ["%i. %s" % (i + 1, s[0]) for i, s in enumerate(sections)] + [""]
  out += box(usage, "-") + [""]
  for i, (name, lines) in enumerate(sections):
    out += box("%i. %s" % (i + 1, name), "-") + lines + [""]
  return "\r\n".join(out)


def paragraphs(text):
  lines = []
  for p in text.split("\n\n"):
    lines += [p, ""]
  return lines[:-1]


def entry_txt(e):
  facts = []
  if e.get("default") is not None:
    facts.append('Default: "%s"' % e["default"])
  facts.append("Added: " + added(e))
  secs = []
  for i, (name, text) in enumerate(e["sections"]):
    lines = paragraphs(text)
    if i == 0:
      lines = [e["summary"], ""] + lines + ["", "   ".join(facts)]
    secs.append((name, lines))
  ex = []
  for cmd, what in e["examples"]:
    ex += [cmd, "    " + what, ""]
  secs.append(("Usage examples", ex[:-1]))
  if e["related"]:
    by = {x["name"]: x for x in H.ENTRIES}
    secs.append(("See also", [", ".join(shown(by[r]) for r in e["related"]), "mdd_help mdd for the overview."]))
  return render("%s: %s" % (KIND_TITLE[e["kind"]], shown(e)), e["usage"], secs)


def overview_txt():
  secs = [("Information", [
    "mDd client proxymod with %s. Install: put the cgame DLL into the defrag folder and set \"seta vm_cgame 0\". "
    "Type \"mdd_\" and press tab in the console to list everything." % PROJECT, "",
    "Help on any name below: \"mdd_help <name>\" or defrag's \"? <name>\" (both read system/help/<name>.txt; leave "
    "out the + of a button). Every help file ends with usage examples. docs/reference.html in the repository is the "
    "same content as a searchable page."])]
  for gid, gname in H.GROUPS:
    secs.append((gname, ["%-44s %s" % (shown(e), e["summary"]) for e in H.ENTRIES if e["group"] == gid]))
  secs.append(("Usage examples", [
    "mdd_help mdd_jumpshoot", "    Help for the +mdd_jumpshoot button.", "",
    "? mdd_grenade_sync", "    The same through defrag's own help command.", "",
    "bind mouse2 +mdd_jumpshoot", "    Buttons are bound like +attack.", "",
    "mdd_grenade_jump 1", "    Cvars are set like any other."]))
  return render("PROXYMOD: mdd (%s)" % PROJECT, ["mdd_help <name>   or   ? <name>"], secs)


def trajectory_txt():
  es = [e for e in H.ENTRIES if e["group"] == "trajectory"]
  secs = [("Information", [
    "Draws your predicted path as a row of hitboxes: where you will be over the next frames with your current input, "
    "including jumps, jump pads and collisions. By akuzmichov (written %s, merged into this fork %s)." %
    (TRAJ_WRITTEN, TRAJ_MERGED), "",
    "mdd_player_trajectory_strafe_modes is the on/off switch (0 = off). Each cvar below has its own help file; the "
    "full reference is docs/mdd_player_trajectory.md in the repository."]),
          ("Cvars", ["%-44s %-14s %s" % (e["name"], '"%s"' % e["default"], e["summary"]) for e in es]),
          ("Usage examples", [
            "mdd_player_trajectory_strafe_modes 0b010", "    On, showing where your current input takes you (default).", "",
            "mdd_player_trajectory_strafe_modes 0b110", "    Add the strafebot's ideal path to compare against.", "",
            "mdd_player_trajectory_frames 125; mdd_player_trajectory_trace_entities 0",
            "    Cheaper: one second ahead, world collision only.", "",
            "mdd_player_trajectory_strafe_modes 0", "    Off."])]
  return render("PROXYMOD: mdd_player_trajectory", ["mdd_player_trajectory_<setting> <value>"], secs)


def main():
  names = [e["name"] for e in H.ENTRIES]
  assert len(names) == len(set(names))
  assert set(names) == set(DATES), set(names) ^ set(DATES)
  files = {e["name"]: entry_txt(e) for e in H.ENTRIES}
  files["mdd"] = overview_txt()
  files["mdd_player_trajectory"] = trajectory_txt()
  hd = os.path.join(ROOT, "help")
  for old in os.listdir(hd):
    if old.endswith(".txt") and old[:-4] not in files:
      print("stale help file:", old)
  for n, t in files.items():
    with open(os.path.join(hd, n + ".txt"), "w", newline="") as f:
      f.write(t + "\r\n")

  data = {
    "project": PROJECT,
    "groups": [{"id": g, "name": n} for g, n in H.GROUPS],
    "entries": [{
      "name": e["name"], "shown": shown(e), "kind": e["kind"], "group": e["group"], "default": e.get("default"),
      "added": DATES[e["name"]] if e["group"] != "trajectory" else TRAJ_MERGED,
      "addedNote": "written %s by akuzmichov, merged into the fork %s" % (TRAJ_WRITTEN, TRAJ_MERGED)
      if e["group"] == "trajectory" else None,
      "usage": e["usage"], "summary": e["summary"], "sections": [list(s) for s in e["sections"]],
      "examples": [list(x) for x in e["examples"]], "related": e["related"], "txt": files[e["name"]].replace("\r\n", "\n"),
    } for e in H.ENTRIES],
  }
  page = open(os.path.join(HERE, "reference_template.html")).read()
  page = page.replace("/*DATA*/null", json.dumps(data, separators=(",", ":")).replace("</", "<\\/"))
  with open(os.path.join(ROOT, "docs", "reference.html"), "w") as f:
    f.write('<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width, initial-scale=1">\n</head>\n<body>\n' + page +
            "\n</body>\n</html>\n")
  if "--fragment" in sys.argv:
    with open(sys.argv[sys.argv.index("--fragment") + 1], "w") as f:
      f.write(page)
  print("%i help files, %i entries" % (len(files), len(H.ENTRIES)))


main()
