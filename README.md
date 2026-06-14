<p align="center">

&#x20; <img width="1920" height="1080" alt="ATLAS Banner" src="https://github.com/user-attachments/assets/23883b15-d35d-427b-93ac-d50e3aa2ea72" />

</p>

# ATLAS Console



ATLAS Console is a lightweight in-game overlay and client mod for OGFN private servers. It provides real-time game controls, visual configuration, and quality-of-life features through an injected DLL and an ImGui-based overlay.



## Features



### Overlay



- Toggle the overlay in-game with a configurable hotkey (default: F9).

- Hotkey is saved automatically and persists across sessions via `%APPDATA%\\ATLAS\\settings.json`.

- Clean, non-intrusive UI — hidden until summoned, blocks no gameplay when closed.



### Editing



- **Edit On Release (EOR)** — Automatically completes a building edit on selection.

- **Reset On Release (ROR)** — Automatically completes a building reset on selection, toggled independently from EOR.

- **Disable Pre-Edits** — Prevents edits from being initiated on pre-edited structures.



### Respawning



- **Force Respawns** — Enables infinite respawns across all playlists.

- **Custom Respawn Time** — Override the time between respawns (1–30 seconds).

- **Custom Respawn Height** — Override the height players respawn at in-air (1,000–50,000 units).



\### Console



- **Console Enabled** — Spawns the Unreal Engine in-game console for direct command input.

- **Potato Graphics** — Applies `r.MipMapLODBias 7` for maximum FPS; revert restores `r.MipMapLODBias 0`.

- **FOV Slider** — Set field of view from 0° to 175° via `fov <value>`.

- **Resolution Dropdown** — Switch between 1920×1080, 1720×1080 (stretched res), and 1280×720. Requires fullscreen.



### Hotkeys



- **Rebind GUI Key** — Reassign the overlay toggle to any F-key or navigation key.

- Binding is written to disk immediately and loaded on next inject.



## Project Layout



```text

ATLAS/

+-- Private/

|   +-- Client.cpp         Core hooks: EOR, ROR, pre-edit, respawn patching, cheat manager

|   +-- dllmain.cpp        DLL entry point, DX11 present hook, ImGui initialisation

|   +-- GUI.cpp            Overlay render loop, section layout, console command dispatch

|   +-- Finders.cpp        UObject finders and SDK helpers

+-- Public/

|   +-- Client.h           Client::Init declaration

|   +-- Configuration.h    Static configuration flags and values (FOV, respawn settings, etc.)

|   +-- GUI.h              FGUI state struct, GUI\_Init / GUI\_Render / GUI\_HandleInput declarations

|   +-- GUI\_Hotkey.h       Lightweight hotkey persistence (read/write %APPDATA%\\ATLAS\\settings.json)

|   +-- Utils.h            Hook helpers, memory patching utilities

|   +-- FortPlayerControllerAthena.h

|   +-- FortPlaylistAthena.h

|   +-- BuildingSMActor.h

+-- ImGui/

|   +-- imgui.h / imgui.cpp

|   +-- imgui\_impl\_dx11.h / imgui\_impl\_dx11.cpp

|   +-- imgui\_impl\_win32.h / imgui\_impl\_win32.cpp

+-- SDK/

|   +-- Engine.h           Unreal Engine SDK types and globals

|   +-- Includes.h         SDK include aggregator

```



## Build Requirements



- Windows 10 or newer.

- Visual Studio 2022 with the **Desktop development with C++** workload.

- Windows SDK 10.0 or newer.

- DirectX 11 SDK (included with the Windows SDK).

- [MinHook](https://github.com/TsudaKageyu/minhook) — included in project via `MinHook.h`.

- [Memcury](https://github.com/kem0x/Memcury) — included in SDK for pattern scanning and PE utilities.



## Building



1. Open `ATLAS.sln` in Visual Studio 2022.

2. Set the configuration to **Release / x64**.

3. Build → **Build Solution** (`Ctrl+Shift+B`).

4. The output DLL will be placed in `x64/Release/ATLAS.dll`.



> After modifying `pch.h`, always run **Build → Clean Solution** before rebuilding to force the precompiled header to regenerate.



## Injection



Inject `ATLAS.dll` into the running Fortnite process using any standard DLL injector (such as the bundled injector in ATLAS Link). The overlay will initialise silently on attach and become visible when you press the configured hotkey.
The DLL is provided via ATLAS Link. If you are using our launcher, this is the default DLL.



## Hotkey Persistence



The GUI toggle hotkey is saved to:



```

%APPDATA%\\ATLAS\\console.json

```



Example file:



```json

{
  "hotkey": 120
}

```



The value is a Windows Virtual Key code (e.g. `120` = F9). This file is created automatically on first bind and read on every inject.



## Version Compatibility



ATLAS Console targets multiple Fortnite builds and engine versions. Version-specific behaviour is gated throughout the codebase:



| Condition | Behaviour |

|---|---|

| `FortniteVersion >= 10` | Showdown/Arena UI extension patching |

| `FortniteVersion < 11` | SelectEdit hook enabled |

| `FortniteVersion < 15.20` | PerformBuildingEditInteraction hook enabled |

| `FortniteVersion < 24.30` | SelectReset hook and edit mode scanning enabled |

| `EngineVersion >= 5.0` | Runtime options patch, log suppression |

| `EngineVersion >= 5.1` | Encryption bypass |

| `EngineVersion >= 5.3` | Iris replication support |

| `EngineVersion >= 5.4` | Tactical sprint / hurdle / slide / clamber disable |



## Credits



- [TsudaKageyu](https://github.com/TsudaKageyu) — [MinHook](https://github.com/TsudaKageyu/minhook), used for function hooking.

- [kem0x](https://github.com/kem0x) — [Memcury](https://github.com/kem0x/Memcury), used for pattern scanning and PE utilities.

- [ocornut](https://github.com/ocornut) — [Dear ImGui](https://github.com/ocornut/imgui), used for the overlay UI.

