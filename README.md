# Rushr

A simple aimlock and ESP for Roblox on Windows. It runs as its own program: it reads the game's memory from outside, draws on a see-through overlay, and moves your real mouse. Nothing is injected into the game.

> [!WARNING]
> Using this breaks Roblox's Terms of Use and can get your account banned. Use it at your own risk.

## Features

**Aimlock**

- Locks onto the player closest to your cursor inside the FOV circle, and stays on them until you let go of the key or they leave the server.
- Aims at the head, the torso, or whichever of the two is closest to the cursor.
- Adjustable FOV size, with an optional circle on screen.
- Adjustable lock speed, where 100 snaps instantly.

**ESP**

- Box, name and health bar for every other player, each with its own colour.

There are no target checks. It aims at and draws everyone, including teammates, friends, dead players and anyone behind a wall. The only things it skips are you, and body parts that aren't loaded or are off screen.

## Requirements

- Windows 10 or 11, 64-bit
- [Visual Studio 2022](https://visualstudio.microsoft.com/) or its Build Tools, with the C++ desktop tools and a Windows SDK

## Build

```
build.bat
```

The exe ends up at `build\Glory.exe`. To choose a configuration or force a full rebuild, use `build.bat [Release|Debug] [rebuild]`.

## Usage

1. Run `build\Glory.exe`. It asks for administrator rights.
2. Start Roblox and join a game. The order doesn't matter, since it waits for Roblox and reconnects if Roblox restarts.
3. Press **Insert** to open or close the menu.

Everything starts off, so turn on what you want:

| Tab | Settings |
|---|---|
| **Aimbot** | Enable and key, Hitbone, FOV size and circle, Sensitivity |
| **Visual** | Enable, Box, Name, Healthbar |
| **Settings** | Save, load and delete configs, hide the overlay from OBS and clips, Unload |

**Aim key:** right-click the key button to choose toggle, hold or always. With no key bound, the aimlock runs whenever it's enabled. While the menu is open, it only moves the mouse on a held key, so you can always reach the menu to turn it off.

Configs are saved in `%APPDATA%\1\configs` and logs in `%APPDATA%\1\logs`.

## Updating offsets

Roblox updates change the memory offsets, and the program stops reading the game correctly until they're updated.

1. Copy the whole header from [offsets.imtheo.lol](https://offsets.imtheo.lol) into `offset.txt`.
2. Close the program if it's running.
3. Run `update_offsets.bat`.

It puts the new offsets in `Glory\src\Offsets\Offsets.hpp`, keeps the old header in `offset_backups\`, and rebuilds. If the pasted text isn't a whole header or is missing an offset the program uses, it changes nothing. Add `nobuild` to skip the rebuild.

## Branches

| Branch | Contents |
|---|---|
| `main` | This simple version |
| `full` | The earlier version with every other feature |

## Project layout

The project and exe are still named Glory.

| Path | Purpose |
|---|---|
| `Glory/src/features/targeting` | The aimlock |
| `Glory/src/features/spotter` | The ESP |
| `Glory/src/cache` | Keeps the player list up to date on a background thread |
| `Glory/src/game`, `sdk`, `memory` | Finds the game and reads its data |
| `Glory/src/render` | Overlay, menu, keybinds and configs |
| `Glory/src/logger` | Console and file logging |
| `Glory/src/Offsets` | The memory offsets |
| `Glory/ext` | Third-party libraries: Dear ImGui, FreeType, nlohmann/json, Clipper2 and stb |
