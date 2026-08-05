# Replay Clip Editor for OBS Studio

A replay trimmer inspired by Steam's Game Recording, built into OBS Studio. Grab
what just happened, trim it on a zoomable filmstrip timeline, choose which audio
tracks to keep, and export a clean clip without leaving OBS.

> **Status:** v1.0.1, Windows only (OBS Studio 31.x and 32.x). macOS and Linux
> are planned; see [Platform support](#platform-support).

![The Replay Clip Editor trim window, showing the filmstrip timeline with in/out trim handles, per-track audio toggles, and export settings.](docs/screenshot.png)

## Features

- **One-key capture:** a single hotkey saves the replay buffer and opens the
  result in the editor, ready to trim.
- **Clip browser:** browse and reopen any recording in your OBS recording folder,
  or point it at a different folder.
- **Frame-accurate trimming:** a custom FFmpeg preview engine with frame-by-frame
  stepping, an in/out range, and a scrubbable playhead.
- **Zoomable timeline:** scroll to zoom at the cursor, drag the zoom indicator to
  pan, and drag the in/out handles to set your range.
- **Per-track audio:** enable or disable individual audio tracks in the export
  (game, mic, desktop, however you record).
- **Export options:** reuse your OBS Recording or Streaming encoder settings for a
  one-click match, or set the encoder, rate control, bitrate/CQ, and container
  yourself.
- **Downscaling:** export at the source resolution or scale down. Only resolutions
  at or below the source are offered, so clips are never upscaled.
- **Size estimate:** shows the approximate output size before you export, and
  updates as you change encoder and resolution settings.

## Installation

### Windows

**Installer (recommended)**

1. Download `obs-replay-clip-editor-<version>-windows-x64.exe` from the
   [Releases page](https://github.com/ProbablyFineSoftware/obs-replay-clip-editor/releases).
2. Close OBS Studio completely.
3. Run the installer and follow the prompts. It asks for administrator rights so it
   can place the plugin in OBS's plugin folder. The build is not code-signed, so if
   Windows SmartScreen warns about an unknown publisher, click "More info" and then
   "Run anyway".
4. Start OBS. Replay Clip Editor appears in the menu bar.

To remove it later, use "Add or remove programs" in Windows Settings.

**Portable zip (no installer)**

1. Download `obs-replay-clip-editor-<version>-windows-x64.zip` instead.
2. Close OBS Studio completely.
3. In File Explorer, open `C:\ProgramData\obs-studio\plugins\`. Paste that path into
   the address bar, since `ProgramData` is hidden by default. Create the `plugins`
   folder if it is not there.
4. Copy the `obs-replay-clip-editor` folder from inside the zip into that `plugins`
   folder. The plugin file should end up at
   `C:\ProgramData\obs-studio\plugins\obs-replay-clip-editor\bin\64bit\obs-replay-clip-editor.dll`.
5. Start OBS. Replay Clip Editor appears in the menu bar.

### macOS / Linux

Not supported yet; see [Platform support](#platform-support) below.

## Usage

- **Open Clip Browser** (menu or hotkey): see your replays and double-click one to
  edit.
- **Open Clip Editor** (menu or hotkey): save the replay buffer right now and jump
  straight into trimming. The replay buffer must be enabled in
  **Settings > Output > Replay Buffer**. Tick **Start replay buffer with OBS** in
  the browser to keep it ready whenever you launch OBS.

Both actions have configurable hotkeys under **Settings > Hotkeys** ("Open Clip
Browser" and "Open Clip Editor").

### In the editor

| Action | Key |
|--------|-----|
| Play / Pause | `Space` |
| Step one frame | `Left` / `Right` |
| Set In / Out point | `I` / `O` |
| Jump to In / Out | `Home` / `End` |

## Building from source

This project uses the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
build system (CMake plus the platform scripts under `.github/`).

- **Windows:** Visual Studio 2022 and CMake 3.30, then run
  `.github/scripts/Build-Windows.ps1`, or configure with the bundled CMake preset.

Dependencies (OBS sources, obs-deps, Qt6) are pinned in `buildspec.json` and
fetched automatically by the build scripts.

macOS and Linux do not compile yet; see [Platform support](#platform-support).

## Platform support

The plugin is Windows-only for now. Almost everything is already portable. The
exception is the live preview, which binds an OBS display to the editor window
through the `gs_window.hwnd` handle that only exists on Windows. macOS (NSView)
and Linux (X11/Wayland) each need their own native-handle path before the plugin
can build there.

Both are on the roadmap. The CI jobs for them already exist but are turned off
(`if: false` in `.github/workflows/build-project.yaml`) until that code is written
and tested on real hardware.

## How this was built

For full disclosure, this plugin was vibe-coded. The entirety of the code was written by
Claude, working from my direction. I designed the feature set and the way the editor behaves, reviewed
the changes, and tested every build manually, but I did not hand-write any of the code here.

I'm putting this up front so anyone using, auditing, or contributing to the plugin
knows exactly how it was made and can weigh that for themselves.

## License

Released under the GNU General Public License v2.0 or later. See [LICENSE](LICENSE).

Copyright (c) 2026 Terra Firma Entertainment.
