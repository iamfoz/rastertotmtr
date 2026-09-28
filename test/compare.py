#!/usr/bin/env python3
"""Diff the new filter (arm64 and x86_64 slices) against Epson's original, byte for byte."""
import os, subprocess, sys, itertools

HERE = os.path.dirname(os.path.abspath(__file__))
ORIG = "/Library/Printers/EPSON/tmprinter/filter/rastertotmtr.app/Contents/MacOS/rastertotmtr"
NEW = os.path.join(HERE, "..", "rastertotmtr")
NEW_X86 = os.path.join(HERE, "rastertotmtr.x86_64")
subprocess.run(["lipo", NEW, "-thin", "x86_64", "-output", NEW_X86], check=True)
subprocess.run(["codesign", "-s", "-", "-f", NEW_X86], check=True, capture_output=True)

env = dict(os.environ, PPD=os.path.join(HERE, "tm.ppd"))
QUEUE = "EPSON_TM_m30II"


def run(exe, args, stdin=None, envx=None):
    p = subprocess.run([QUEUE] + args, executable=exe, env=envx or env,
                       stdin=stdin, capture_output=True)
    return p.returncode, p.stdout, p.stderr


def check(label, args, stdin_file=None, envx=None):
    outs = []
    for exe in (ORIG, NEW, NEW_X86):
        f = open(stdin_file, "rb") if stdin_file else None
        outs.append(run(exe, args, f, envx))
        if f:
            f.close()
    ok = outs[0] == outs[1] == outs[2]
    if not ok:
        print("DIFF", label, [(o[0], len(o[1])) for o in outs])
        for o in outs[1:]:
            if o[2] != outs[0][2]:
                print("  stderr differs:\n   ", outs[0][2].decode(errors="replace")[-300:],
                      "\n   ", o[2].decode(errors="replace")[-300:])
    return ok, outs[0]


total = passed = 0
for ras, red, cut, bz in itertools.product(
        ["page1.ras", "multi.ras", "page58.ras"],
        ["Off", "Top", "Bottom", "Both"],
        ["NoCut", "CutPerJob", "CutPerPage"],
        ["NotUsed", "InternalBuzzer", "ExternalBuzzer", "OpenDrawer1", "OpenDrawer2"]):
    opts = f"TmxPaperReduction={red} TmxPaperCut={cut} TmxBuzzerAndDrawer={bz}"
    ok, o = check(f"{ras} {opts}", ["1", "user", "title", "1", opts, os.path.join(HERE, ras)])
    total += 1
    passed += ok
print(f"option matrix: {passed}/{total} identical (stdout, stderr, exit code)")

extra = [
    ("stdin mode", ["1", "user", "title", "1", ""], "multi.ras", None),
    ("bad argc", ["1", "2", "3"], None, None),
    ("missing input", ["1", "u", "t", "1", "", "/nonexistent.ras"], None, None),
    ("bogus option", ["1", "u", "t", "1", "TmxPaperCut=Bogus", os.path.join(HERE, "page1.ras")], None, None),
    ("no PPD", ["1", "u", "t", "1", "", os.path.join(HERE, "page1.ras")], None, dict(env, PPD="/nonexistent")),
    ("truncated raster", ["1", "u", "t", "1", "", os.path.join(HERE, "trunc.ras")], None, None),
]
for label, args, stdin_file, envx in extra:
    ok, o = check(label, args, stdin_file, envx)
    print(f"{label}: {'identical' if ok else 'DIFFERENT'} (rc={o[0]}, {len(o[1])} bytes out)")
    passed += ok
    total += 1
print(f"TOTAL {passed}/{total}")
sys.exit(0 if passed == total else 1)
