# Packaging

Build on the target operating system, with the dependencies listed in the
[main README](../README.md). Run these commands from the project directory.

## Linux AppImage

Additional tools: Python 3, `readelf`, `gst-inspect-1.0`, `pactl`, `qmake6`,
and PipeWire/SPA development files.

```sh
python3 scripts/package-appimage.py
chmod +x dist/StreamingApp-x86_64.AppImage
./dist/StreamingApp-x86_64.AppImage --check-runtime
```

The script downloads its packaging tools on first use, builds in Release,
and bundles Qt/FFmpeg and GStreamer. Use the matching artifact name for aarch64.
If FUSE is unavailable:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./dist/StreamingApp-x86_64.AppImage
```

Recipients still need desktop audio services, graphics drivers, and a
ScreenCast portal with PipeWire on Wayland. Check `dist/build-info.json`
for the required glibc version: an AppImage built on a newer distribution
may not run on an older one. Build on the oldest distribution you plan to support.

## Windows ZIP

Use Developer PowerShell for Visual Studio 2022, with Qt and GStreamer
MSVC x64 runtime/development kits. Adjust these paths:

```powershell
./scripts/package-windows.ps1 -QtDir 'C:\Qt\6.11.2\msvc2022_64' -GstDir 'C:\gstreamer\1.0\msvc_x86_64'
```

The script builds in Release, bundles Qt/FFmpeg, GStreamer, and the MSVC runtime,
and checks the bundled runtime before creating `dist/StreamingApp-windows-x64.zip`.
If the MSVC runtime is not found, pass `-VcRedistDir` with the directory containing
`x64/Microsoft.VC143.CRT`.

Recipients must extract the entire ZIP and open `streaming_app.exe`.
The executable alone is not enough. Build and validate this package on Windows.

## Share and verify

Send the AppImage or complete Windows ZIP, plus a room invitation. Recipients
paste the invitation into the app; they do not need source code, build tools,
or the server token. Run the [signaling server](server/README.md) on one
machine reachable by both clients.

Before sharing a package, test it on a clean target machine: startup, WSS
connection, joining a room, screen/audio capture, and playback.
`--check-runtime` checks dependencies, not desktop capture or network reachability.

Packages do not include your local settings or invitations. Preserve the
license notices and meet the redistribution requirements of bundled libraries.
