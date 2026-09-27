# StreamingApp

A C++20 screen-sharing app for Linux and Windows, built with Qt and GStreamer.
Two people can join a room and take turns sharing their screen and system audio.

- H.264 video and Opus audio over WebRTC, encrypted with DTLS-SRTP.
- Selectable 30/60 FPS, 720p/1080p, and 1–50 Mbit/s target bitrate.
- Room invitations, optional system audio, volume control, and fullscreen video.

Video encoding uses the CPU. The app does not currently support recording,
HDR, lossless video, hardware encoding, or rooms with more than two people.

## Requirements

- C++20 compiler, CMake 3.21+, Ninja, and pkg-config/pkgconf.
- Qt 6.5+: Widgets, Multimedia, MultimediaWidgets, Network, and WebSockets;
  also DBus on Linux. Screen capture requires Qt Multimedia's FFmpeg backend.
- GStreamer 1.20+ development libraries: core, app, video, WebRTC, and SDP.
- GStreamer runtime plugins: base, good, bad, ugly, libav, and libnice
  (including H.264, Opus, RTP, WebRTC, and DTLS/SRTP).
- Linux audio: PulseAudio or PipeWire-Pulse, the GStreamer PulseAudio plugin,
  and `pactl`. Wayland capture also needs PipeWire and a working ScreenCast
  desktop portal. Linux portal tests require `dbus-run-session`.
- Windows 10+: Qt and GStreamer MSVC x64 kits, including GStreamer runtime,
  development files, and WASAPI2 plugins.

## Build on Linux

From the project directory, with the dependencies installed:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/streaming_app --check-runtime
./build/streaming_app
```

If CMake cannot find Qt, add `-DCMAKE_PREFIX_PATH=/path/to/Qt/kit`.
Run the client from your desktop session, without `sudo`.

## Build on Windows

Use Developer PowerShell for Visual Studio 2022. Adjust the kit paths:

```powershell
$qtKit = "C:/Qt/6.11.2/msvc2022_64"
$gstKit = "C:/gstreamer/1.0/msvc_x86_64"
$env:PATH = "$qtKit/bin;$gstKit/bin;$env:PATH"
$env:PKG_CONFIG_PATH = "$gstKit/lib/pkgconfig"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qtKit"
cmake --build build
& "$qtKit/bin/windeployqt.exe" --release build/streaming_app.exe
./build/streaming_app.exe --check-runtime
./build/streaming_app.exe
```

These commands use the installed development runtime. For a portable package,
see the [packaging guide](packaging/DISTRIBUTION.md). Windows must be built
and tested on Windows.

## Share a screen

1. Start a server using the [server setup guide](packaging/server/README.md).
2. In the client settings, enter the public server URL and server token.
3. Create a room, copy its invitation, and send it to the other person.
4. The other person pastes the invitation into the app and joins.
5. Choose a screen, quality settings, and whether to share system audio.
   Either participant can start sharing; stop before switching presenters.

Leave the TURN field empty to use the server's automatic configuration.
Double-click the video for fullscreen; press Esc to exit.

Choose quality settings before joining. The session uses the lower limits
selected by the two participants; leave and rejoin to change them.
FPS is a processing limit, so actual frame rate also depends on capture and CPU.
Invitations expire when the room empties or the server restarts.

## Local test

For two client instances on the same computer:

```sh
(umask 077; openssl rand -hex 32 > /tmp/streaming-token)
./build/signaling_server --insecure-local --port 8443 --token-file /tmp/streaming-token
```

Use `ws://127.0.0.1:8443` and the token file's contents in the first client,
then create a room and paste its invitation into the second client.
For different computers, use WSS as described in the server guide.

## Tests and packages

```sh
ctest --test-dir build --output-on-failure
```

Network tests require local sockets and a non-loopback IPv4 interface.
TURN integration tests also require coturn.

See the [packaging guide](packaging/DISTRIBUTION.md) to create a Linux AppImage
or a Windows ZIP for someone else.
