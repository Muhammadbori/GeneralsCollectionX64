# Golden replays (Generals Collection x64)

Replays recorded with this project's Windows x64 build, played by the replay CRC gate of the macOS port
(`.github/workflows/crc-selfhosted.yml`, `scripts/ci/play_golden.py`): every platform must compute the same logic CRC on
every frame (`-logicCrcLog`). Only `.rep` files are here; no game data, maps or CRC logs.

| game | replay | frames | note |
|---|---|---|---|
| GeneralsMD | zh_river.rep | 6000 | skirmish, 2 AI |
| GeneralsMD | mp_lan.rep | 3506 | LAN match |
| Generals | g_short.rep, g_short_o136.rep | 1500 | skirmish |
| Generals | g_desert.rep | 1602 | stops with "CRC Mismatch in Frame 1600" on every platform (a known wall clock issue); compared up to there |

Two more ZH replays (zh_desert, zh_desert_o136, 9000 frames, about 1.5 MB each) are kept outside the repository.

## Setting up a self-hosted runner

The game data stays on the runner machine and is never uploaded.

1. Install Generals and Zero Hour on the machine.
2. Add the runner in the repository settings (Settings > Actions > Runners > New self-hosted runner), with the label
   `generals-windows` (Windows x64) or `generals-macos` (Apple Silicon Mac).
3. Give the runner these environment variables (its `.env` file next to the runner):
   - `RTS_GAME_DIR_ZEROHOUR`: the Zero Hour game folder;
   - `RTS_GAME_DIR_GENERALS`: the Generals game folder;
   - Windows only: `VS_VCVARS64`: the path of `vcvars64.bat` of Visual Studio.
4. Run the runner as its own user account with access to these folders only.
5. Start "Replay CRC (self-hosted)" by hand in the Actions tab. It never runs on pull requests.
