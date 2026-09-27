#!/usr/bin/env python3
"""Build a Linux client AppImage from this machine's Qt/GStreamer installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ELEMENTS = "appsrc appsink videoconvert x264enc h264parse avdec_h264 webrtcbin nicesrc nicesink rtpbin rtph264pay rtph264depay dtlssrtpenc dtlssrtpdec srtpenc srtpdec opusenc opusdec rtpopuspay rtpopusdepay audioconvert audioresample volume valve pulsesrc pulsesink audiotestsrc fakesink".split()


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def output(args, **kwargs):
    return run(args, stdout=subprocess.PIPE, text=True, **kwargs).stdout.strip()


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--tools-dir", type=Path, default=ROOT / ".local/appimage-tools")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("AppImage must be built on Linux; use package-windows.ps1 on Windows")
    arch = platform.machine()
    if arch not in ("x86_64", "aarch64"):
        parser.error("supported architectures: x86_64, aarch64")
    build = args.build_dir.resolve()
    if not args.skip_build:
        run(["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release"])
        run(["cmake", "--build", build, "--target", "streaming_app", "--parallel", "4"])
    tools = args.tools_dir.resolve()
    tools.mkdir(parents=True, exist_ok=True)
    tool_hashes = {}
    for name in ("linuxdeploy",):
        target = tools / f"{name}-{arch}.AppImage"
        url = f"https://github.com/linuxdeploy/{name}/releases/download/continuous/{target.name}"
        if not target.exists():
            temporary = target.with_suffix(".download")
            print(f"Downloading {url}", flush=True)
            urllib.request.urlretrieve(url, temporary)
            temporary.replace(target)
        target.chmod(0o755)
        tool_hashes[target.name] = hashlib.sha256(target.read_bytes()).hexdigest()
    runtime = tools / f"runtime-{arch}"
    if not runtime.exists():
        url = f"https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-{arch}"
        temporary = runtime.with_suffix(".download")
        urllib.request.urlretrieve(url, temporary)
        temporary.replace(runtime)
    tool_hashes[runtime.name] = hashlib.sha256(runtime.read_bytes()).hexdigest()
    appdir = ROOT / "build-appimage/AppDir"
    marker = appdir / ".streaming-appdir"
    if appdir.exists():
        if not marker.exists() or appdir.is_symlink():
            raise RuntimeError(f"Refusing to replace unrecognized directory: {appdir}")
        shutil.rmtree(appdir)
    appdir.mkdir(parents=True)
    marker.touch()
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(LC_ALL="C", APPIMAGE_EXTRACT_AND_RUN="1", NO_STRIP="1")
    env["GST_PLUGIN_PATH_1_0"] = str(ROOT / ".local/gstreamer-1.0") + os.pathsep + env.get("GST_PLUGIN_PATH_1_0", "")
    env["QMAKE"] = env.get("QMAKE") or shutil.which("qmake6") or shutil.which("qmake") or "qmake6"
    env["PATH"] = str(tools) + os.pathsep + env["PATH"]
    plugins = set()
    for element in ELEMENTS:
        details = output(["gst-inspect-1.0", element], env=env)
        match = re.search(r"^\s*Filename\s+(.+)$", details, re.MULTILINE)
        if not match:
            raise RuntimeError(f"Cannot locate plugin for {element}")
        plugins.add(Path(match[1].strip()))
    for plugin in sorted(plugins):
        copy(plugin, appdir / "usr/lib/gstreamer-1.0" / plugin.name)
    scanner_dir = output(["pkg-config", "--variable=pluginscannerdir", "gstreamer-1.0"])
    copy(Path(scanner_dir) / "gst-plugin-scanner", appdir / "usr/libexec/gstreamer-1.0/gst-plugin-scanner")
    qt_plugins = Path(output([env["QMAKE"], "-query", "QT_INSTALL_PLUGINS"]))
    # Explicitly include dynamically loaded multimedia, TLS and Wayland plugins.
    for group in ("multimedia", "tls", "networkinformation", "xcbglintegrations", "wayland-shell-integration", "wayland-decoration-client", "wayland-graphics-integration-client"):
        if (qt_plugins / group).exists():
            shutil.copytree(qt_plugins / group, appdir / "usr/plugins" / group, dirs_exist_ok=True)
    platforms = [p.name for p in (qt_plugins / "platforms").glob("*wayland*.so")]
    platforms += ["libqoffscreen.so"]
    platforms += ["libqxcb.so"]
    for name in platforms:
        copy(qt_plugins / "platforms" / name, appdir / "usr/plugins/platforms" / name)
    for group, names in {
        "platforminputcontexts": ["libcomposeplatforminputcontextplugin.so", "libibusplatforminputcontextplugin.so"],
        "imageformats": ["libqjpeg.so", "libqsvg.so", "libqico.so"],
    }.items():
        for name in names:
            if (qt_plugins / group / name).exists():
                copy(qt_plugins / group / name, appdir / "usr/plugins" / group / name)
    (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/bin/qt.conf").write_text("[Paths]\nPrefix=..\nPlugins=plugins\n")
    # Qt loads PipeWire with dlopen: ldd alone cannot discover these files.
    pw_lib = Path(output(["pkg-config", "--variable=libdir", "libpipewire-0.3"]))
    pw_modules = Path(output(["pkg-config", "--variable=moduledir", "libpipewire-0.3"]))
    spa = Path(output(["pkg-config", "--variable=plugindir", "libspa-0.2"]))
    copy(pw_lib / "libpipewire-0.3.so.0", appdir / "usr/lib/libpipewire-0.3.so.0")
    for name in ("rt", "protocol-native", "client-node", "client-device", "adapter", "metadata", "session-manager"):
        copy(pw_modules / f"libpipewire-module-{name}.so", appdir / "usr/lib/pipewire-0.3" / f"libpipewire-module-{name}.so")
    for group in ("support", "audioconvert", "videoconvert"):
        shutil.copytree(spa / group, appdir / "usr/lib/spa-0.2" / group)
    pw_prefix = Path(output(["pkg-config", "--variable=prefix", "libpipewire-0.3"]))
    copy(pw_prefix / "share/pipewire/client.conf", appdir / "usr/share/pipewire/client.conf")
    deploy = tools / f"linuxdeploy-{arch}.AppImage"
    command = [deploy, "--appdir", appdir, "--executable", build / "streaming_app",
               "--executable", shutil.which("pactl"), "--executable", shutil.which("gst-inspect-1.0"),
               "--desktop-file", ROOT / "packaging/linux/streaming-app.desktop",
               "--icon-file", ROOT / "packaging/linux/streaming-app.svg",
               "--custom-apprun", ROOT / "packaging/linux/AppRun",
               "--deploy-deps-only", appdir / "usr"]
    run(command, env=env)
    copy(ROOT / "README.md", appdir / "usr/share/doc/streaming-app/README.md")
    copy(ROOT / "packaging/DISTRIBUTION.md", appdir / "usr/share/doc/streaming-app/DISTRIBUTION.md")
    report = {"architecture": arch, "host_glibc": output(["getconf", "GNU_LIBC_VERSION"]),
              "qt": output([env["QMAKE"], "-query", "QT_VERSION"]),
              "gstreamer": output(["pkg-config", "--modversion", "gstreamer-1.0"]),
              "tools_sha256": tool_hashes, "gstreamer_plugins": [str(p) for p in sorted(plugins)]}
    # Determine the actual glibc requirement of every ELF in the bundle.
    versions = set()
    for path in (appdir / "usr").rglob("*"):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF": continue
            data = output(["readelf", "--version-info", path])
            versions.update(re.findall(r"Name: GLIBC_(\d+(?:\.\d+)+)", data))
    report["minimum_glibc"] = max(versions, key=lambda v: tuple(map(int, v.split('.'))))
    # Copy distro license notices for the source binaries when available.
    if shutil.which("pacman"):
        owners = set()
        for path in [build / "streaming_app", *plugins, *(pw_modules.glob("*.so"))]:
            result = subprocess.run(["pacman", "-Qqo", str(path)], capture_output=True, text=True)
            if result.returncode == 0: owners.update(result.stdout.splitlines())
        for owner in sorted(owners):
            directory = Path("/usr/share/licenses") / owner
            if directory.is_dir(): shutil.copytree(directory, appdir / "usr/share/licenses" / owner, dirs_exist_ok=True)
    (appdir / "usr/share/doc/streaming-app/build-info.json").write_text(json.dumps(report, indent=2) + "\n")
    target = dist / f"StreamingApp-{arch}.AppImage"
    env["LDAI_OUTPUT"] = str(target)
    env["LDAI_RUNTIME_FILE"] = str(runtime)
    run([deploy, "--appdir", appdir, "--custom-apprun", ROOT / "packaging/linux/AppRun", "--output", "appimage"], env=env, cwd=dist)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    target.with_suffix(".AppImage.sha256").write_text(f"{digest}  {target.name}\n")
    (dist / "build-info.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Created {target}\nRequires glibc >= {report['minimum_glibc']}; validate on the destination OS.")


if __name__ == "__main__":
    try:
        main()
    except (subprocess.CalledProcessError, OSError, RuntimeError) as error:
        sys.exit(str(error))
