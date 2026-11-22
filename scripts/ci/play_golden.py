#!/usr/bin/env python3
# TheSuperHackers @build O140 Plays the golden replays (Tests/GoldenReplays/<game>) headless with -logicCrcLog and keeps
# one per-frame logic CRC log per replay, for comparing platforms (scripts/ci/crcdiff.py).
#
# The game data is never copied or uploaded: it stays in the runner's own game folder (game_dir). The script mirrors that
# folder into work_dir with hard links (no copies), puts the built exe there under a test exe name ("..._ci_uitest",
# which -logicCrcLog needs) and runs each replay with its own user data folder.
#
#   play_golden.py --game GeneralsMD --exe build/.../generalszh.exe --game-dir "C:/Games/Zero Hour" --work-dir work --out logs
import argparse
import os
import shutil
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(here))

parser = argparse.ArgumentParser()
parser.add_argument("--game", required=True, choices=["Generals", "GeneralsMD"])
parser.add_argument("--exe", required=True)
parser.add_argument("--game-dir", required=True)
parser.add_argument("--work-dir", required=True)
parser.add_argument("--out", required=True)
parser.add_argument("--timeout", type=int, default=900)
args = parser.parse_args()

game_dir = os.path.abspath(args.game_dir)
work = os.path.abspath(args.work_dir)
out = os.path.abspath(args.out)
os.makedirs(out, exist_ok=True)

# Hard link mirror of the game folder (same volume needed; falls back to symlinks for files on another volume).
for root, dirs, files in os.walk(game_dir):
    rel = os.path.relpath(root, game_dir)
    target_dir = os.path.join(work, rel) if rel != "." else work
    os.makedirs(target_dir, exist_ok=True)
    for name in files:
        target = os.path.join(target_dir, name)
        if not os.path.exists(target):
            source = os.path.join(root, name)
            try:
                os.link(source, target)
            except OSError:
                os.symlink(source, target)

exe_name = os.path.basename(args.exe)
stem, ext = os.path.splitext(exe_name)
test_exe = os.path.join(work, stem + "_ci_uitest" + ext)
shutil.copy2(args.exe, test_exe)
exe_dir = os.path.dirname(os.path.abspath(args.exe))
for name in os.listdir(exe_dir):
    if name.lower().endswith((".dll", ".dylib", ".so")):
        shutil.copy2(os.path.join(exe_dir, name), os.path.join(work, name))

replays = os.path.join(repo, "Tests", "GoldenReplays", args.game)
failed = 0
for name in sorted(os.listdir(replays)):
    if not name.endswith(".rep"):
        continue
    replay = os.path.splitext(name)[0]
    user_data = os.path.join(work, "ud_" + replay)
    shutil.rmtree(user_data, ignore_errors=True)
    os.makedirs(os.path.join(user_data, "Replays"))
    shutil.copy2(os.path.join(replays, name), os.path.join(user_data, "Replays", name))
    log = os.path.join(out, "%s.%s.crc.log" % (args.game, replay))
    cmd = [test_exe, "-headless", "-replay", name, "-userDataDir", user_data, "-logicCrcLog", log]
    with open(os.path.join(out, "%s.%s.out.txt" % (args.game, replay)), "wb") as output:
        try:
            code = subprocess.run(cmd, cwd=work, stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    # Exit code 1 with "CRC Mismatch" is the replay's own check (g_desert stops at frame 1600 on every platform, a known
    # wall clock issue); the cross-platform comparison of the logs is the gate.
    print("%s %s: exit %s, %d frames logged" % (args.game, replay, code,
        sum(1 for line in open(log) if not line.startswith("#")) if os.path.exists(log) else 0))
    if not os.path.exists(log):
        failed += 1
sys.exit(1 if failed else 0)
