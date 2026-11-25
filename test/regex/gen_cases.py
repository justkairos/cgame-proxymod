#!/usr/bin/env python3
"""Writes test vectors for test/regex/regex_test.c: each pattern on each text, with Python's re as the reference.
   gen_cases.py > cases.txt      line format: pattern TAB text TAB result   (result: "E", or the matches joined by \x01)"""
import random, re, sys

random.seed(7)
patterns = [
  r"abc", r"a.c", r"^abc", r"abc$", r"^$", r"a*", r"a+", r"a?b", r"ab*c", r"(ab)+", r"(?:ab|cd)+e", r"a|b|c", r"(a|ab)(c|bcd)",
  r"[abc]+", r"[^abc]+", r"[a-f0-9]+", r"[]a]+", r"[^]a]+", r"[a\-z]+", r"[\d\s]+", r"[^\w]+", r"\d+", r"\D+", r"\w+", r"\W+",
  r"\s+", r"\S+", r"\bfoo\b", r"\Bfoo", r"foo\B", r"a{3}", r"a{2,}", r"a{2,4}", r"a{0,2}b", r"(ab){2,3}", r"a{2,4}?", r"a+?",
  r"a*?b", r"a??b", r".*", r".+?x", r"<.*>", r"<.*?>", r"(?i)hello", r"(?i)[a-c]+", r"(?i)x\d", r"\x41+", r"\.\*\+", r"a\|b",
  r"(a(b(c)?)?)?d", r"x*y*z*", r"(x|y)*z", r"[0-9]+\.[0-9]+", r"cg_\w*time", r"^\s*\d+\s+(weapon|ammo)_\w+", r"\$\d+", r"a{,2}",
  r"a{x}", r"{", r"a{1", r"\'quoted\'", r"mdd_[a-z_]+", r"(\d+):(\d+)\.(\d+)", r"[A-Z][a-z]+ [A-Z][a-z]+", r"\t", r"a\tb",
  # errors
  r"(", r")", r"a)", r"[abc", r"*a", r"a**", r"\\", "ab\\", r"a{5,2}", r"[z-a]", r"\xZZ", r"a+*",
]
texts = [
  "", "abc", "aabbcc", "xabcabcx", "aaaa", "aaaaaab", "abababab", "ab cd ab cdcde", "foo foobar barfoo foo", "<a><b> <c>",
  "Hello HELLO hello hElLo", "x1 X2 y3", "AAA a|b .*+", "abcd acd bcd d", "xxyyzz z xyz", "3.14 2.718 10 .5 5.",
  "cg_centertime cg_drawTimer cg_fov cg_time", "   12 weapon_rocketlauncher", "  3  ammo_rockets", "cost $15 and $200",
  "'quoted' text", "mdd_grenade_sync mdd_x", "time 0:18.800 and 12:01.050", "Evan Compton went To Town", "a\tb", "{a{1}",
  "]]a]a", "a-z", "1 2\t3", "!!! ???", "aXbXc", "the quick brown fox",
]
for _ in range(40):
  texts.append("".join(random.choice("abAB01 _.-x") for _ in range(random.randint(0, 24))))
out = []
for p in patterns:
  try:
    rx = re.compile(p)
  except re.error:
    rx = None
  for t in texts:
    if "\n" in t:
      continue
    if rx is None:
      res = "E"
    else:
      res = "M" + "\x01".join(m.group(0) for m in rx.finditer(t) if m.group(0) != "")
    out.append("%s\t%s\t%s" % (p.replace("\t", "\x02"), t.replace("\t", "\x02"), res.replace("\t", "\x02")))
sys.stdout.write("\n".join(out) + "\n")
