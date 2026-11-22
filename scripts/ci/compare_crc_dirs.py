#!/usr/bin/env python3
# TheSuperHackers @build O140 Compares the per-frame logic CRC logs of two platforms (play_golden.py output folders):
# every *.crc.log of the reference folder against the same file of the other. Prints the first differing frame of each
# replay (a desync to fix) and exits 1 when any replay differs or is missing.
#
#   compare_crc_dirs.py windows-x64 macos-arm64
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
reference, other = sys.argv[1], sys.argv[2]
failed = 0
lines = ["| replay | %s vs %s |" % (os.path.basename(reference), os.path.basename(other)), "|---|---|"]
for name in sorted(os.listdir(reference)):
    if not name.endswith(".crc.log"):
        continue
    if not os.path.exists(os.path.join(other, name)):
        lines.append("| %s | missing |" % name)
        failed += 1
        continue
    result = subprocess.run([sys.executable, os.path.join(here, "crcdiff.py"), os.path.join(reference, name), os.path.join(other, name)],
                            capture_output=True, text=True)
    verdict = [l for l in result.stdout.splitlines() if l.startswith(("SAME", "FIRST DIFF"))]
    lines.append("| %s | %s |" % (name, verdict[0] if verdict else result.stdout.strip()[-120:]))
    if result.returncode != 0:
        failed += 1
print("\n".join(lines))
summary = os.environ.get("GITHUB_STEP_SUMMARY")
if summary:
    with open(summary, "a") as f:
        f.write("\n".join(lines) + "\n")
sys.exit(1 if failed else 0)
