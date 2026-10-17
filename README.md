# Bayonetta 3 — Dynamic FPS

An exefs code mod (an [exlaunch](https://github.com/shadowninja108/exlaunch) `subsdk9` module) that removes the 60 FPS cap from Bayonetta 3 and makes game logic follow real frame time.

- **At 60 FPS and above** the game runs at true speed: 90, 120, 144 FPS and so on.
- **Below 60 FPS** the game runs in slow motion (one update never advances more than one 1/60 s tick). This is deliberate: it keeps physics and timed events safe.
- About 700 hooks convert per-frame counters, timers and smoothing to real time, so timers, Witch Time and enemy patterns keep their normal length at high FPS.

> **Status: beta.** Tested in short sessions on Eden. A full playthrough is not finished yet. Please report problems (see below).

## Requirements

| | |
|---|---|
| Game | Bayonetta 3, title ID `01004A4010FEA000` |
| Version | **1.2.0 only** (main build ID `B47CC79B88B7091EA514B2B725C50AE01C6F00DF`). Other versions will crash or misbehave. |
| Emulator | Eden (tested). Other yuzu forks and Ryujinx load exefs mods the same way, but they are not tested. Real hardware (Atmosphère) is not tested. |

## Install

1. Download `Bayonetta3-Dynamic-FPS-<version>.zip` from [Releases](../../releases).
2. Extract it. You get a folder `Dynamic FPS/exefs/` with `subsdk9` and `main.npdm`.
3. Put the `Dynamic FPS` folder in your emulator's mod folder for the game:
   - **Eden / yuzu forks:** right-click the game → *Open Mod Data Location*, or `~/.local/share/eden/load/01004A4010FEA000/`
   - **Ryujinx:** right-click the game → *Open Atmosphere Mods Directory*
   - **Atmosphère (untested):** copy the *contents* of `exefs/` to `atmosphere/contents/01004A4010FEA000/exefs/`
4. **Disable every other FPS mod** for this game (for example theboy181's "60fps" pchtxt). They patch the same code.
5. In Eden, turn **off** *Sync to framerate of video playback*, or the game stays capped at 60. Turn off VSync / the emulator frame limit if you want more than your display rate.

Graphics-only mods (for example "Improved LOD v2", "Remove Film Grain") work alongside it.

To uninstall, delete the `Dynamic FPS` folder.

## Known limits

The mod logs one `[B3DYN]` line to the emulator log about every 600 updates. Each counter on it counts a rare case the mod cannot handle exactly. All of them should stay at `0` in normal play:

| Counter | Meaning |
|---|---|
| `nodeFull`, `staleTok` | More than 576 queued events needed a speed copy at once. |
| `stepFull` | More than 512 objects had a cached step at once. |
| `stepZeroUnk` | A zero-length update where the mod could not tell the step's speed factor. |
| `cntEvictLive`, `genEvictLive`, `velEvictLive`, `pickEvictLive`, `phaseEvictLive`, `thrOverflow` | A tracking table overflowed or an address was reused. |

Pre-rendered movies play at their own speed. The Bayonetta Origins prologue is a separate program and is not covered.

## Reporting a problem

Open an issue with:

1. Where it happened (chapter, place, action) and your FPS.
2. What looked wrong (too fast, too slow, something happens twice, freeze, crash).
3. The last `[B3DYN]` line from your emulator log (Eden: `~/.local/share/eden/log/eden_log.txt`).

## Build from source

Needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64 and libnx.

```sh
DEVKITPRO=/opt/devkitpro ./build.sh     # output: dist/Dynamic FPS/exefs/
```

Or use the `devkitpro/devkita64` Docker image. Hook offsets are in `source/program/`; the design is in [ARCHITECTURE.md](ARCHITECTURE.md) (working notes, newest section at the end).

## License and credits

GPL-2.0, inherited from exlaunch (see [LICENSE](LICENSE)). exlaunch by shadowninja108 (see [EXLAUNCH_README.md](EXLAUNCH_README.md)). theboy181's Bayonetta 3 60fps pchtxt was a reference for the frame-cap patch points.

This repository contains no game code or assets. You need your own legally dumped copy of the game.
