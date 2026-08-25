<p align="center">

&#x20; <img width="1920" height="1080" alt="ATLAS Banner" src="https://github.com/user-attachments/assets/23883b15-d35d-427b-93ac-d50e3aa2ea72" />

</p>

# ATLAS Console



ATLAS Console is a lightweight in-game overlay and client mod for OGFN private servers. It provides real-time game controls, visual configuration, and quality-of-life features through an injected DLL and an ImGui-based overlay.



## Features



### Overlay



- Toggle the overlay in-game with a configurable hotkey (default: F9).

- Hotkeys, console preferences, persistent toggle states, commands, and macros are saved automatically in `%APPDATA%\\ATLAS\\console.json`.

- Clean, non-intrusive UI — hidden until summoned, blocks no gameplay when closed.



### Editing



- **Edit On Release (EOR)** — Automatically completes a building edit on selection.

- **Reset On Release (ROR)** — Automatically completes a building reset on selection, toggled independently from EOR.

- **Disable Pre-Edits** — Prevents edits from being initiated on pre-edited structures.



### Respawning



- **Force Respawns** — Enables infinite respawns across all playlists.

- **Custom Respawn Time** — Override the time between respawns (1–30 seconds).

- **Custom Respawn Height** — Override the height players respawn at in-air (1,000–50,000 units).



### Console



- **Enhanced UE-Style Console** — F8 or Unreal's normal backtick/tilde key cycles from a compact command field at the bottom to a full-screen, bottom-anchored console, then closes it. When ATLAS Console and the default-on Slash Key preference are selected, pressing `/` during gameplay opens the compact console with `/` already entered; Original UE Console receives the key normally. Escape clears non-empty console text first, then closes the console on the next press. Running a command from the compact field closes it automatically. Recent lines grow upward from the bottom with native-style command spacing and white text, and the bar includes a compact log-clear control. Up/Down shows and navigates a compact monochrome history list.

- **History and Completion** — Use Up/Down to browse prior commands and Tab to complete from recent or saved commands.

- **Slash Commands** — `/give`, `/suicide`, and `/startaircraft` are dispatched as `cheat give`, `cheat suicide`, and `cheat startaircraft`.

- **Bounded Output Capture** — Native UE4 console responses and asynchronous ClientMessage/cheat replies are copied into a capped queue so output bursts cannot grow memory without limit.

- **Game-Thread Command Dispatch** — Console, hotkey, and macro commands are handed through a one-command mailbox and executed from Unreal's game-thread tick. Rapid binds cannot re-enter ProcessEvent or stall the Present/render mutex.

- **Balanced Mouse Hotkeys** — Mixed raw-mouse packets are routed as a complete packet, keeping every forwarded button-down paired with its release even when MOUSE4/MOUSE5 is assigned to an ATLAS command.

- **Preferences** — The Config tab switches between ATLAS Console and Original UE Console and can enable or disable the Slash Key shortcut. Save Toggle Preferences controls whether managed toggles restore saved choices or start from defaults each session. All three preferences are enabled by default.

  Close the active console before changing backends so Unreal can finish its own open/close state transition.

- **Mobile Builds** — Toggles static-mesh LOD distance scaling between `r.StaticMeshLODDistanceScale 1000` and Unreal's normal value of `1`.

- **Gameplay Fog** — Toggles gameplay fog with `r.Fog 1` and `r.Fog 0`; enabled by default.

- **Potato Graphics** — Adjusts `r.MipMapLODBias` from 0–15 with a live slider. Right-click the slider to type an exact clamped value.

- **FOV Slider** — Set field of view from 1° to 175° via `fov <value>`. Right-click the slider to type an exact clamped value.

- **Other Tools** — Toggle target information on any supported build, plus the debug camera through Fortnite 10.40.

- **Resolution Dropdown** — Switch between 1920×1080, 1720×1080 (stretched res), and 1280×720. Requires fullscreen.



### Hotkeys



- **Rebind Menu Key** — Reassign the menu toggle (default: F9).

- **Console Keys** — Uses F8 and Unreal's native backtick/tilde binding for the three-state console cycle; `/` opens the ATLAS command field only when ATLAS Console and Slash Key are enabled.

- **Saved Toggles** — With Save Toggle Preferences enabled, editing, graphics, movement, respawn, and Slash Key preferences are written immediately and restored on the next inject. When disabled, those settings use defaults on each inject and changes last only for the current session. Debug Camera and Target Info are always session-only.

- **Reset All** — Restores console style, hotkeys, toggle preferences, graphics sliders, FOV, host fields, commands, and macros to their defaults and reapplies command-backed defaults in game.

- **Editable Saved Commands** — Double-click a saved command, macro step, or the wait time between macro steps to edit it inline. Enter or clicking away saves a valid edit; Escape restores the original value.

- **Rebind Join Key** — Reassign quick Join (default: F5).

- Binding is written to disk immediately and loaded on next inject.



## Project Layout



```text

ATLAS/

+-- Private/

|   +-- Client.cpp         Core hooks, cheat-manager lifecycle, and bounded ClientMessage capture

|   +-- dllmain.cpp        DLL entry point, DX11/DX12 hooks, and synchronized ImGui input/rendering

|   +-- GUI.cpp            Overlay render loop, section layout, console command dispatch

|   +-- Finders.cpp        UObject finders and SDK helpers

+-- Public/

|   +-- Client.h           Client initialization and console-capture configuration

|   +-- Configuration.h    Static configuration flags and values (FOV, respawn settings, etc.)

|   +-- GUI.h              FGUI state struct, GUI_Init / GUI_Render / GUI_HandleInput declarations

|   +-- Hotkey.h           Hotkey/command persistence (read/write %APPDATA%\\ATLAS\\console.json)

|   +-- Utils.h            Hook helpers, memory patching utilities

|   +-- FortPlayerControllerAthena.h

|   +-- FortPlaylistAthena.h

|   +-- BuildingSMActor.h

+-- ImGui/

|   +-- imgui.h / imgui.cpp

|   +-- imgui_impl_dx11.h / imgui_impl_dx11.cpp

|   +-- imgui_impl_win32.h / imgui_impl_win32.cpp

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

> ATLAS remains loaded for the process lifetime because its render and client hooks are not safe to unload live. Exit the game to unload it cleanly.



> After modifying `pch.h`, always run **Build → Clean Solution** before rebuilding to force the precompiled header to regenerate.



## Injection



Inject `ATLAS.dll` into the running Fortnite process using any standard DLL injector (such as the bundled injector in ATLAS Link). The overlay will initialise silently on attach and become visible when you press the configured hotkey.
The DLL is provided via ATLAS Link. If you are using our launcher, this is the default DLL.



## Settings Persistence



The console mode, toggle preferences, menu and Join hotkeys, command binds, and macros are saved to:



```

%APPDATA%\\ATLAS\\console.json

```



Example file:



```json

{
  "hotkey": 120,
  "joinHotkey": 116,
  "consoleMode": 0,
  "preferences": {
    "saveTogglePreferences": true,
    "slashKey": true,
    "editOnRelease": true,
    "resetOnRelease": true,
    "disablePreEdits": true,
    "mobileBuilds": false,
    "inGameFog": true,
    "sprintByDefault": true,
    "respawnsEnabled": true
  },
  "commands": [],
  "macros": []
}

```



Hotkeys use Windows Virtual Key codes (`120` = F9 and `116` = F5). `consoleMode` is `0` for ATLAS Console and `1` for Original UE Console. Missing preference fields use ATLAS defaults, while version gates decide where legacy-only features are available. When `saveTogglePreferences` is `false`, ATLAS remembers that master choice but ignores the remaining saved toggle values and applies defaults for the session. The file is created automatically and read on every inject.



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

- [Plooshi](https://github.com/plooshi) - [ErbiumClient](https://github.com/plooshi/Erbium/tree/main/ErbiumClient), the core/base of the console.

- [TsudaKageyu](https://github.com/TsudaKageyu) — [MinHook](https://github.com/TsudaKageyu/minhook), used for function hooking.

- [kem0x](https://github.com/kem0x) — [Memcury](https://github.com/kem0x/Memcury), used for pattern scanning and PE utilities.

- [ocornut](https://github.com/ocornut) — [Dear ImGui](https://github.com/ocornut/imgui), used for the overlay UI.

