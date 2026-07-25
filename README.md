# Replay Clip Editor for OBS Studio

A Steam-style replay trimmer built into OBS Studio. Grab what just happened, trim
it on a zoomable filmstrip timeline, pick which audio tracks to keep, and export a
clean clip — without leaving OBS or opening a separate video editor.

> **Status:** v1.0.0 · Works with OBS Studio **31.x and 32.x** on Windows.
> (macOS and Linux build from source via the same CMake project; see below.)

<!-- TODO: add a screenshot or short GIF of the editor here before publishing.
     e.g. ![Replay Clip Editor](docs/screenshot.png) -->

## Features

- **Clip That** — one hotkey saves the replay buffer and drops the result straight
  into the editor, ready to trim.
- **Clip browser** — browse and reopen any recording in your OBS recording folder
  (or point it at a different folder).
- **Frame-accurate trimming** — a custom FFmpeg preview engine with frame-by-frame
  stepping, an in/out trim range, and a playhead you can scrub.
- **Zoomable filmstrip timeline** — wheel to zoom at the cursor, drag the zoom
  indicator to pan, drag the in/out handles to set your range.
- **Per-track audio** — enable or disable individual audio tracks in the export
  (game / mic / desktop, however you record).
- **Flexible export** — reuse your **OBS Recording** or **OBS Streaming** encoder
  settings for a one-click match, or go **Custom** (encoder, rate control,
  bitrate/CQ, container).
- **Smart downscaling** — export at source resolution or downscale up to 4K, with
  only resolutions **at or below the source** offered so you never upscale.
- **Size estimate** and an overwrite prompt (replace, or auto-number the file).

## Installation

### Windows (recommended)

1. Download the latest release from the
   [Releases page](https://github.com/ProbablyFineSoftware/obs-replay-clip-editor/releases).
2. **Close OBS Studio completely.**
3. Use either package:
   - **Installer (`.exe`)** — run it and it places the files for you, **or**
   - **Portable (`.zip`)** — unzip it over your OBS Studio install folder
     (typically `C:\Program Files\obs-studio`), merging the `obs-plugins` and
     `data` folders.
4. Start OBS. You'll find **Replay Clip Editor** in the menu bar.

> The release binaries are not code-signed, so Windows SmartScreen or your
> antivirus may warn on the installer. The portable `.zip` avoids this — unzip and
> go.

### macOS / Linux

Prebuilt packages are produced by CI where available on the
[Releases page](https://github.com/ProbablyFineSoftware/obs-replay-clip-editor/releases).
Otherwise, build from source (below).

## Usage

- **Open Clip Browser** (menu or hotkey) — see your replays, double-click one to
  edit.
- **Open Clip Editor / Clip That** (menu or hotkey) — save the replay buffer *right
  now* and jump straight into trimming.
  - The replay buffer must be enabled in **Settings → Output → Replay Buffer**.
    Tick **Start replay buffer with OBS** in the browser to have it always ready.

Both actions have configurable hotkeys under **Settings → Hotkeys**
("Open Clip Browser" and "Open Clip Editor").

### In the editor

| Action | Key |
|--------|-----|
| Play / Pause | `Space` |
| Step one frame | `←` / `→` |
| Set In / Out point | `I` / `O` |
| Jump to In / Out | `Home` / `End` |

## Building from source

This project uses the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
build system (CMake + platform scripts under `.github/`).

- **Windows:** Visual Studio 2022 + CMake 3.30, then run
  `.github/scripts/Build-Windows.ps1` or configure with the bundled CMake preset.
- **macOS:** Xcode 16 + CMake 3.30 → `.github/scripts/build-macos`.
- **Ubuntu 24.04:** `cmake`, `ninja-build`, `pkg-config`, `build-essential` →
  `.github/scripts/build-ubuntu`.

Dependencies (OBS sources, obs-deps, Qt6) are pinned in `buildspec.json` and
fetched automatically by the build scripts.

## Releasing

Pushing a semantic-version tag (e.g. `1.0.0`) to `main` triggers the GitHub
Actions pipeline, which builds all platforms and drafts a GitHub Release with
installer packages attached.

## License

Released under the **GNU General Public License v2.0 or later**. See
[LICENSE](LICENSE).

Copyright © 2026 Terra Firma Entertainment.
