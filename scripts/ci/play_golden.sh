#!/bin/sh
# TheSuperHackers @build O166 Plays the golden replays headless with -logicCrcLog, one per-frame logic CRC log per
# replay: the job of play_golden.py in plain POSIX sh, with only what a clean macOS has (no Python, no Xcode command
# line tools), for the self-hosted Mac of crc-selfhosted.yml.
#
# The game data is never copied or uploaded: the game folder is mirrored into the work folder with hard links (symbolic
# links when it is on another volume), the exe goes there under a test exe name ("..._ci_uitest", which -logicCrcLog
# needs) and every replay runs with its own user data folder.
#
#   play_golden.sh GAME EXE GAME_DIR WORK_DIR OUT_DIR REPLAY_DIR [TIMEOUT_SECONDS]
#   GAME is Generals or GeneralsMD; REPLAY_DIR holds the .rep files of that game.
set -u

if [ $# -lt 6 ]; then
	echo "usage: $0 GAME EXE GAME_DIR WORK_DIR OUT_DIR REPLAY_DIR [TIMEOUT_SECONDS]" >&2
	exit 2
fi
game=$1
exe=$2
game_dir=$3
work=$4
out=$5
replays=$6
timeout_seconds=${7:-900}

absolute() { # a folder's absolute path
	( cd "$1" && pwd -P )
}

mkdir -p "$work" "$out" || exit 1
game_dir=$(absolute "$game_dir") || { echo "no game folder: $3" >&2; exit 1; }
work=$(absolute "$work")
out=$(absolute "$out")
replays=$(absolute "$replays") || { echo "no replay folder: $6" >&2; exit 1; }

# Hard link mirror of the game folder (no copies).
( cd "$game_dir" && find . -type d ) | while IFS= read -r dir; do
	mkdir -p "$work/$dir"
done
( cd "$game_dir" && find . -type f ) | while IFS= read -r file; do
	if [ ! -e "$work/$file" ]; then
		ln "$game_dir/$file" "$work/$file" 2>/dev/null || ln -s "$game_dir/$file" "$work/$file"
	fi
done

name=$(basename "$exe")
test_exe="$work/${name}_ci_uitest"
cp "$exe" "$test_exe" || exit 1
chmod +x "$test_exe"

failed=0
for rep in "$replays"/*.rep; do
	[ -f "$rep" ] || continue
	file=$(basename "$rep")
	replay=${file%.rep}
	user_data="$work/ud_$replay"
	rm -rf "$user_data"
	mkdir -p "$user_data/Replays"
	cp "$rep" "$user_data/Replays/$file"
	log="$out/$game.$replay.crc.log"
	rm -f "$log"

	# The run, with a watchdog that stops it after the timeout.
	( cd "$work" && exec "$test_exe" -headless -replay "$file" -userDataDir "$user_data" -logicCrcLog "$log" ) \
		> "$out/$game.$replay.out.txt" 2>&1 &
	pid=$!
	( sleep "$timeout_seconds"; kill -9 "$pid" 2>/dev/null ) &
	watchdog=$!
	wait "$pid"
	code=$?
	kill "$watchdog" 2>/dev/null
	wait "$watchdog" 2>/dev/null

	# Exit code 1 with "CRC Mismatch" is the replay's own check (g_desert stops at frame 1600 on every platform, a
	# known wall clock issue); the cross-platform comparison of the logs is the gate.
	if [ -f "$log" ]; then
		frames=$(grep -vc '^#' "$log")
	else
		frames=0
		failed=$((failed + 1))
	fi
	echo "$game $replay: exit $code, $frames frames logged"
done

[ "$failed" -eq 0 ]
