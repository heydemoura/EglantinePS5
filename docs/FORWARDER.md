# Starting a game from a home screen forwarder

A forwarder is a separate small app with its own home screen tile (its own title ID, icon and
name). When it is opened, it launches EglantinePS5 (`PPSA98515`) with launch arguments, and
EglantinePS5 starts that game directly instead of opening its launcher.

## Arguments

| Argument | Meaning |
| --- | --- |
| `--rom <file>` or `--rom=<file>` | The game to start. An absolute path (`/mnt/usb0/eden/roms/Game.nsp`), or a path inside the game files folder's `roms/` (`Game.nsp`, `rpg/Game.xci`). A relative path may not contain `..`. |
| `--exit-after-game` | When that game ends normally, close EglantinePS5 so the console returns to the home screen. Without it, the library opens. |

Unknown arguments are ignored. Parsing is in `headless/forwarded_launch.h`; `headless/main.cpp`
uses it after filesystem access is set up, so `roms/` resolves against the folder chosen in
**Settings > Game files**.

## Behaviour

- The forwarded game starts once. Quitting it (Touchpad + L1) opens the library, or, with
  `--exit-after-game`, ends EglantinePS5.
- If the file is missing (or filesystem access was not granted, so the path cannot be read), the
  launcher opens and shows `Forwarded game not found: <path>`.
- After a crash, EglantinePS5 restarts without arguments and shows the crash notice; the
  forwarded game is not started again.
- Arguments only reach a new EglantinePS5 process. If EglantinePS5 is already running, the
  system brings it to the front and `main` does not run again; a forwarder should close it
  first.
