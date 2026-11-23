# Golden replays (Generals Collection x64)

Replays recorded with this project's Windows x64 build, played by the replay CRC gate of the macOS port
(`.github/workflows/crc-selfhosted.yml`, `scripts/ci/play_golden.py`): every platform must compute the same logic CRC on
every frame (`-logicCrcLog`). Here are the `.rep` files and, in `crc/windows-x64`, the per-frame CRC logs of the
Windows x64 build for them (O165: the reference the Mac runner is compared with; numbers only). No game data or maps.

| game | replay | frames | note |
|---|---|---|---|
| GeneralsMD | zh_river.rep | 6000 | skirmish, 2 AI |
| GeneralsMD | mp_lan.rep | 3506 | LAN match |
| Generals | g_short.rep, g_short_o136.rep | 1500 | skirmish |
| Generals | g_desert.rep | 1602 | stops with "CRC Mismatch in Frame 1600" on every platform (a known wall clock issue## Setting up the self-hosted runner

O165: there is one runner, an Apple Silicon Mac with the label `generals-macos`; there is no Windows runner. Its logs
are compared with `crc/windows-x64`. The step-by-step guide (in Uzbek) is `dx12-notes/plan10-boburbek-mac-runner.md`
of the project notes. In short:

1. Copy the game folder (Generals Collection x64 without the exe and dll files: `Big`, `Data`, `Packs`,
   `Archives_*.txt`, `Collection_manifest.txt`, `langdata.dat`; about 4.2 GB) to the Mac's own disk.
2. Add the runner (Settings > Actions > Runners > New self-hosted runner > macOS ARM64) with `--labels generals-macos`,
   in its own macOS user account.
3. In the runner's `.env`: `RTS_GAME_DIR_ZEROHOUR` and `RTS_GAME_DIR_GENERALS`, both the game folder.
4. Start "Replay CRC (self-hosted)" by hand in the Actions tab. It runs only by hand and only in the owner's repository.

When the replays or the Windows build change, make the reference logs again with `scripts/ci/play_golden.py` and the
Windows x64 build (`--out Tests/GoldenReplays/crc/windows-x64`; keep only the `*.crc.log` files).
