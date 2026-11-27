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

    def read(self, x, y, window=None):
        data = self.image(self.d, self.root if window is None else window, x, y, 1, 1, -1, 2)
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
    assert "fade_ms = 60" in cfg.read_text()
    assert desktop("config", "set", "ui.corner_radius", 61, check=False).returncode != 0
    assert desktop("config", "set", "blur.easing_power", 7, check=False).returncode != 0
    run("xset", "s", "off")
    wm = sp.Popen([os.environ["TEST_WM"]], stdout=sp.DEVNULL, stderr=sp.DEVNULL) if os.environ.get("TEST_WM") else None
    x = X()
    ui = None
    try:
        def output(name):
            return next(o for o in status()["outputs"] if o["name"] == name)

        def visible_app(window, visible):
            (x.map if visible else x.unmap)(x.d, window)
            x.sync(x.d, 0)

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
        # Keep a legacy duration in the file and verify the actual pixels throughout the switch.
        cfg.write_text(cfg.read_text().replace("fade_ms = 60", "crossfade_ms = 800"))
        desktop("reload")
        incoming = tmp / "Incoming.png"
        incoming.write_bytes(second.read_bytes())
        places = ((200, 340), (250, 720))
        old, new = 0xEC1C3E, 0x2EA153
        until(lambda: all(x.read(*pos) == old for pos in places))
        start = time.monotonic()
        desktop("set", incoming)
        samples = []
        while time.monotonic() - start < 2:
            pixels = tuple(x.read(*pos) for pos in places)
            samples.append(pixels)
            if pixels == (new, new):
                break
            time.sleep(.01)
        assert samples[-1] == (new, new)
        assert time.monotonic() - start >= .75
        black = samples.index((0, 0))
        assert any(0 < (a >> 16) < 220 and (a >> 16) > (a >> 8 & 255)
                   for a, b in samples[:black] if a == b), samples
        assert any(0 < (a >> 8 & 255) < 150 and (a >> 8 & 255) > (a >> 16)
                   for a, b in samples[black + 1:] if a == b), samples
        desktop("config", "set", "layout.fade_ms", 0)
        desktop("set", first)
        until(lambda: all(x.read(*pos) == old for pos in places))
        desktop("config", "set", "layout.fade_ms", 60)
        if not wm:
            run("xdotool", "mousemove", "720", "240")
            until(lambda: x.read(710, 325) == 0xEC1C3E and x.read(560, 680) == 0xEC1C3E)
            window = x.app()
            until(lambda: output("Wide")["occupied"])
            time.sleep(.66)
            assert 0 < output("Wide")["blur_radius_px"] < 5.5
            until(lambda: output("Wide")["blur_radius_px"] == 24)
            assert 25 < (x.read(710, 325) >> 16) < 213
            assert output("Laptop")["blur_radius_px"] == 0 and x.read(560, 680) == 0xEC1C3E
            visible_app(window, False)
            until(lambda: not output("Wide")["occupied"])
            time.sleep(.66)
            assert 18.5 < output("Wide")["blur_radius_px"] < 24
            until(lambda: output("Wide")["blur_radius_px"] == 0 and x.read(710, 325) == 0xEC1C3E)

        desktop("config", "set", "blur.enabled", "false")
        if not wm:
            run("xdotool", "mousemove", "570", "690")
            until(lambda: x.read(568, 650) == 0xEC1C3E)
            until(lambda: output("Wide")["parallax_speed"] > .9 and output("Laptop")["parallax_speed"] > .9)
            window = x.app()
            until(lambda: output("Wide")["occupied"] and output("Wide")["parallax_speed"] < .03)
            assert not output("Laptop")["occupied"] and output("Laptop")["parallax_speed"] > .9
            run("xdotool", "mousemove", "950", "690")
            until(lambda: x.read(568, 650) == 0x1937D2)
            x.move(x.d, window, 180, 525)
            x.sync(x.d, 0)
            until(lambda: output("Laptop")["occupied"] and output("Laptop")["parallax_speed"] < .03)
            until(lambda: x.read(568, 650) == 0xEC1C3E)
            visible_app(window, False)
            until(lambda: x.read(568, 650) == 0x1937D2)
            run("xdotool", "mousemove", "121", "690")
            until(lambda: x.read(573, 650) == 0xEC1C3E)
            sweep = time.monotonic()
            run("xdotool", "mousemove", "1018", "690")
            until(lambda: x.read(568, 650) == 0x1937D2)
            assert time.monotonic() - sweep > .20
            run("xdotool", "mousemove", "570", "690")
            until(lambda: x.read(569, 650) == 0xEC1C3E and x.read(570, 650) == 0x1937D2)
            # These positions straddle a pixel rounding boundary, even while held.
            for pos in (621, 609, 627, 608, 624, 610):
                run("xdotool", "mousemove", str(pos), "690")
                time.sleep(.14)
                assert x.read(569, 650) == 0xEC1C3E
            run("xdotool", "mousemove", "625", "690")
            time.sleep(.8)
            assert x.read(569, 650) == 0xEC1C3E
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
            "testsrc2=size=1536x960:rate=12", "-vf",
            "drawbox=x=0:y=0:w=768:h=260:color=red:t=fill,drawbox=x=768:y=0:w=768:h=260:color=blue:t=fill",
            "-t", "1", "-c:v", "mpeg4", "-threads", "1", str(video))
        desktop("config", "set", "video.ramp_ms", "1200")
        desktop("config", "set", "blur.enabled", "true")
        desktop("set", video)
        until(lambda: status()["kind"] == "video" and status()["video_speed"] > .9)
        assert x.read(710, 170) >> 16 > 245
        p = x.read(300, 400)
        until(lambda: x.read(300, 400) != p)
        x.map(x.d, window)
        x.sync(x.d, 0)
        until(lambda: status()["occupied"])
        time.sleep(.48)
        assert .82 < status()["video_speed"] < 1
        until(lambda: status()["occupied"] and .15 < status()["video_speed"] < .85)
        until(lambda: status()["occupied"] and status()["video_speed"] < .01)
        until(lambda: status()["outputs"][0]["blur_radius_px"] == 24)
        if not wm:  # The window manager can tile the first window over the sample point.
            assert 25 < (x.read(710, 170) >> 16) < 225
        p = x.read(300, 400)
        time.sleep(.35)
        assert x.read(300, 400) == p
        x.unmap(x.d, window)
        x.sync(x.d, 0)
        until(lambda: not status()["occupied"])
        time.sleep(.48)
        assert 0 < status()["video_speed"] < .18
        until(lambda: status()["video_speed"] > .99 and status()["outputs"][0]["blur_radius_px"] == 0)
        assert x.read(710, 170) >> 16 > 245
        if not wm:
            run("xrandr", "--setmonitor", "Wide", "1440/380x480/127+0+0", "screen")
            run("xrandr", "--setmonitor", "Laptop", "900/238x420/111+120+480", "none")
            desktop("reapply")
            until(lambda: output("Wide")["video_speed"] > .99 and output("Laptop")["video_speed"] > .99)
            x.map(x.d, window)
            x.sync(x.d, 0)
            until(lambda: output("Wide")["occupied"] and .15 < output("Wide")["video_speed"] < .85)
            until(lambda: output("Wide")["video_speed"] == 0 and output("Wide")["blur_radius_px"] == 24)
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
        desktop("config", "set", "video.ramp_power", "1")
        until(lambda: status()["occupied"] and status()["video_speed"] == 0)
        x.unmap(x.d, window)
        x.sync(x.d, 0)
        until(lambda: not status()["occupied"])
        time.sleep(.48)
        assert .25 < status()["video_speed"] < .62
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

        packaged = wallpapers / "Canyon Morning"
        desktop("pack", second, packaged)
        assert (packaged / second.name).read_bytes() == second.read_bytes()
        assert (packaged / "thumbnail.webp").read_bytes()[8:12] == b"WEBP"
        assert desktop("pack", first, packaged, check=False).returncode != 0
        descriptor = packaged / "wallpaper.ini"
        descriptor.write_text(descriptor.read_text().replace(
            "[wallpaper]\n", "[wallpaper]\ntitle = Canyon Morning\ntags = sunrise, sandstone\nauthor = Test Artist\n"))
        broken = wallpapers / "Broken"
        broken.mkdir()
        (broken / "wallpaper.ini").write_text("[wallpaper]\nversion = 1\ntype = image\n[image]\nfile = ../Alpha.png\n")
        assert desktop("set", broken, check=False).returncode != 0

        color_video = tmp / "Clock.mp4"
        run("ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=red:size=1536x960:rate=12:d=1.5",
            "-vf", "drawbox=color=lime:t=fill:enable='gte(t,0.5)'", "-c:v", "mpeg4", "-threads", "1", str(color_video))
        slow_video = wallpapers / "Slow Motion Test"
        desktop("pack", color_video, slow_video)
        descriptor = slow_video / "wallpaper.ini"
        desktop("config", "set", "video.ramp_ms", 0)

        def time_color_change():
            desktop("set", first)
            until(lambda: x.read(700, 400) == 0xEC1C3E)
            desktop("set", slow_video)
            until(lambda: x.read(700, 400) & 0xF0F0F0 == 0xF00000)
            start = time.monotonic()
            until(lambda: x.read(700, 400) & 0xF0F0F0 == 0x00F000)
            return time.monotonic() - start

        normal_time = time_color_change()
        assert .30 <= normal_time <= .95, normal_time
        assert status()["playback_speed"] == 1
        descriptor.write_text(descriptor.read_text().replace("speed = 1", "speed = .25"))
        slow_time = time_color_change()
        assert 1.65 <= slow_time <= 3.0, slow_time
        print(f"Video frame transition: {normal_time:.2f}s at 1×, {slow_time:.2f}s at 0.25×.")
        slowed = status()
        assert slowed["package"] == str(slow_video) and slowed["playback_speed"] == .25
        assert slowed["video_speed"] == .25 and all(o["video_speed"] == .25 for o in slowed["outputs"])
        assert desktop("status").stdout.strip() == str(slow_video / color_video.name)
        assert desktop("status", "--path").stdout.strip() == str(slow_video)
        desktop("config", "set", "library.recursive", "true")
        metadata = json.loads(desktop("list", "--json").stdout)["wallpapers"]
        assert not any(Path(row["path"]).parent in (packaged, slow_video) for row in metadata)
        assert not any(row["path"] == str(broken) for row in metadata)
        package_entry = next(row for row in metadata if row["path"] == str(packaged))
        assert package_entry["kind"] == "image" and package_entry["tags"] == ["sunrise", "sandstone"]
        assert package_entry["title"] == "Canyon Morning" and package_entry["author"] == "Test Artist"
        assert package_entry["media"] == str(packaged / second.name)

        desktop("set", first)
        ui = sp.Popen([BIN], stdout=sp.DEVNULL, stderr=sp.PIPE, text=True)
        def editor_window():
            found = run("xdotool", "search", "--onlyvisible", "--class", "^Desktop$", check=False)
            return found.stdout.strip()
        wid = until(editor_window)
        assert "_NET_WM_WINDOW_TYPE_DIALOG" in run("xprop", "-id", wid, "_NET_WM_WINDOW_TYPE").stdout
        run("xdotool", "windowfocus", wid)
        time.sleep(.7)
        run("xdotool", "mousemove", "--window", wid, "177", "341", "click", "1")
        time.sleep(.25)
        run("xdotool", "key", "ctrl+f", "type", "slow motion")
        time.sleep(.3)
        run("xdotool", "mousemove", "--window", wid, "730", "246", "click", "1")
        until(lambda: status()["path"] == str(slow_video))
        run("xdotool", "mousemove", "--window", wid, "102", "341", "click", "1")
        run("xdotool", "key", "ctrl+f", "ctrl+a", "type", "sunrise sandstone")
        time.sleep(.3)
        run("xdotool", "mousemove", "--window", wid, "730", "246", "click", "1")
        until(lambda: status()["path"] == str(packaged))
        run("xdotool", "key", "ctrl+f", "ctrl+a", "type", "Tiny")

        def has_tile():
            return x.read(74, 392, int(wid)) != x.read(305, 392, int(wid))

        until(lambda: not has_tile())
        time.sleep(.15)
        assert not has_tile()
        run("xdotool", "mousemove", "--window", wid, "235", "341", "click", "1")
        until(has_tile)
        run("xdotool", "mousemove", "--window", wid, "235", "341", "click", "1")
        until(lambda: not has_tile())
        time.sleep(.5)
        run("xdotool", "mousemove", "--window", wid, "810", "34", "click", "1")
        time.sleep(.35)
        run("xdotool", "mousemove", "--window", wid, "514", "214", "click", "1")
        time.sleep(.25)
        run("xdotool", "mousemove", "--window", wid, "672", "614", "click", "1")
        until(lambda: status()["rotation"] == "interval")
        run("xdotool", "mousemove", "--window", wid, "682", "34", "click", "1")
        time.sleep(.35)
        run("xdotool", "mousemove", "--window", wid, "725", "88", "click", "1")
        run("xdotool", "mousemove", "--window", wid, "725", "214", "click", "1")
        time.sleep(.25)
        run("xdotool", "mousemove", "--window", wid, "672", "614", "click", "1")
        until(lambda: "enabled = false" in cfg.read_text().split("[blur]")[1].split("[motion]")[0])
        assert "empty_only = false" in cfg.read_text().split("[parallax]")[1].split("[video]")[0]
        run("xdotool", "key", "Escape")
        assert ui.wait(5) == 0, ui.stderr.read()
        desktop("set", first)
        previous = status()
        migrated = wallpapers / "Alpha Package"
        desktop("pack", first, migrated)
        first.rename(tmp / first.name)
        desktop("stop")
        restored = status()
        assert restored["selection"] == str(migrated) and restored["path"] == str(migrated)
        assert restored["changed_at"] == previous["changed_at"]
        print("X11 checks passed: crop, fade, rotation, effects, packages, video speed, filter, search, and settings.")
    finally:
        desktop("stop", check=False)
        x.close(x.d)
        if ui and ui.poll() is None:
            ui.terminate()
            ui.wait(5)
        if wm:
            wm.terminate()
            wm.wait(5)
