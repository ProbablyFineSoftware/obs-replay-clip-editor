# Replay Clip Editor for OBS Studio

A replay trimmer inspired by Steam's Game Recording, built into OBS Studio. Grab
what just happened, trim it on a zoomable filmstrip timeline, choose which audio
tracks to keep, and export a clean clip without leaving OBS.

> **Status:** v1.0.0, Windows only (OBS Studio 31.x and 32.x). macOS and Linux
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

1. Download `obs-replay-clip-editor-<version>-windows-x64.zip` from the
   [Releases page](https://github.com/ProbablyFineSoftware/obs-replay-clip-editor/releases).
2. Close OBS Studio completely.
3. Unzip it over your OBS Studio install folder (typically
   `C:\Program Files\obs-studio`), merging the `obs-plugins` and `data` folders.
4. Start OBS. Replay Clip Editor appears in the menu bar.

The release is a portable `.zip`, with no installer. The binaries are not
code-signed, so if Windows SmartScreen or your antivirus flags the download,
choose Keep or allow.

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

## Releasing

Pushing a semantic-version tag (for example `1.0.0`) to `master` triggers the
GitHub Actions pipeline, which builds the plugin and drafts a GitHub Release with
the Windows package attached.

## License

Released under the GNU General Public License v2.0 or later. See [LICENSE](LICENSE).

Copyright (c) 2026 Terra Firma Entertainment.
