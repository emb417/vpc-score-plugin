# vpc-score-plugin

A VPX plugin that reads live game state from NVRAM and broadcasts score events via a local WebSocket server on `127.0.0.1:3131`. Designed to work alongside [vpc-score-agent](https://github.com/emb417/vpc-score-agent), which consumes these events and forwards them to the VPC Data API.

---

## How it works

1. VPX loads the plugin at startup by scanning its `plugins` folder
2. When a table starts, the plugin resolves the ROM name and loads the matching NVRAM map from the bundled `nvram-maps` directory
3. On every rendered frame, the plugin reads the `.nv` file from disk, decodes scores and game state from NVRAM, and broadcasts `current_scores` events via WebSocket when state changes
4. On game start and game end, dedicated `game_start` and `game_end` events are broadcast with full score payloads
5. `vpc-score-agent` connects to the WebSocket, enriches each event with player identity and VPS table context, and forwards it to the VPC Data API

---

## Prerequisites

- VPinballX 10.8+ (BGFX or DX build)
- PinMAME installed (provides the `.nv` NVRAM files the plugin reads)
- [vpc-score-agent](https://github.com/emb417/vpc-score-agent) running alongside VPX (launched via PinUp Popper open script)

---

## Installation

### Step 1: Download the release

Download the latest `vpc-score-plugin-windows.zip` from the [Releases](../../releases) page.

### Step 2: Extract to the VPX plugins folder

Locate your VPX executable — on a typical Baller Installer cabinet this is:

```bash
C:\vPinball\VisualPinball\VPinballX_BGFX64.exe
```

Create a `plugins` folder next to the exe if it doesn't already exist, then extract the zip contents into a `vpc-score-plugin` subfolder:

```bash
C:\vPinball\VisualPinball\plugins\vpc-score-plugin\
  plugin-vpc-score64.dll
  plugin.cfg
  nvram-maps\
```

> The `plugins` folder must be in the same directory as the VPX executable that PinUp Popper launches. To confirm which exe is running, check Task Manager while a table is active.

### Step 3: Verify the plugin loaded

Launch VPX and open a table. Enable logging in VPX via **Preferences → Editor → Enable Log**. Check `%AppData%\VPinballX\VPinballX.log` for:

```bash
VPC Score Plugin loading
NVRAM maps path: ...
WebSocket server listening on 127.0.0.1:3131
```

---

## WebSocket event contract

The plugin broadcasts JSON events on `ws://127.0.0.1:3131`. All events share a common envelope:

| Field       | Description                     |
| ----------- | ------------------------------- |
| `type`      | Event type (see below)          |
| `timestamp` | ISO 8601 UTC timestamp          |
| `rom`       | ROM name as reported by PinMAME |

### `connected`

Sent immediately to each client on successful WebSocket handshake.

```json
{
  "type": "connected",
  "timestamp": "2026-03-16T18:00:00.000Z",
  "server": "vpc-score-plugin",
  "version": "1.0",
  "rom": "lostspc"
}
```

### `game_start`

Sent when PinMAME fires the `OnGameStart` event.

```json
{
  "type": "game_start",
  "timestamp": "2026-03-16T18:00:00.000Z",
  "rom": "lostspc"
}
```

### `current_scores`

Sent on each rendered frame when game state changes (score, ball, or player changes). Only emitted when an NVRAM map exists for the ROM.

```json
{
  "type": "current_scores",
  "timestamp": "2026-03-16T18:02:00.000Z",
  "rom": "lostspc",
  "players": 1,
  "current_player": 1,
  "current_ball": 2,
  "scores": [{ "player": "Player 1", "score": "12500000" }]
}
```

### `game_end`

Sent when game over is detected, either via NVRAM flag (primary) or ball-drop fallback. Includes final scores and game duration.

```json
{
  "type": "game_end",
  "timestamp": "2026-03-16T18:07:00.000Z",
  "rom": "lostspc",
  "reason": "game_over",
  "game_duration": 420,
  "players": 1,
  "scores": [{ "player": "Player 1", "score": "47250000" }]
}
```

`reason` is one of:

- `game_over` — detected via NVRAM flag or ball-drop
- `plugin_unload` — VPX was closed while a game was active

---

## NVRAM map coverage

Score extraction requires an NVRAM map for the ROM being played. Maps are provided by the bundled [pinmame-nvram-maps](https://github.com/tomlogic/pinmame-nvram-maps) submodule. Coverage spans hundreds of ROMs including most popular Williams, Bally, Stern, and Sega titles.

If no map exists for a ROM, `game_start` and `game_end` events are still emitted but `current_scores` will not be broadcast and scores in `game_end` will be empty. The VPX log will include:

```bash
No nvram map for ROM: <romname> — current_scores will not be emitted
```

---

## Building from source

Requires CMake 3.20+ and a C++20 compiler (MSVC on Windows, GCC/Clang on Linux/macOS).

```bash
# Clone with submodules (required for nvram-maps)
git clone --recurse-submodules https://github.com/emb417/vpc-score-plugin.git
cd vpc-score-plugin

# Windows x64
cmake -B build -A x64 -DBUILD_SHARED=ON -DPluginPlatform=windows -DPluginArch=x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Linux
cmake -B build -DBUILD_SHARED=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output is placed in `build/plugins/vpc-score-plugin/` alongside `plugin.cfg` and `nvram-maps`.

Releases are built automatically by GitHub Actions on version tags (`v*.*.*`) for both Windows and Linux, and attached to the GitHub Release as zip files.

---

## Project structure

```bash
vpc-score-plugin/
  plugins/
    vpc-score-plugin/
      include/plugins/     ← VPX plugin API headers
      vpc-score-plugin.cpp ← single-file plugin implementation
      plugin.cfg           ← plugin metadata and library names
  nvram-maps/              ← git submodule (tomlogic/pinmame-nvram-maps)
  CMakeLists.txt
  .github/workflows/build.yml
```
