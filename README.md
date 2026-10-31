# Rushr

An external game overlay for Windows. It runs as its own program: it reads player data from the game's memory, draws it on a see-through overlay, and can move your real mouse onto a player. Nothing is injected into the game.

> [!WARNING]
> Using this in an online game can break its terms of use and get your account banned. Use it at your own risk.

## Features

**Mouse lock**

- Moves your mouse onto the player closest to your cursor inside an adjustable circle, and stays on them until you let go of the key or they leave the server.
- Targets the head, the torso, or whichever of the two is closest to the cursor.
- Adjustable circle size, with the option to show it on screen.
- Adjustable speed, where 100 snaps instantly.

**Player overlay**

- Box, name and health bar for every other player, each with its own colour.

It treats every player the same, including teammates, friends, dead players and anyone behind a wall. The only things it skips are you, and body parts that aren't loaded or are off screen.

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
2. Start the game and join a server. The order doesn't matter, since it waits for the game and reconnects if the game restarts.
3. Press **Insert** to open or close the menu.

Everything starts off, so turn on what you want:

| Tab | Settings |
|---|---|
| First | Mouse lock: enable and key, Hitbone, FOV size and circle, Sensitivity |
| Second | Player overlay: enable, Box, Name, Healthbar |
| Third | Save, load and delete configs, hide the overlay from OBS and clips, Unload |

**Lock key:** right-click the key button to choose toggle, hold or always. With no key bound, the lock runs whenever it's enabled. While the menu is open, it only moves the mouse on a held key, so you can always reach the menu to turn it off.

Configs are saved in `%APPDATA%\1\configs` and logs in `%APPDATA%\1\logs`.

## Updating offsets

Game updates change the memory offsets, and the program stops reading the game correctly until they're updated.

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
| `Glory/src/features/targeting` | The mouse lock |
| `Glory/src/features/spotter` | The player overlay |
| `Glory/src/cache` | Keeps the player list up to date on a background thread |
| `Glory/src/game`, `sdk`, `memory` | Finds the game and reads its data |
| `Glory/src/render` | Overlay, menu, keybinds and configs |
| `Glory/src/logger` | Console and file logging |
| `Glory/src/Offsets` | The memory offsets |
| `Glory/ext` | Third-party libraries: Dear ImGui, FreeType, nlohmann/json, Clipper2 and stb |
