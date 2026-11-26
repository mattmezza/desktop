"""Exercise the installed command boundaries in a private X11 session."""
import ctypes as C
import json
import os
from pathlib import Path
import struct
import subprocess as sp
import sys
import tempfile
import time
import zlib

BIN = str(Path(sys.argv[1]).resolve())
assert os.environ.get("DISPLAY"), "Run through xvfb-run."

def run(*args, check=True, **kwargs):
    return sp.run(args, text=True, capture_output=True, timeout=20, check=check, **kwargs)

def desktop(*args, **kwargs):
    return run(BIN, *map(str, args), **kwargs)

def status():
    data = json.loads(desktop("status", "--json").stdout)
    assert data["schema"] == 1
    return data

def until(test, seconds=5):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = test()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(f"condition did not become true: {test}")

def png(path, w, h, colors):
    def chunk(kind, data):
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
    left, right = (bytes(color) for color in colors)
    row = b"\0" + left * (w // 2) + right * (w - w // 2)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack("!IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(row * h)) + chunk(b"IEND", b""))

class X:
    def __init__(self):
        lib = C.CDLL("libX11.so.6")
        def bind(name, result, *args):
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = result, args
            return fn
        P, U, I = C.c_void_p, C.c_ulong, C.c_int
        self.d = bind("XOpenDisplay", P, C.c_char_p)(None)
        assert self.d
        self.root = bind("XDefaultRootWindow", U, P)(self.d)
        self.image = bind("XGetImage", P, P, U, I, I, C.c_uint, C.c_uint, U, I)
        self.pixel = bind("XGetPixel", U, P, I, I)
        self.destroy = bind("XDestroyImage", I, P)
        self.close = bind("XCloseDisplay", I, P)
        self.make = bind("XCreateSimpleWindow", U, P, U, I, I, C.c_uint, C.c_uint, C.c_uint, U, U)
        self.map = bind("XMapRaised", I, P, U)
        self.unmap = bind("XUnmapWindow", I, P, U)
        self.move = bind("XMoveWindow", I, P, U, I, I)
        self.sync = bind("XSync", I, P, I)
        self.select = bind("XSelectInput", I, P, U, C.c_long)
        self.pending = bind("XCheckTypedWindowEvent", I, P, U, I, P)

    def read(self, x, y):
        data = self.image(self.d, self.root, x, y, 1, 1, -1, 2)
        assert data
        value = self.pixel(data, 0, 0) & 0xFFFFFF
        self.destroy(data)
        return value

    def app(self):
        w = self.make(self.d, self.root, 16, 24, 180, 90, 0, 0, 0)
        self.map(self.d, w)
        self.sync(self.d, 0)
        return w

with tempfile.TemporaryDirectory(prefix="desktop-check-") as name:
    tmp = Path(name)
    config = tmp / "config/desktop"
    config.mkdir(parents=True)
    for key, value in (("CONFIG_HOME", config.parent), ("STATE_HOME", tmp / "state"),
                       ("RUNTIME_DIR", tmp / "run")):
        os.environ[f"XDG_{key}"] = str(value)
    (tmp / "run").mkdir(mode=0o700)
    wallpapers = tmp / "pictures"
    wallpapers.mkdir()
    first, second, third = (wallpapers / f for f in ("Alpha.png", "Béta wallpaper.png", 'Third "shot".png'))
    small = wallpapers / "Tiny.png"
    png(first, 1600, 1000, ((236, 28, 62), (25, 55, 210)))
    png(second, 1600, 1000, ((46, 161, 83), (30, 69, 44)))
    png(third, 1600, 1000, ((72, 33, 126), (25, 85, 150)))
    png(small, 320, 200, ((255, 255, 30), (90, 15, 20)))
    cfg = tmp / "dotfiles-config.ini"
    cfg.write_text(desktop("config", "defaults").stdout + "\n# stowed marker\n")
    (config / "config.ini").symlink_to(cfg)
    desktop("config", "set", "library.directory", wallpapers)
    desktop("config", "set", "rotation.mode", "manual")
    desktop("config", "set", "layout.crossfade_ms", "60")
    assert (config / "config.ini").is_symlink() and "# stowed marker" in cfg.read_text()
    assert desktop("config", "set", "ui.corner_radius", 61, check=False).returncode != 0
    run("xset", "s", "off")
    wm = sp.Popen([os.environ["TEST_WM"]], stdout=sp.DEVNULL, stderr=sp.DEVNULL) if os.environ.get("TEST_WM") else None
    x = X()
    ui = None
    try:
        desktop("set", first)
        until(lambda: x.read(60, 600) == 0xEC1C3E and x.read(1300, 600) == 0x1937D2)
        assert "_XROOTPMAP_ID(PIXMAP)" in run("xprop", "-root", "_XROOTPMAP_ID").stdout
        snapshot = status()
        desktop("reapply")
        assert status()["changed_at"] == snapshot["changed_at"]
        run("xrandr", "--setmonitor", "Wide", "1440/380x480/127+0+0", "screen")
        run("xrandr", "--setmonitor", "Laptop", "900/238x420/111+120+480", "none")
        desktop("reapply")
        docked = status()
        assert {o["name"] for o in docked["outputs"]} == {"Wide", "Laptop"}
        assert docked["wallpaper"] == str(first) and docked["changed_at"] == snapshot["changed_at"]
        until(lambda: x.read(1300, 700) == 0x0D0E0F)
        if not wm:
            def output(name):
                return next(o for o in status()["outputs"] if o["name"] == name)

            run("xdotool", "mousemove", "570", "690")
            until(lambda: x.read(565, 650) == 0xEC1C3E)
            until(lambda: output("Wide")["parallax_speed"] > .9 and output("Laptop")["parallax_speed"] > .9)
            window = x.app()
            until(lambda: output("Wide")["occupied"] and output("Wide")["parallax_speed"] < .03)
            assert not output("Laptop")["occupied"] and output("Laptop")["parallax_speed"] > .9
            run("xdotool", "mousemove", "950", "690")
            until(lambda: x.read(565, 650) == 0x1937D2)
            x.move(x.d, window, 180, 525)
            x.sync(x.d, 0)
            until(lambda: output("Laptop")["occupied"] and output("Laptop")["parallax_speed"] < .03)
            until(lambda: x.read(565, 650) == 0xEC1C3E)
            x.unmap(x.d, window)
            x.sync(x.d, 0)
            until(lambda: x.read(565, 650) == 0x1937D2)
        run("xrandr", "--delmonitor", "Laptop")
        run("xrandr", "--delmonitor", "Wide")
        desktop("reapply")
        assert status()["outputs"][0]["width"] == 1440
        until(lambda: x.read(1300, 700) == 0x1937D2)
        listing = json.loads(desktop("list", "--json").stdout)["wallpapers"]
        assert len(listing) == 4
        assert next(item for item in listing if item["path"] == str(third))["eligible"]
        assert "Too small" in next(item for item in listing if item["path"] == str(small))["reason"]
        assert desktop("set", small, check=False).returncode != 0
        assert status()["wallpaper"] == str(first)
        seen = {str(first)}
        for _ in range(2):
            desktop("next")
            path = status()["wallpaper"]
            assert path not in seen
            seen.add(path)
        desktop("pause")
        assert status()["paused"]
        desktop("stop")
        assert status()["paused"]
        desktop("resume")
        desktop("login")
        assert not status()["paused"]

        until(lambda: status()["parallax_speed"] > .9)
        if not wm:
            x.select(x.d, x.root, 4)  # ButtonPressMask on the root.
            x.sync(x.d, 0)
            run("xdotool", "mousemove", "98", "700", "click", "3")
            event = C.create_string_buffer(192)
            until(lambda: x.pending(x.d, x.root, 4, event))
        window = x.app()
        until(lambda: status()["occupied"])
        until(lambda: status()["parallax_speed"] < .03)
        x.unmap(x.d, window)
        x.sync(x.d, 0)
        until(lambda: status()["parallax_speed"] > .9)

        video = wallpapers / "Motion.mp4"
        run("ffmpeg", "-v", "error", "-threads", "1", "-f", "lavfi", "-i",
            "testsrc2=size=1536x960:rate=12", "-t", "1", "-c:v", "mpeg4", "-threads", "1", str(video))
        desktop("reload")
        desktop("set", video)
        until(lambda: status()["kind"] == "video" and status()["video_speed"] > .9)
        p = x.read(300, 400)
        until(lambda: x.read(300, 400) != p)
        x.map(x.d, window)
        x.sync(x.d, 0)
        until(lambda: status()["occupied"] and .15 < status()["video_speed"] < .85)
        until(lambda: status()["occupied"] and status()["video_speed"] < .01)
        p = x.read(300, 400)
        time.sleep(.35)
        assert x.read(300, 400) == p
        if not wm:
            x.unmap(x.d, window)
            x.sync(x.d, 0)
            run("xrandr", "--setmonitor", "Wide", "1440/380x480/127+0+0", "screen")
            run("xrandr", "--setmonitor", "Laptop", "900/238x420/111+120+480", "none")
            desktop("reapply")
            until(lambda: output("Wide")["video_speed"] > .99 and output("Laptop")["video_speed"] > .99)
            x.map(x.d, window)
            x.sync(x.d, 0)
            until(lambda: output("Wide")["occupied"] and .15 < output("Wide")["video_speed"] < .85)
            until(lambda: output("Wide")["video_speed"] == 0)
            assert output("Laptop")["video_speed"] > .99

            def pixels(y):
                return tuple(x.read(px, y + py) for px in (250, 330, 440, 600, 770) for py in (0, 25, 50))

            frozen, moving = pixels(350), pixels(710)
            until(lambda: pixels(710) != moving)
            assert pixels(350) == frozen
            x.unmap(x.d, window)
            x.sync(x.d, 0)
            until(lambda: not output("Wide")["occupied"] and .15 < output("Wide")["video_speed"] < .85)
            until(lambda: pixels(350) != frozen)
            assert output("Laptop")["video_speed"] > .99
            run("xrandr", "--delmonitor", "Laptop")
            run("xrandr", "--delmonitor", "Wide")
            desktop("reapply")
            x.map(x.d, window)
            x.sync(x.d, 0)
        desktop("config", "set", "video.empty_only", "false")
        until(lambda: status()["occupied"] and status()["video_speed"] > .9)
        desktop("inhibit")
        until(lambda: status()["inhibited"] and status()["video_speed"] < .01)
        desktop("uninhibit")
        guard = sp.Popen([BIN, "inhibit", "--", "sleep", "1"])
        until(lambda: status()["inhibited"])
        assert guard.wait(4) == 0
        until(lambda: not status()["inhibited"])
        x.unmap(x.d, window)
        x.sync(x.d, 0)

        desktop("set", first)
        ui = sp.Popen([BIN], stdout=sp.DEVNULL, stderr=sp.PIPE, text=True)
        def editor_window():
            found = run("xdotool", "search", "--onlyvisible", "--class", "^Desktop$", check=False)
            return found.stdout.strip()
        wid = until(editor_window)
        assert "_NET_WM_WINDOW_TYPE_DIALOG" in run("xprop", "-id", wid, "_NET_WM_WINDOW_TYPE").stdout
        run("xdotool", "windowfocus", wid)
        time.sleep(.5)
        run("xdotool", "mousemove", "--window", wid, "810", "34", "click", "1")
        time.sleep(.35)
        run("xdotool", "mousemove", "--window", wid, "514", "214", "click", "1")
        time.sleep(.25)
        run("xdotool", "mousemove", "--window", wid, "725", "375", "click", "1")
        time.sleep(.25)
        run("xdotool", "mousemove", "--window", wid, "672", "614", "click", "1")
        until(lambda: status()["rotation"] == "interval")
        assert "empty_only = false" in cfg.read_text().split("[parallax]")[1].split("[video]")[0]
        run("xdotool", "key", "Escape")
        assert ui.wait(5) == 0, ui.stderr.read()
        print("X11 checks passed: crop and root clicks, eligibility, rotation, video, motion, stowed config, and settings.")
    finally:
        desktop("stop", check=False)
        x.close(x.d)
        if ui and ui.poll() is None:
            ui.terminate()
            ui.wait(5)
        if wm:
            wm.terminate()
            wm.wait(5)
