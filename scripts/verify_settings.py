"""Check that every option of the settings menu does what it says, by changing
it through the menu as a player would and measuring the effect.

    python3 scripts/verify_settings.py        # build/main's game, results in build/verify-settings
    python3 scripts/verify_settings.py --bin ... --out ~/vs --phases audio,corrupt
    python3 scripts/verify_settings.py --analyze ~/vs           # re-read finished runs

Each phase is one launch of the game with a fresh profile of its own (never
the player's): on Linux XDG_DATA_HOME, XDG_CACHE_HOME and XDG_CONFIG_HOME
under --out, SDL's offscreen video driver and NFSMW_GAME_MODE=0; on macOS a
HOME under --out and a 1376x576 window. Audio always goes to SDL's disk
driver (a file under --out, deleted once measured), so nothing is heard.
The captures stay under --out for --analyze (about 1 GB for every phase at
3440x1440): the default is the repo's build/, not /tmp (RAM on SteamOS).
--out must be new, empty, or an earlier run's (it holds a .verify-settings
mark): a phase's folders there are deleted before it runs.
Menu keys come from NFSMW_UI_KEYS (with "Shot" captures on the keys' clock),
controller input from NFSMW_VIRTUAL_PAD (an SDL virtual controller, so the
deadzones and rumble take the device path), game input from
NFSMW_INPUT_SCRIPT. Every menu change is logged by the game ("[settings]
menu: ..."), which is how navigation is confirmed before an effect is judged.

Phases (the race ones take 3-6 minutes, the rest under 2; in this order,
as the relaunch phases reuse the video phase's profile):
  video     the first race: Vibration off, a few seconds of driving (the
            game asks for rumble, the virtual controller gets none), then the
            pause menu. Each picture option is changed and put back between
            captures (before, changed, after: a paused scene still drifts as
            the game's exposure adapts, so a change is judged against the two
            captures around it, and a bracket with the menu open and nothing
            changed is the control): anti-aliasing, upscaling, sharpening,
            anisotropic filtering, aspect ratio, the performance overlay;
            Mipmaps and Internal Resolution marked RESTART and changing
            nothing yet; V-Sync's present mode and frame rate; the deadzones
            on a virtual pad. Also the defaults of a new profile, and
            settings.toml after the menu closes.
  relaunch  the same profile: settings.toml loads, the deadzones apply at
            launch, Vibration (stored off) back on and the rumble reaches the
            controller, Mipmaps off is what runs (the menu defers On).
  relaunch2 Mipmaps on again: the race's world load reads more bytes per
            texture than relaunch's did (mip levels; base levels are ~3/4);
            Internal Resolution 1x for the next launch.
  relaunch3 (short) the stored 1x is the internal resolution.
  upscale   the picture checks at NFSMW_RESOLUTION=1x: a 720p frame scaled up
            to the window (3440x1440 offscreen on Linux, 2752x1152 on a Retina
            Mac), what Upscaling and Sharpening are for. (At Auto the frame is
            already about the window's size.)
  audio     the title screen and attract movies: master volume, mute, mute in
            background (focus events), measured on SDL's disk driver's output
            against the game's own mix (NFSMW_AUDIO_DUMP); an environment
            override (NFSMW_SHARPEN) shows ENV and isn't saved over.
  corrupt   a settings.toml that doesn't parse: toast, .bad copy, defaults.
  gamemode  (short) Game Mode (NFSMW_GAME_MODE=1, as in Steam's gamescope
            session): the window starts fullscreen, and the menu's Fullscreen
            row shows On and ignores Left, Right and Enter. On macOS only with
            --fullscreen (it takes over the screen).
  async_on, async_off
            cold shader caches to the first race, Asynchronous Shaders left
            on, then turned off from the menu at 3 s: no draw skipped for a
            compiling pipeline.
  window    fullscreen and V-Sync on a real window: the swapchain and present
            mode follow. On macOS it's the normal window (fullscreen only with
            --fullscreen, as it takes over the screen for a few seconds); on
            Linux only with --display, e.g. inside a nested headless gamescope:
              gamescope --backend headless -W 3440 -H 1440 -w 3440 -h 1440 -r 60 -- \\
                python3 scripts/verify_settings.py --phases window --display --fullscreen ...
            (gamescope's window manager doesn't restore a window's size when
            it leaves fullscreen: that check is inconclusive there.)
By default every phase runs that fits the machine: on Linux all but window,
on macOS all but gamemode (about 30 minutes).
Results: a table on stdout and <out>/results.tsv; PASS, FAIL, or INCONCLUSIVE
(the run didn't reach the state a check needs, e.g. the scene wasn't still).
The scripted input reaches the first race from a new profile; it drifts with
machine speed, which the checks tolerate (they only need a paused race).
Needs numpy for the image and audio measurements. On SteamOS run it inside
the nfsmw-build distrobox, and not while the game is being played.
"""
import argparse
import os
import pathlib
import platform
import re
import shutil
import signal
import subprocess
import sys
import time

try:
    import numpy as np
except ImportError:  # the analysis needs it; say so when it runs
    np = None

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAC = platform.system() == 'Darwin'

# The first race from a new profile, as scripts/bench.sh reaches it; START at
# the end pauses it (the scene keeps rendering behind the pause menu).
RACE = '8-26/2:START,30-130/4:DOWN,31-131/4:A,32.5-132.5/4:START,145:A'

# The menu's pages and rows (runtime/user/settings.cpp, hidden rows left out).
PAGES = ['Display', 'Graphics', 'Audio', 'Input', 'Advanced']
ROWS = {
    'Display': ['video.fullscreen', 'video.vsync', 'video.frame_rate', 'video.aspect_ratio'],
    'Graphics': ['video.anti_aliasing', 'video.scaling', 'video.sharpening', 'video.anisotropic_filtering',
                 'video.mipmaps', 'video.internal_resolution'],
    'Audio': ['audio.master_volume', 'audio.mute', 'audio.mute_in_background'],
    'Input': ['input.stick_deadzone', 'input.trigger_deadzone', 'input.vibration'],
    'Advanced': ['advanced.async_shaders', 'advanced.performance_overlay'],
}
# Defaults (the option table) as settings.toml writes them.
DEFAULTS = {
    'video.fullscreen': 'false', 'video.vsync': 'true', 'video.frame_rate': '"60"', 'video.present_scale': '"100"',
    'video.anti_aliasing': 'true', 'video.scaling': '"bicubic"',
    'video.aspect_ratio': '"auto"', 'video.sharpening': '0.3', 'video.anisotropic_filtering': '"auto"',
    'video.mipmaps': 'true', 'video.internal_resolution': '"auto"', 'audio.master_volume': '100', 'audio.mute': 'false',
    'audio.mute_in_background': 'false', 'input.stick_deadzone': '0.08', 'input.trigger_deadzone': '0.02',
    'input.vibration': 'true', 'advanced.async_shaders': 'true', 'advanced.performance_overlay': 'false',
}


class Program:
    """NFSMW_UI_KEYS as a player would press them, with the menu's state
    tracked (its page survives closing it; a page opens on its first row)."""

    def __init__(self, start):
        self.t = start
        self.items = []
        self.page = 'Display'
        self.row = 0
        self.open = False
        self.shots = []      # labels, in the order of build/ui_shot_<n>.ppm
        self.shot_times = [] # when each is due (the keys' clock)
        self.expect = []     # (time, 'table.key', value shown after, how)

    def key(self, name, gap=0.3):
        self.items.append(f'{self.t:.2f}:{name}')
        self.t += gap

    def wait(self, seconds):
        self.t += seconds

    def shot(self, label):
        self.shots.append(label)
        self.shot_times.append(self.t)
        self.key('Shot', 0.6)

    def menu(self, page):
        if not self.open:
            self.key('F1', 1.2)
            self.open = True
            self.row = 0
        steps = (PAGES.index(page) - PAGES.index(self.page)) % len(PAGES)
        if steps:
            name, count = ('E', steps) if steps <= 2 else ('Q', len(PAGES) - steps)
            for _ in range(count):
                self.key(name, 0.8)
            self.page = page
            self.row = 0

    def select(self, option):
        """Opens the menu at the option's page and moves the cursor to it."""
        page = next(p for p, rows in ROWS.items() if option in rows)
        self.menu(page)
        target = ROWS[page].index(option)
        while self.row != target:
            self.key('Down' if target > self.row else 'Up', 0.3)
            self.row += 1 if target > self.row else -1

    def change(self, option, presses, shows, how='applied now'):
        self.select(option)
        for _ in range(abs(presses)):
            self.key('Right' if presses > 0 else 'Left', 0.25)
        self.expect.append((self.t, option, shows, how))
        self.wait(0.3)

    def close(self, settle=1.2):
        if self.open:
            self.key('Escape', settle)
            self.open = False

    def env(self):
        return ','.join(self.items)


def race_start(p, vibration=None):
    """The first race from a new profile: Vibration toggled in the menu (if
    given) while the car waits at the start line, six seconds of driving (the
    game rumbles with the engine), then the pause menu. Returns the input
    script. Before any capture on purpose: the keys after a Shot run later
    by its time (ui.cpp's Tick), off the input script's clock."""
    p.t = 150.0
    if vibration:
        p.change('input.vibration', 1, vibration)
        p.close()
    drive = max(p.t + 3, 157.0)  # the keys' clock starts a second or two after the script's
    pause = drive + 7
    p.t = pause + 4
    return f'{RACE},{drive:.1f}~{drive + 6:.1f}:RT,{pause:.1f}:START'


def bracket(p, label, forward, back, settle=1.2):
    """A capture before, one with the change, one after putting it back: a
    paused scene still drifts (the game's exposure keeps adapting), so the
    change is judged against the two captures around it."""
    p.shot(label + '_ref')
    for change in forward:
        p.change(*change)
    p.close(settle)
    p.shot(label)
    for change in back:
        p.change(*change)
    p.close(settle)
    p.shot(label + '_back')


def picture_checks(p):
    # The control: the menu open as long as for a change, nothing changed
    # (the scene still moves a little: shadows, lights).
    p.shot('noop_ref')
    p.menu('Graphics')
    p.wait(2.5)
    p.close()
    p.shot('noop')
    p.menu('Graphics')
    p.wait(2.5)
    p.close()
    p.shot('noop_back')
    bracket(p, 'aa_off', [('video.anti_aliasing', 1, 'Off')], [('video.anti_aliasing', 1, 'On')])
    bracket(p, 'bilinear', [('video.scaling', 1, 'Bilinear')], [('video.scaling', -1, 'Bicubic')])
    bracket(p, 'sharpen_1', [('video.sharpening', 14, '1.00')], [('video.sharpening', -14, '0.30')])
    bracket(p, 'sharpen_0', [('video.sharpening', -6, '0.00')], [('video.sharpening', 6, '0.30')])
    bracket(p, 'aniso_off', [('video.anisotropic_filtering', 1, 'Off')], [('video.anisotropic_filtering', -1, 'Auto')])
    # The whole range, whatever Auto is here (16x on a discrete GPU, 4x on an integrated one).
    p.change('video.anisotropic_filtering', 1, 'Off')
    p.close()
    bracket(p, 'aniso_16', [('video.anisotropic_filtering', 4, '16x')], [('video.anisotropic_filtering', -4, 'Off')])
    p.change('video.anisotropic_filtering', -1, 'Auto')
    p.close()


def phase_video():
    """The first race: Vibration off, then drive (the game asks for rumble);
    paused, one option at a time against the captures around it; the
    deadzones on a virtual pad."""
    p = Program(0)
    script = race_start(p, 'Off')
    p.shot('base0')
    p.wait(1.0)
    p.shot('base1')
    # Deadzones: the virtual pad's steps are on the process clock, the keys
    # on the UI's (from the first frame, a second or so later, and later
    # still after stalls): keep gaps.
    pad = []
    t0 = p.t + 1
    pad += [f'{t0:.1f}:LX=0.05', f'{t0 + 1.5:.1f}:LX=0.12', f'{t0 + 3:.1f}:RT=0.01', f'{t0 + 4.5:.1f}:RT=0.06']
    p.t = t0 + 7
    p.change('input.stick_deadzone', 5, '13%')
    p.change('input.trigger_deadzone', 8, '10%')
    p.close()
    t1 = p.t + 3
    pad += [f'{t1:.1f}:LX=0.12', f'{t1 + 1.5:.1f}:LX=0.18', f'{t1 + 3:.1f}:RT=0.06', f'{t1 + 4.5:.1f}:RT=0.15',
            f'{t1 + 6:.1f}:LX=0', f'{t1 + 6:.1f}:RT=0']
    p.t = t1 + 8
    p.menu('Display')
    p.shot('menu_display')  # a fresh profile: every row at its default
    p.menu('Graphics')
    p.shot('menu_graphics')
    p.close()
    picture_checks(p)
    bracket(p, 'aspect_169', [('video.aspect_ratio', 1, '16:9')], [('video.aspect_ratio', -1, 'Fill Screen')])
    # Restart options: changed, the picture as before; put back (the running
    # values again), as before too.
    bracket(p, 'restart', [('video.mipmaps', 1, 'Off', 'at the next launch'),
                           ('video.internal_resolution', 1, '1x (720p)', 'at the next launch')],
            [('video.mipmaps', 1, 'On', 'applied now'), ('video.internal_resolution', -1, 'Auto', 'applied now')])
    # Stored for the next launch (the relaunch phases): Mipmaps off, both marked.
    p.change('video.mipmaps', 1, 'Off', 'at the next launch')
    p.change('video.internal_resolution', 1, '1x (720p)', 'at the next launch')
    p.shot('menu_restart')  # RESTART on both rows
    p.change('video.internal_resolution', -1, 'Auto', 'applied now')
    p.close()
    bracket(p, 'overlay_on', [('advanced.performance_overlay', 1, 'On')], [('advanced.performance_overlay', -1, 'Off')], 2.0)
    # V-Sync off for a while in a scene the game draws at 60 fps: the present
    # mode and the [perf] frame rate.
    p.change('video.vsync', -1, 'Off')
    p.close(10)
    p.change('video.vsync', 1, 'On')
    p.close(4)
    p.shot('end')
    return dict(script=script, keys=p, pad=','.join(pad), secs=p.t + 2,
                env={'NFSMW_INPUT_LOG': '1', 'NFSMW_GPU_TIMING': '1'})


def phase_upscale():
    """The picture checks again at 1x (NFSMW_RESOLUTION=1x): a 720p frame
    scaled up to the window, what Upscaling and Sharpening are for."""
    p = Program(0)
    script = race_start(p)
    p.shot('base0')
    p.wait(1.0)
    p.shot('base1')
    picture_checks(p)
    p.shot('end')
    return dict(script=script, keys=p, secs=p.t + 3, env={'NFSMW_RESOLUTION': '1x'})


def phase_relaunch():
    """The video phase's profile: its stored values at launch. Vibration
    (stored off) back on, then drive: the rumble reaches the controller.
    Mipmaps (stored off) is what runs; On waits for the next launch."""
    p = Program(0)
    script = race_start(p, 'On')
    p.shot('base0')
    p.wait(1.0)
    p.shot('base1')
    p.select('video.mipmaps')
    p.shot('menu_mips')  # Off, no RESTART marker: the stored value is the running one
    p.change('video.mipmaps', 1, 'On', 'at the next launch')
    p.close()
    p.shot('end')
    # A (virtual) controller to rumble; no steps.
    return dict(script=script, keys=p, pad='0:LX=0', secs=p.t + 3, env={'NFSMW_INPUT_LOG': '1'})


def phase_relaunch2():
    """Mipmaps back on at launch (the race's world load compares with the
    relaunch's); Internal Resolution 1x for the next launch."""
    p = Program(0)
    script = race_start(p)
    p.shot('base0')
    p.wait(1.0)
    p.shot('base1')
    p.change('video.internal_resolution', 1, '1x (720p)', 'at the next launch')
    p.close()
    p.shot('end')
    return dict(script=script, keys=p, secs=p.t + 3)


def phase_relaunch3():
    p = Program(8.0)
    p.shot('start')
    p.shot('end')
    return dict(script='', keys=p, secs=16)


def phase_audio():
    """The title screen and attract loop (no input: the music plays on).
    Levels are judged window by window against the game's own mix."""
    p = Program(14.0)
    p.change('audio.master_volume', -10, '50%')
    p.close()
    p.wait(8)
    p.change('audio.master_volume', 10, '100%')
    p.change('audio.mute', 1, 'On')
    p.close()
    p.wait(8)
    p.change('audio.mute', -1, 'Off')
    p.change('audio.mute_in_background', 1, 'On')
    p.close()
    p.wait(3)
    p.key('FocusLost', 8)
    p.key('FocusGained', 6)
    p.change('audio.mute_in_background', -1, 'Off')
    p.close()
    p.wait(3)
    p.key('FocusLost', 8)
    p.key('FocusGained', 3)
    # An environment override: shown, applied, never saved over.
    p.change('video.sharpening', 1, '0.35', "saved; the environment keeps this run's value")
    p.shot('menu_env')
    p.close()
    p.shot('end')
    return dict(script='', keys=p, secs=p.t + 3, env={'NFSMW_SHARPEN': '0.8', 'NFSMW_AUDIO_DUMP_SECONDS': '600'},
                dump=True)


CORRUPT_TOML = '[video]\nanti_aliasing = false\nsharpening = \n'


def phase_corrupt():
    p = Program(5.0)
    p.shot('toast')
    p.t = 20.0
    p.shot('after_toast')
    p.shot('end')
    return dict(script='', keys=p, secs=23, seed=CORRUPT_TOML)


def phase_async(off):
    p = Program(3.0)  # before the first START tap, so both runs get the same input
    if off:
        p.change('advanced.async_shaders', 1, 'Off')
        p.close()
    return dict(script=f'{RACE},165:START', keys=p, secs=172, cold=True)


def phase_gamemode():
    """Game Mode always runs fullscreen (Steam sets the resolution): the row
    is shown On and doesn't change, and nothing is saved over the desktop's
    preference in the same settings.toml."""
    p = Program(10.0)
    p.select('video.fullscreen')
    for name in ('Left', 'Right', 'Return'):
        p.key(name, 0.5)
    p.shot('menu_locked')  # On, with "Always on in Game Mode" below
    p.close()
    p.shot('end')
    return dict(script='', keys=p, secs=p.t + 2, env={'NFSMW_GAME_MODE': '1'})


def phase_window(fullscreen):
    p = Program(10.0)
    p.shot('windowed')
    if fullscreen:
        p.change('video.fullscreen', 1, 'On')
        p.close(2.5)
        p.shot('fullscreen')
        p.change('video.fullscreen', -1, 'Off')
        p.close(2.5)
        p.shot('windowed_again')
    p.change('video.vsync', -1, 'Off')
    p.close()
    p.wait(20)
    p.change('video.vsync', 1, 'On')
    p.close()
    p.wait(12)
    p.shot('end')
    return dict(script='', keys=p, secs=p.t + 2, env={'NFSMW_WINDOW': '1376x576'})


# ---------------------------------------------------------------- running

def phase_plan(name, args):
    if name == 'video': return phase_video()
    if name == 'upscale': return phase_upscale()
    if name == 'relaunch': return phase_relaunch()
    if name == 'relaunch2': return phase_relaunch2()
    if name == 'relaunch3': return phase_relaunch3()
    if name == 'audio': return phase_audio()
    if name == 'corrupt': return phase_corrupt()
    if name == 'async_on': return phase_async(False)
    if name == 'async_off': return phase_async(True)
    if name == 'window': return phase_window(args.fullscreen)
    if name == 'gamemode': return phase_gamemode()
    raise SystemExit(f'unknown phase {name}')


PROFILE = {'relaunch': 'video', 'relaunch2': 'video', 'relaunch3': 'video'}  # phases that reuse another's profile


def user_dir(out, profile):
    base = out / 'profiles' / profile
    return base / 'home/Library/Application Support/SpeedBreaker' if MAC else base / 'data/speedbreaker'


def run_phase(name, args):
    plan = phase_plan(name, args)
    out = pathlib.Path(args.out).resolve()
    run = out / name
    profile = PROFILE.get(name, name)
    prof = out / 'profiles' / profile
    if profile == name and prof.exists():
        shutil.rmtree(prof)
    if run.exists():
        shutil.rmtree(run)
    (run / 'build').mkdir(parents=True)
    udir = user_dir(out, profile)
    udir.mkdir(parents=True, exist_ok=True)
    if 'seed' in plan:
        (udir / 'settings.toml').write_text(plan['seed'])
        (run / 'seed.toml').write_text(plan['seed'])
    if (udir / 'settings.toml').exists():
        shutil.copy(udir / 'settings.toml', run / 'settings.before.toml')
    env = dict(os.environ)
    for k in list(env):
        if k.startswith('NFSMW_') and k != 'NFSMW_GAME_DIR':
            del env[k]  # only what the phase sets
    cache = out / 'caches' / name
    if cache.exists():
        shutil.rmtree(cache)
    warm = None if plan.get('cold') else args.cache
    if MAC:
        home = prof / 'home'
        env['HOME'] = str(home)
        cache_dir = home / 'Library/Caches/SpeedBreaker'
        env['NFSMW_WINDOW'] = '1376x576'
    else:
        env['XDG_DATA_HOME'] = str(prof / 'data')
        env['XDG_CACHE_HOME'] = str(cache)
        env['XDG_CONFIG_HOME'] = str(prof / 'config')
        cache_dir = cache / 'speedbreaker'
        env['NFSMW_GAME_MODE'] = '0'
        env['NFSMW_WINDOW'] = '3440x1440'
        # A real display only on request (X11, as scripts/steamos/speedbreaker.sh runs it).
        env['SDL_VIDEODRIVER'] = 'x11' if args.display else 'offscreen'
    if cache_dir.exists():
        shutil.rmtree(cache_dir)
    if warm:
        shutil.copytree(warm, cache_dir)
    else:
        cache_dir.mkdir(parents=True)
    env['SDL_AUDIO_DRIVER'] = 'disk'
    env['SDL_AUDIO_DISK_OUTPUT_FILE'] = str(run / 'sdl-disk.raw')
    if args.game_dir:
        env['NFSMW_GAME_DIR'] = args.game_dir
    if plan['script']:
        env['NFSMW_INPUT_SCRIPT'] = plan['script']
    env['NFSMW_UI_KEYS'] = plan['keys'].env()
    if plan.get('pad'):
        env['NFSMW_VIRTUAL_PAD'] = plan['pad']
    if plan.get('dump'):
        env['NFSMW_AUDIO_DUMP'] = str(run / 'mix-dump.raw')
    env.update(plan.get('env', {}))
    with open(run / 'plan.txt', 'w') as f:
        for k in sorted(env):
            if k.startswith(('NFSMW_', 'SDL_', 'XDG_', 'GAMESCOPE')) or k == 'HOME':
                f.write(f'{k}={env[k]}\n')
        f.write(f'shots={",".join(plan["keys"].shots)}\n')
        f.write(f'shot_times={",".join(f"{t:.2f}" for t in plan["keys"].shot_times)}\n')
        for t, key, shows, how in plan['keys'].expect:
            f.write(f'expect={t:.2f}|{key}|{shows}|{how}\n')
    secs = plan['secs']
    print(f'[{name}] {secs:.0f} s', flush=True)
    with open(run / 'run.log', 'wb') as log:
        proc = subprocess.Popen([str(pathlib.Path(args.bin).resolve())], cwd=run, env=env, stdout=log,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        # Until the last capture ('end', after everything else) is saved: the
        # keys after each capture run later by its time. A phase without
        # captures runs for its planned time. 90 s of grace.
        want = len(plan['keys'].shots)
        start = time.monotonic()
        while proc.poll() is None and time.monotonic() - start < secs + (90 if want else 0):
            time.sleep(1)
            if want and time.monotonic() - start > 10 and \
                    (run / 'run.log').read_text(errors='replace').count('saved to build/ui_shot_') >= want:
                time.sleep(1.5)
                break
        for sig in (signal.SIGTERM, signal.SIGKILL):
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, sig)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
    for src, dst in (('settings.toml', 'settings.after.toml'), ('settings.toml.bad', 'settings.after.bad')):
        if (udir / src).exists():
            shutil.copy(udir / src, run / dst)
    if (run / 'sdl-disk.raw').exists():
        audio_levels(run)
    for f in ('sdl-disk.raw', 'mix-dump.raw', 'sdlaudio.raw'):
        (run / f).unlink(missing_ok=True)
    if not args.keep_cache and cache.exists():
        shutil.rmtree(cache)


# ---------------------------------------------------------------- audio

def audio_levels(run):
    """RMS per 50 ms of the device's output and of the game's own mix
    (NFSMW_AUDIO_DUMP, 6 channels), to levels.txt; the raw files are big."""
    log = (run / 'run.log').read_text(errors='replace')
    m = re.search(r'\[audio\] output: .*device (\d+) ch', log)
    ch = int(m.group(1)) if m else 2
    step = 2400  # 50 ms at 48 kHz

    def rms(path, channels):
        if not path.exists():
            return np.zeros(0)
        a = np.fromfile(path, dtype='<f4')
        n = len(a) // (channels * step)
        a = a[:n * channels * step].reshape(n, step * channels).astype(np.float64)
        return np.sqrt((a * a).mean(axis=1))

    out = rms(run / 'sdl-disk.raw', ch)
    mix = rms(run / 'mix-dump.raw', 6)
    with open(run / 'levels.txt', 'w') as f:
        f.write(f'# 50 ms windows: device output ({ch} ch) rms, game mix rms\n')
        for i in range(max(len(out), len(mix))):
            f.write(f'{i * 0.05:.2f} {out[i] if i < len(out) else -1:.6f} {mix[i] if i < len(mix) else -1:.6f}\n')


# ---------------------------------------------------------------- analysis

class Results:
    def __init__(self):
        self.rows = []

    def add(self, option, phase, check, verdict, detail):
        self.rows.append((option, phase, check, verdict, detail))


def read_ppm(path):
    data = path.read_bytes()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data, np.uint8, w * h * 3, pos + 1).reshape(h, w, 3).astype(np.float32)


class Shots(dict):
    """label -> capture file, for one run (its plan knows the times)."""
    run = None


def shots(run):
    names = []
    for line in (run / 'plan.txt').read_text().splitlines():
        if line.startswith('shots='):
            names = [n for n in line[6:].split(',') if n]
    found = Shots()
    found.run = run
    for i, n in enumerate(names):
        path = run / 'build' / f'ui_shot_{i}.ppm'
        if path.exists():
            found[n] = path
    return found


def shot_weight(run, label):
    """Where the changed capture falls between the two around it, in time
    (as planned): the drift between them is interpolated there."""
    names, times = [], []
    for line in (run / 'plan.txt').read_text().splitlines():
        if line.startswith('shots='):
            names = line[6:].split(',')
        elif line.startswith('shot_times='):
            times = [float(x) for x in line[11:].split(',')]
    try:
        tr, tc, tb = (times[names.index(n)] for n in (label + '_ref', label, label + '_back'))
        return (tc - tr) / (tb - tr)
    except (ValueError, IndexError, ZeroDivisionError):
        return 0.5


def luma(img):
    return img[..., 0] * 0.299 + img[..., 1] * 0.587 + img[..., 2] * 0.114


BLOCK = 16


def still_blocks(a, b, threshold=6.0):
    """16x16 blocks where two captures of the same paused frame agree (the
    game animates a few things under its pause menu), one pixel's margin."""
    d = np.abs(a - b).max(axis=2)
    h, w = d.shape[0] // BLOCK * BLOCK, d.shape[1] // BLOCK * BLOCK
    noisy = (d[:h, :w] > threshold).reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).any(axis=(1, 3))
    grown = noisy.copy()
    grown[1:] |= noisy[:-1]; grown[:-1] |= noisy[1:]; grown[:, 1:] |= noisy[:, :-1]; grown[:, :-1] |= noisy[:, 1:]
    return ~grown


def lap_energy(img, keep, rows=None):
    """Mean squared Laplacian of the luma over the kept blocks: edges and fine
    detail. Anti-aliasing and softer scaling lower it; sharpening raises it."""
    y = luma(img)
    lap = np.zeros_like(y)
    lap[1:-1, 1:-1] = 4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:]
    h, w = keep.shape[0] * BLOCK, keep.shape[1] * BLOCK
    e = (lap[:h, :w] ** 2).reshape(keep.shape[0], BLOCK, keep.shape[1], BLOCK).mean(axis=(1, 3))
    k = keep.copy()
    if rows is not None:
        k[:int(k.shape[0] * rows[0])] = False
        k[int(k.shape[0] * rows[1]):] = False
    return float(e[k].mean()) if k.any() else float('nan')


HOW = r"applied now|at the next launch|saved; the environment keeps this run's value|invalid"


def menu_lines(log):
    # "[settings] menu: video.internal_resolution Auto -> 1x (720p) (at the next launch)"
    return re.findall(rf'^\[settings\] menu: (\S+) (.+?) -> (.+) \(({HOW})\)$', log, re.M)


def check_navigation(res, phase, run, log):
    """Every change the phase meant to make shows in the log, in order."""
    want = [line.split('=', 1)[1].split('|') for line in (run / 'plan.txt').read_text().splitlines()
            if line.startswith('expect=')]
    got = menu_lines(log)
    gi, missing = 0, []
    for _, key, shows, how in want:
        while gi < len(got) and not (got[gi][0] == key and got[gi][2] == shows):
            gi += 1
        if gi == len(got):
            missing.append(f'{key} -> {shows}')
            continue
        if got[gi][3] != how:
            missing.append(f'{key} -> {shows} said "{got[gi][3]}", wanted "{how}"')
        gi += 1
    # A key that landed on the wrong row changes an option the plan never names.
    planned = {key for _, key, _, _ in want}
    stray = sorted({g[0] for g in got if g[0] not in planned})
    missing += [f'unplanned change of {key}' for key in stray]
    ok = not missing
    res.add('(menu)', phase, 'each planned change made through the menu, logged in order', 'PASS' if ok else 'FAIL',
            f'{len(want)} changes' if ok else '; '.join(missing[:4]))
    return ok


def toml_values(path):
    vals, table = {}, None
    if not path.exists():
        return vals
    for line in path.read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if m := re.match(r'\[(\w+)\]$', line):
            table = m.group(1)
        elif (m := re.match(r'(\w+)\s*=\s*(.+)$', line)) and table:
            vals[f'{table}.{m.group(1)}'] = m.group(2).strip()
    return vals


def masked_diff(a, b, keep):
    """Mean per-pixel difference (largest channel) over the kept blocks."""
    h, w = keep.shape[0] * BLOCK, keep.shape[1] * BLOCK
    d = np.abs(a[:h, :w] - b[:h, :w]).max(axis=2).reshape(keep.shape[0], BLOCK, keep.shape[1], BLOCK).mean(axis=(1, 3))
    return float(d[keep].mean()) if keep.any() else float('nan')


def analyze_video(res, run, log):
    phase = 'video'
    check_navigation(res, phase, run, log)
    s = shots(run)
    # Defaults of a fresh profile.
    fresh = 'yet: defaults' in log
    starts = {
        'anti-aliasing, bicubic scaling, sharpen 0.30': 'video.anti_aliasing/scaling/sharpening',
        'stick deadzone 0.08, trigger deadzone 0.02': 'input deadzones',
        'present mode FIFO (V-Sync on)': 'video.vsync',
        'gain 1.0000 (volume 100%)': 'audio.master_volume/mute',
        'anisotropic filtering: ': 'video.anisotropic_filtering',
        'internal resolution ': 'video.internal_resolution',
    }
    missing = [what for text, what in starts.items() if text not in log]
    auto_res = re.search(r'internal resolution \S+ \(\S+, auto for', log)
    res.add('(defaults)', phase, 'a new profile starts from the defaults (log at launch)',
            'PASS' if fresh and not missing and auto_res else 'FAIL',
            'no settings.toml, defaults in effect' if fresh and not missing and auto_res else f'missing: {missing}, fresh {fresh}')
    after = toml_values(run / 'settings.after.toml')
    untouched = [k for k in DEFAULTS if k not in ('video.mipmaps', 'input.stick_deadzone', 'input.trigger_deadzone',
                                                  'input.vibration')]
    wrong = [f'{k}={after.get(k)}' for k in untouched if after.get(k) != DEFAULTS[k]]
    res.add('(defaults)', phase, 'options put back or left alone are saved at their defaults',
            'PASS' if after and not wrong else 'FAIL', 'settings.toml matches' if after and not wrong else f'{wrong or "no settings.toml"}')
    changed = {'video.mipmaps': 'false', 'input.stick_deadzone': '0.13', 'input.trigger_deadzone': '0.1',
               'input.vibration': 'false'}
    wrong = [f'{k}={after.get(k)}' for k, v in changed.items() if after.get(k) != v]
    res.add('(persistence)', phase, 'values changed in the menu are in settings.toml',
            'PASS' if not wrong else 'FAIL', ', '.join(f'{k}={v}' for k, v in changed.items()) if not wrong else str(wrong))

    rumble_results(res, phase, log, 'Off')

    # Deadzones: each virtual-pad step against the formula (input.cpp).
    steps = re.findall(r'\[input\] virtual pad (\S+): left stick raw \(([-+.\d]+), ([-+.\d]+)\) -> game \((-?\d+), (-?\d+)\).*?'
                       r'triggers raw \(([.\d]+), ([.\d]+)\) -> game \((\d+), (\d+)\); deadzones: stick ([.\d]+), trigger ([.\d]+)', log)
    for option, axis in (('input.stick_deadzone', 'LX'), ('input.trigger_deadzone', 'RT')):
        seen, bad, dzs, stuck = 0, [], set(), []
        for st in steps:
            if not st[0].startswith(axis + '='):
                continue
            raw = float(st[1]) if axis == 'LX' else float(st[6])
            got = int(st[3]) if axis == 'LX' else int(st[8])
            dz = float(st[9]) if axis == 'LX' else float(st[10])
            # The device must report the value the step set, or the formula
            # below holds trivially (a pad stuck at 0.05 reads 0 at any deadzone).
            if abs(raw - float(st[0].split('=')[1])) > 0.002:
                stuck.append(f'{st[0]} read {raw:.3f}')
                continue
            dzs.add(dz)
            if axis == 'LX':
                want = 0 if abs(raw) <= dz else int(max(-1, min(1, (abs(raw) - dz) / (1 - dz))) * 32767 * (1 if raw > 0 else -1))
            else:
                want = 0 if raw <= dz else int(min(1, (raw - dz) / (1 - dz)) * 255 + 0.5)
            seen += 1
            if abs(got - want) > 2:
                bad.append(f'{st[0]}: {got} (want {want} at {dz})')
        wanted = {0.08, 0.13} if axis == 'LX' else {0.02, 0.10}
        ok = seen >= 4 and not bad and wanted <= dzs
        res.add(option, phase, 'the game gets the deadzone the menu set (virtual pad, before and after)',
                'PASS' if ok else 'FAIL' if bad else 'INCONCLUSIVE',
                f'{seen} steps at deadzones {sorted(dzs)}' + (f'; {bad[:2]}' if bad else '') +
                (f"; the virtual pad didn't take {len(stuck)} of its values: {stuck[:2]}" if stuck else ''))

    # Pictures: each change against the captures just before and after it.
    if not paused_scene(res, phase, s):
        return
    picture_results(res, phase, s)
    # The AA pass's GPU time ([perf], NFSMW_GPU_TIMING=1) while it was off.
    off0, on1 = log.find('video.anti_aliasing On -> Off'), log.find('video.anti_aliasing Off -> On')
    perf = [(m.start(), float(m.group(1))) for m in re.finditer(r'post-processing GPU [.\d]+ ms \(AA ([.\d]+) ms\)', log)]
    before = [v for pos, v in perf if pos < off0]
    during = [v for pos, v in perf if off0 < pos < on1][1:]  # the first window has frames from before
    if before and max(before) > 0 and during:
        # (A pass that doesn't run still reads ~0.01 ms: two timestamps back to back.)
        res.add('video.anti_aliasing', phase, 'off: its pass stops running ([perf] AA GPU ms)',
                'PASS' if max(during) < 0.05 and max(before) > 0.1 else 'FAIL', f'{before[-1]:.2f} ms a frame before, {during} while off')
    if (tri := bracket_shots(s, 'aspect_169')):
        r, b, back = tri[:3]
        h, w = b.shape[:2]
        bar = int((w - h * 16 / 9) / 2)
        if bar < 8:
            res.add('video.aspect_ratio', phase, '16:9: bars at the sides', 'INCONCLUSIVE', f'the window is {w}x{h}, not wider than 16:9')
        else:
            edge = int(bar * 0.9)
            side = lambda img: float(luma(img[:, :edge]).mean() + luma(img[:, w - edge:]).mean()) / 2
            ok = side(b) < 1.0 and side(r) > 4.0 and side(back) > 4.0
            res.add('video.aspect_ratio', phase, '16:9: black bars at the sides; Fill Screen again fills them',
                    'PASS' if ok else 'FAIL', f'outer {edge} px columns, mean luma: {side(r):.1f} -> {side(b):.2f} -> {side(back):.1f}')
    unchanged_picture(res, 'video.mipmaps', phase, 'Mipmaps and Internal Resolution changed and put back: '
                      'the picture as with nothing changed', s, 'restart')
    vsync_results(res, phase, log)
    for key in ('video.mipmaps', 'video.internal_resolution'):
        ok = re.search(rf'menu: {re.escape(key)} .* \(at the next launch\)', log)
        res.add(key, phase, 'marked RESTART (stored for the next launch, not applied)', 'PASS' if ok else 'FAIL',
                'logged "at the next launch"; see the menu_restart capture' if ok else 'no deferred change logged')
    if (tri := bracket_shots(s, 'overlay_on')):
        r, b, back = tri[:3]
        h, w = b.shape[:2]
        box = (slice(0, h // 4), slice(0, w // 4))
        d = float(np.abs(b[box] - r[box]).mean())
        n = float(np.abs(back[box] - r[box]).mean())
        res.add('advanced.performance_overlay', phase, 'on: the overlay appears top left; off: gone', 'PASS' if d > max(2.0, 5 * n)
                else 'FAIL', f'top-left quarter mean difference {d:.2f} on, {n:.2f} off again')


def vsync_results(res, phase, log, fps=True):
    modes = re.findall(r'\[video\] present mode (\w+) \(V-Sync (on|off)', log)
    if not any(m[1] == 'off' for m in modes):
        return
    offmode = next(m[0] for m in modes if m[1] == 'off')
    ok = modes[0] == ('FIFO', 'on') and modes[-1] == ('FIFO', 'on')
    res.add('video.vsync', phase, 'off and on from the menu: the present mode follows',
            ('PASS' if offmode != 'FIFO' else 'INCONCLUSIVE') if ok else 'FAIL',
            ' -> '.join(f'{m[0]} ({m[1]})' for m in modes) + (' (the surface offers only FIFO)' if offmode == 'FIFO' else ''))
    perf = [(m.start(), float(m.group(1)), int(m.group(2)), int(m.group(3))) for m in
            re.finditer(r'\[perf\] ([.\d]+) fps.*?display: (\d+) dropped, (\d+) repeated', log)]
    off_at, on_at = log.find('video.vsync On -> Off'), log.find('video.vsync Off -> On')
    before = [f for pos, f, d, r in perf if pos < off_at][-3:]
    during = [(f, d, r) for pos, f, d, r in perf if off_at < pos < on_at][1:]
    if during and fps:
        res.add('video.vsync', phase, 'off: the game keeps its 60 Hz (its own vblank), frames still shown',
                'PASS' if min(f for f, d, r in during) > 55 else 'FAIL',
                f'[perf] fps before {before}, while off {[f for f, d, r in during]} '
                f'(dropped/repeated {[(d, r) for f, d, r in during]})')


def bracket_shots(s, label):
    """Before, changed, after, and where the changed one falls between them."""
    if all(n in s for n in (label + '_ref', label, label + '_back')):
        w = shot_weight(s.run, label) if s.run else 0.5
        return read_ppm(s[label + '_ref']), read_ppm(s[label]), read_ppm(s[label + '_back']), w
    return None


def paused_scene(res, phase, s):
    if 'base0' not in s or 'base1' not in s:
        res.add('(pictures)', phase, 'a paused race to compare in', 'INCONCLUSIVE', 'no baseline captures')
        return False
    a0, a1 = read_ppm(s['base0']), read_ppm(s['base1'])
    keep = still_blocks(a0, a1)
    if keep.mean() < 0.5:
        res.add('(pictures)', phase, 'a paused race to compare in', 'INCONCLUSIVE', f'only {keep.mean():.0%} of the frame is still')
        return False
    res.add('(pictures)', phase, 'a paused race to compare in', 'PASS',
            f'{keep.mean():.0%} of the frame still between two captures; {a0.shape[1]}x{a0.shape[0]}')
    return True


def bracketed(res, option, phase, check, s, label, expect, rows=None):
    """The change's capture against the captures before and after it
    (interpolated to its time), over the blocks those two agree on; the
    margin grows with how much they differ from each other."""
    tri = bracket_shots(s, label)
    if not tri:
        res.add(option, phase, check, 'INCONCLUSIVE', f'captures for {label} missing')
        return
    r, c, b, w = tri
    keep = still_blocks(r, b)
    if keep.mean() < 0.5:
        res.add(option, phase, check, 'INCONCLUSIVE', f'the scene moved around the change ({keep.mean():.0%} still)')
        return
    er, ec, eb = lap_energy(r, keep, rows), lap_energy(c, keep, rows), lap_energy(b, keep, rows)
    noise = abs(eb / er - 1)
    margin = max(0.02, 4 * noise)
    ratio = ec / (er + w * (eb - er))
    # The way it should go, beyond the drift of the two captures around it;
    # and either by a clear margin or with many blocks changed beyond the
    # no-change control (a small but real effect over the whole frame).
    toward = ratio < 1 - noise if expect == 'down' else ratio > 1 + noise
    clear = ratio < 1 - margin if expect == 'down' else ratio > 1 + margin
    frac = changed_blocks(s, label, keep, rows)
    ok = toward and (clear or (frac is not None and frac > 0.05))
    # The right way, past the drift, but by less than the margin: a small
    # effect here (sharpening where the frame is about the window's size).
    weak = toward and not ok
    res.add(option, phase, check, 'PASS' if ok else 'INCONCLUSIVE' if weak else 'FAIL',
            f'detail energy x{ratio:.3f} (want {"<" if expect == "down" else ">"} {1 - margin if expect == "down" else 1 + margin:.3f}, '
            f'or past {1 - noise if expect == "down" else 1 + noise:.3f} with over 5% of blocks changed: '
            f'{"?" if frac is None else f"{frac:.1%}"}); {er:.2f} -> {ec:.2f} -> {eb:.2f} back; '
            f'put back: mean difference {masked_diff(b, r, keep):.2f}')


def changed_blocks(s, label, keep, rows=None):
    """The share of blocks the change moved beyond the no-change control's
    99th percentile (1% by chance), or None without the control."""
    tri, ctrl = bracket_shots(s, label), bracket_shots(s, 'noop')
    if not tri or not ctrl:
        return None
    keep = keep & still_blocks(ctrl[0], ctrl[2])
    rows = rows or (0.0, 1.0)
    rc, r0 = block_residuals(tri, keep, rows), block_residuals(ctrl, keep, rows)
    if not len(rc) or not len(r0):
        return None
    return float((rc > np.percentile(r0, 99)).mean())


def unchanged_picture(res, option, phase, check, s, label):
    """The opposite of bracketed: what a restart option would change live
    (the internal resolution's detail, mip levels' aliasing) would move the
    detail energy; it stays within the drift of the captures around it. (A
    count of changed blocks can't tell: the longer a bracket, the more the
    paused scene's shadows and lights move.)"""
    tri = bracket_shots(s, label)
    if not tri:
        res.add(option, phase, check, 'INCONCLUSIVE', f'captures for {label} missing')
        return
    r, c, b, w = tri
    keep = still_blocks(r, b)
    er, ec, eb = lap_energy(r, keep), lap_energy(c, keep), lap_energy(b, keep)
    noise = abs(eb / er - 1)
    ratio = ec / (er + w * (eb - er))
    frac = changed_blocks(s, label, keep)
    res.add(option, phase, check, 'PASS' if abs(ratio - 1) < max(0.02, 3 * noise) else 'FAIL',
            f'detail energy x{ratio:.3f} (want within {max(0.02, 3 * noise):.3f} of 1); {er:.2f} -> {ec:.2f} -> {eb:.2f}; '
            f'{"?" if frac is None else f"{frac:.1%}"} of blocks moved past the control\'s 99th percentile')


def block_residuals(tri, keep, rows):
    """Per 16x16 block, how far the middle capture is from the two around it
    interpolated to its time (a drift linear in time cancels), over the kept
    blocks of a band of rows."""
    r, c, b, w = tri
    d = np.abs(c - (r + w * (b - r))).max(axis=2)
    h, w = keep.shape[0] * BLOCK, keep.shape[1] * BLOCK
    blocks = d[:h, :w].reshape(keep.shape[0], BLOCK, keep.shape[1], BLOCK).mean(axis=(1, 3))
    k = keep.copy()
    k[:int(k.shape[0] * rows[0])] = False
    k[int(k.shape[0] * rows[1]):] = False
    return blocks[k]


def changes_picture(res, option, phase, check, s, label, rows, strict):
    """Direction-free: the change either moves many blocks beyond the
    no-change control (some surfaces change a lot) or moves the band's
    detail energy well past the drift (every surface a little). Not strict:
    undetected is inconclusive (at a high internal resolution supersampling
    hides most of what anisotropic filtering does)."""
    tri = bracket_shots(s, label)
    if not tri:
        res.add(option, phase, check, 'INCONCLUSIVE', f'captures for {label} missing')
        return
    keep = still_blocks(tri[0], tri[2])
    frac = changed_blocks(s, label, keep, rows)
    er, ec, eb = (lap_energy(x, keep, rows) for x in tri[:3])
    noise = abs(eb / er - 1)
    ratio = ec / (er + tri[3] * (eb - er))
    margin = max(0.03, 4 * noise)
    ok = (frac is not None and frac > 0.05) or abs(ratio - 1) > margin
    res.add(option, phase, check, 'PASS' if ok else 'FAIL' if strict else 'INCONCLUSIVE',
            f'{"?" if frac is None else f"{frac:.1%}"} of blocks changed beyond the no-change control\'s 99th percentile '
            f'(1% by chance; want over 5%), or detail energy x{ratio:.3f} (want off 1 by over {margin:.3f})')


def picture_results(res, phase, s, strict_aniso=False):
    bracketed(res, 'video.anti_aliasing', phase, 'off: more detail energy (jagged edges)', s, 'aa_off', 'up')
    bracketed(res, 'video.scaling', phase, 'bilinear: softer than bicubic', s, 'bilinear', 'down')
    bracketed(res, 'video.sharpening', phase, '1.00: sharper than 0.30', s, 'sharpen_1', 'up')
    bracketed(res, 'video.sharpening', phase, '0.00: softer than 0.30', s, 'sharpen_0', 'down')
    # Anisotropy sharpens mipmapped surfaces but smooths aliasing on ones
    # without mips, so the direction of the detail energy isn't fixed: it
    # must change the ground and walls, beyond the control.
    changes_picture(res, 'video.anisotropic_filtering', phase, 'Auto -> Off changes the surfaces in the distance', s, 'aniso_off',
                    (0.35, 0.8), strict_aniso)
    changes_picture(res, 'video.anisotropic_filtering', phase, 'Off -> 16x changes the surfaces in the distance', s, 'aniso_16',
                    (0.35, 0.8), strict_aniso)


def analyze_upscale(res, run, log):
    phase = 'upscale'
    check_navigation(res, phase, run, log)
    m = re.search(r'internal resolution (\S+) \((\S+),', log)
    res.add('(pictures)', phase, 'rendered at 1x (NFSMW_RESOLUTION=1x)', 'PASS' if m and m.group(1) == '1x1' else 'INCONCLUSIVE',
            m.group(0) if m else 'no internal resolution line')
    s = shots(run)
    if paused_scene(res, phase, s):
        picture_results(res, phase, s, strict_aniso=bool(m and m.group(1) == '1x1'))


def rumble_results(res, phase, log, to):
    """Vibration switched to `to` in the menu, then driving: what the game
    asked for and what the (virtual) controller got."""
    at = log.find(f"input.vibration {'On' if to == 'Off' else 'Off'} -> {to}")
    check = 'on: the rumble the game asks for reaches the controller' if to == 'On' else \
        'off: the controller gets nothing while the game asks for rumble'
    if at < 0:
        res.add('input.vibration', phase, check, 'FAIL', 'the change isn\'t logged')
        return
    asked = [m for m in re.finditer(r'\[input\] rumble: game low (\d+), high (\d+); Vibration (on|off)', log)
             if m.start() > at and (m.group(1) != '0' or m.group(2) != '0')]
    got = [m.groups() for m in re.finditer(r'\[input\] virtual pad: rumble low (\d+), high (\d+)', log) if m.start() > at]
    nonzero = [g for g in got if g != ('0', '0')]
    if not asked:
        res.add('input.vibration', phase, check, 'INCONCLUSIVE', 'no rumble requested after the change')
    elif to == 'On':
        res.add('input.vibration', phase, check, 'PASS' if nonzero else 'FAIL',
                f'{len(asked)} requests after the change; the controller got {len(nonzero)} nonzero, e.g. {nonzero[:3]}')
    else:
        res.add('input.vibration', phase, check, 'PASS' if not nonzero else 'FAIL',
                f'{len(asked)} requests after the change; the controller got {got[:2] or "nothing"}'
                + (f', nonzero {nonzero[:2]}' if nonzero else ''))
    # Every rumble the controller got, against the game's last request.
    last, count, wrong = None, 0, []
    for m in re.finditer(r'\[input\] (?:rumble: game low (\d+), high (\d+); Vibration (on|off)|virtual pad: rumble low (\d+), high (\d+))', log):
        if m.group(1) is not None:
            last = m.groups()[:3]
            continue
        count += 1
        want = ('0', '0') if last is None or last[2] == 'off' else last[:2]
        if m.groups()[3:] != want:
            wrong.append(f'{m.groups()[3:]} for {last}')
    if count:
        res.add('input.vibration', phase, 'every rumble the controller got is the game\'s last request (0 while off)',
                'PASS' if not wrong else 'FAIL', f'{count} changes' + (f'; {wrong[:2]}' if wrong else ''))


def analyze_relaunch(res, run, log, out):
    phase = 'relaunch'
    check_navigation(res, phase, run, log)
    m = re.search(r'\[settings\] .+: (\d+) values? from the file', log)
    res.add('(persistence)', phase, 'the saved settings.toml loads at the next launch', 'PASS' if m and int(m.group(1)) >= 17 else 'FAIL',
            f'{m.group(1)} values from the file' if m else 'not loaded')
    ok = 'stick deadzone 0.13, trigger deadzone 0.10' in log
    for option in ('input.stick_deadzone', 'input.trigger_deadzone'):
        res.add(option, phase, 'the stored value applies at launch', 'PASS' if ok else 'FAIL',
                'log at launch: stick deadzone 0.13, trigger deadzone 0.10' if ok else 'not in the log')
    ok = 'input.vibration Off -> On' in log
    res.add('input.vibration', phase, 'the stored Off shows in the menu at the next launch', 'PASS' if ok else 'FAIL',
            'the toggle went Off -> On' if ok else 'not seen')
    rumble_results(res, phase, log, 'On')
    # Deferred means stored != running: the running value is the stored Off.
    ok = re.search(r'menu: video\.mipmaps Off -> On \(at the next launch\)', log)
    res.add('video.mipmaps', phase, 'the stored Off is what runs (the menu shows Off; On waits for a restart)',
            'PASS' if ok else 'FAIL', 'logged "Off -> On (at the next launch)"; see the menu_mips capture' if ok else 'not seen')
    if (out / 'video' / 'run.log').exists():
        mips_by_load(res, phase, 'the texture cache loads base levels only (Off, from settings.toml)', log,
                     (out / 'video' / 'run.log').read_text(errors='replace'), 'the video phase, On', 'less')


def world_load(log):
    """The frame that loaded the most textures (the race's world, the same
    few hundred every launch): its [hitch] report's texture count and KB."""
    loads = [(int(n), int(kb)) for n, kb in re.findall(r'textures (\d+) \([.\d]+ ms, (\d+) KB', log)]
    return max(loads) if loads else None


def mips_by_load(res, phase, check, log, ref_log, ref_name, want):
    """Mipmaps as the texture cache applied them: the same world load's bytes
    per texture against another launch's (base levels only: ~3/4). `want`:
    'less' or 'more' than the other launch."""
    a, b = world_load(log), world_load(ref_log)
    if not a or not b or abs(a[0] - b[0]) > 0.05 * b[0]:
        res.add('video.mipmaps', phase, check, 'INCONCLUSIVE', f'world loads {a} and {b} ({ref_name}) not comparable '
                '(the load can be split over frames, or its [hitch] report skipped)')
        return False
    ratio = (a[1] / a[0]) / (b[1] / b[0])
    ok = ratio < 0.9 if want == 'less' else ratio > 1.1
    res.add('video.mipmaps', phase, check, 'PASS' if ok else 'FAIL',
            f'world load: {a[0]} textures, {a[1]} KB here; {b[0]} textures, {b[1]} KB in {ref_name}: x{ratio:.3f} per texture')
    return True


def matched_energy(a, a1, b, b1, rows):
    """Detail energy of two launches' paused frames over the blocks both
    launches show alike (the same content, still in each) in a band of rows."""
    keep = still_blocks(a, a1) & still_blocks(b, b1)
    ya, yb = luma(a), luma(b)
    h, w = keep.shape[0] * BLOCK, keep.shape[1] * BLOCK
    ma = ya[:h, :w].reshape(keep.shape[0], BLOCK, keep.shape[1], BLOCK).mean(axis=(1, 3))
    mb = yb[:h, :w].reshape(keep.shape[0], BLOCK, keep.shape[1], BLOCK).mean(axis=(1, 3))
    keep &= np.abs(ma - mb) < 4.0
    return lap_energy(a, keep, rows), lap_energy(b, keep, rows), float(keep[int(keep.shape[0] * rows[0]):int(keep.shape[0] * rows[1])].mean())


def analyze_relaunch2(res, run, log, out):
    phase = 'relaunch2'
    check_navigation(res, phase, run, log)
    vals = toml_values(run / 'settings.before.toml')
    res.add('video.mipmaps', phase, 'the stored On is in settings.toml for this launch', 'PASS' if vals.get('video.mipmaps') == 'true'
            else 'FAIL', f'mipmaps = {vals.get("video.mipmaps")}')
    if (out / 'relaunch' / 'run.log').exists():
        mips_by_load(res, phase, 'mip levels load again (On, from settings.toml): more bytes than the Off launch', log,
                     (out / 'relaunch' / 'run.log').read_text(errors='replace'), 'relaunch, Off', 'more')
    off, on = shots(out / 'relaunch'), shots(run)
    if not all(n in d for d in (off, on) for n in ('base0', 'base1')):
        res.add('video.mipmaps', phase, 'off vs on at launch: distant textures', 'INCONCLUSIVE', 'captures missing')
        return
    a, a1, b, b1 = (read_ppm(d[n]) for d in (off, on) for n in ('base0', 'base1'))
    if a.shape != b.shape:
        res.add('video.mipmaps', phase, 'off vs on at launch: distant textures', 'INCONCLUSIVE', 'different sizes')
        return
    # The far part of the picture (above the middle), where textures are minified.
    e_off, e_on, share = matched_energy(a, a1, b, b1, (0.2, 0.55))
    if share < 0.6:
        res.add('video.mipmaps', phase, 'off vs on at launch: distant textures', 'INCONCLUSIVE',
                f'the two launches paused on different frames ({share:.0%} of the band alike)')
        return
    ratio = e_off / e_on
    res.add('video.mipmaps', phase, 'the launch with Mipmaps off (relaunch) aliases in the distance: more detail energy than on',
            'PASS' if ratio > 1.03 else 'FAIL', f'x{ratio:.3f} off/on over the {share:.0%} of the far band both launches show alike')


def analyze_relaunch3(res, run, log):
    phase = 'relaunch3'
    m = re.search(r'internal resolution (\S+) \((\S+), (.+?);', log)
    ok = m and m.group(1) == '1x1' and m.group(3) == 'setting'
    res.add('video.internal_resolution', phase, 'the stored 1x applies at the next launch', 'PASS' if ok else 'FAIL',
            m.group(0) if m else 'no internal resolution line')


def analyze_audio(res, run, log):
    phase = 'audio'
    check_navigation(res, phase, run, log)
    levels = run / 'levels.txt'
    if not levels.exists():
        res.add('audio.master_volume', phase, 'disk-driver output', 'INCONCLUSIVE', 'no audio recorded')
        return
    rows = np.array([[float(x) for x in line.split()] for line in levels.read_text().splitlines() if not line.startswith('#')])
    n = int(min((rows[:, 1] >= 0).sum(), (rows[:, 2] >= 0).sum()))
    outr, mixr = rows[:n, 1], rows[:n, 2]
    # The device lags the mix by its queue: find it from the envelopes.
    best, lag = -1.0, 0
    for k in range(0, 20):
        a, b = outr[k:], mixr[:n - k]
        if len(a) > 100 and a.std() > 0 and b.std() > 0:
            c = float(np.corrcoef(a, b)[0, 1])
            if c > best:
                best, lag = c, k
    outr = outr[lag:]
    mixr = mixr[:len(outr)]
    # The gain the game set, over the output's position (logged with it).
    changes = [(float(t), float(g), text) for g, text, t in
               re.findall(r'\[audio\] gain ([.\d]+) \((.*?)\) from ([.\d]+) s of output', log)]
    if not changes:
        res.add('audio.master_volume', phase, 'gain changes logged', 'FAIL', 'no [audio] gain lines')
        return
    t = np.arange(len(outr)) * 0.05
    gain = np.full(len(outr), np.nan)
    for i, (start, g, _) in enumerate(changes):
        end = changes[i + 1][0] if i + 1 < len(changes) else 1e9
        sel = (t >= start + 0.3) & (t < end - 0.1)  # clear of the change
        gain[sel] = g
    loud = mixr > 0.003
    ratio = np.where(loud, outr / np.maximum(mixr, 1e-9), np.nan)
    ref = np.nanmedian(ratio[(gain == 1.0) & loud])

    def seg(label):
        # The segment after the change whose text says `label`.
        for i, (start, g, text) in enumerate(changes):
            if label(text, g):
                end = changes[i + 1][0] if i + 1 < len(changes) else t[-1]
                sel = (t >= start + 0.3) & (t < end - 0.1) & loud
                return g, sel
        return None, None

    g, sel = seg(lambda text, g: text == 'volume 50%')
    if sel is None or not sel.any():
        res.add('audio.master_volume', phase, '50%: output at 0.25 of the mix', 'INCONCLUSIVE', 'no loud window at 50%')
    else:
        r = float(np.nanmedian(ratio[sel]) / ref)
        res.add('audio.master_volume', phase, '50% (gain 0.25, squared): measured output level against the game mix',
                'PASS' if abs(r - 0.25) < 0.04 else 'FAIL', f'x{r:.3f} of the 100% level over {int(sel.sum()) * 0.05:.1f} s (want 0.25)')
    g, sel = seg(lambda text, g: 'muted' in text and 'background' not in text)
    if sel is None or not sel.any():
        res.add('audio.mute', phase, 'on: silence', 'INCONCLUSIVE', 'no loud window while muted')
    else:
        peak = float(outr[sel].max())
        res.add('audio.mute', phase, 'on: the output is silent while the game plays sound', 'PASS' if peak < 1e-4 else 'FAIL',
                f'max output rms {peak:.6f} over {int(sel.sum()) * 0.05:.1f} s; mix rms {float(mixr[sel].mean()):.4f}')
    g, sel = seg(lambda text, g: 'background' in text)
    if sel is None or not sel.any():
        res.add('audio.mute_in_background', phase, 'on + focus lost: silence', 'INCONCLUSIVE' if 'window focus lost' in log else 'FAIL',
                'no muted-in-background window')
    else:
        peak = float(outr[sel].max())
        res.add('audio.mute_in_background', phase, 'on, window loses focus: silent', 'PASS' if peak < 1e-4 else 'FAIL',
                f'max output rms {peak:.6f} over {int(sel.sum()) * 0.05:.1f} s')
    # Off, focus lost again: nothing changes the gain, so no log line marks
    # that stretch of the output. It is placed from the keys' plan (no Shot
    # before it, so the keys kept their times), anchored on the first focus
    # loss, whose mute is logged with its output position; then measured.
    keys = []
    for line in (run / 'plan.txt').read_text().splitlines():
        if line.startswith('NFSMW_UI_KEYS='):
            keys = [(float(k.split(':', 1)[0]), k.split(':', 1)[1]) for k in line[14:].split(',') if ':' in k]
    focus = [t for t, k in keys if k in ('FocusLost', 'FocusGained')]
    muted_bg = [start for start, g, text in changes if 'background' in text]
    last_lost = [m.start() for m in re.finditer(r'window focus lost', log)]
    if len(focus) >= 4 and muted_bg and len(last_lost) >= 2:
        logged = 'muted in the background' in log[last_lost[-1]:]
        sel = (t >= muted_bg[0] + focus[2] - focus[0] + 0.5) & (t < muted_bg[0] + focus[3] - focus[0] - 0.3) & loud
        if not sel.any():
            res.add('audio.mute_in_background', phase, 'off, window loses focus: still heard', 'FAIL' if logged else 'INCONCLUSIVE',
                    'muted anyway (logged)' if logged else 'no loud window while the focus was away')
        else:
            r = float(np.nanmedian(ratio[sel]) / ref)
            res.add('audio.mute_in_background', phase, 'off, window loses focus: still heard (measured)',
                    'PASS' if abs(r - 1) < 0.1 and not logged else 'FAIL',
                    f'x{r:.3f} of the 100% level over {int(sel.sum()) * 0.05:.1f} s' + ('; a background mute logged' if logged else ''))
    # Mute off again: the stretch after the first return to gain 1 that follows it.
    after = [i for i, c in enumerate(changes) if c[2].endswith(', muted')]
    if after and after[0] + 1 < len(changes) and changes[after[0] + 1][1] == 1.0:
        i = after[0] + 1
        end = changes[i + 1][0] if i + 1 < len(changes) else t[-1]
        sel = (t >= changes[i][0] + 0.3) & (t < end - 0.1) & loud
        r = float(np.nanmedian(ratio[sel]) / ref) if sel.any() else float('nan')
        res.add('audio.mute', phase, 'off again: full level (measured)', 'PASS' if abs(r - 1) < 0.1 else
                'INCONCLUSIVE' if not sel.any() else 'FAIL', f'x{r:.3f} of the 100% level over {int(sel.sum()) * 0.05:.1f} s')
    else:
        res.add('audio.mute', phase, 'off again: full level', 'FAIL', 'no return to gain 1 logged after the mute')
    # The environment override.
    ok = 'NFSMW_SHARPEN overrides video.sharpening' in log and 'sharpen 0.80' in log
    after = toml_values(run / 'settings.after.toml').get('video.sharpening')
    res.add('(env override)', phase, 'NFSMW_SHARPEN=0.8 wins for the run; the menu edit is saved, not 0.8',
            'PASS' if ok and after == '0.35' else 'FAIL', f'applied 0.80: {ok}; saved sharpening = {after} (menu: ENV marker in menu_env)')


def analyze_corrupt(res, run, log):
    phase = 'corrupt'
    parsed = "doesn't parse" in log
    bad = (run / 'settings.after.bad')
    kept = bad.exists() and bad.read_text() == CORRUPT_TOML
    res.add('(corrupt file)', phase, 'logged, kept as settings.toml.bad', 'PASS' if parsed and kept else 'FAIL',
            f'parse error logged {parsed}, .bad identical {kept}')
    ok = 'anti-aliasing, bicubic scaling, sharpen 0.30' in log
    res.add('(corrupt file)', phase, 'every option at its default (anti_aliasing = false in it ignored)', 'PASS' if ok else 'FAIL',
            'post-processing at defaults' if ok else 'not at defaults')
    s = shots(run)
    if 'toast' in s and 'after_toast' in s:
        # The toast is a near-opaque dark blue-grey panel (ui.cpp's WindowBg)
        # at the bottom centre, whatever the game shows behind it.
        def panel(img):
            h, w = img.shape[:2]
            box = img[int(h * 0.75):, int(w * 0.10):int(w * 0.90)]
            r, g, bl = box[..., 0], box[..., 1], box[..., 2]
            return float(((bl > 12) & (bl < 45) & (bl - r > 4) & (bl - g > 2) & (r < 35)).mean())
        pa, pb = panel(read_ppm(s['toast'])), panel(read_ppm(s['after_toast']))
        res.add('(corrupt file)', phase, 'a toast at the bottom of the screen, gone after 12 s', 'PASS' if pa > 0.05 and pb < pa / 5
                else 'FAIL', f'toast-panel pixels in the bottom quarter {pa:.3f} at 5 s -> {pb:.3f} at 20 s')
    else:
        res.add('(corrupt file)', phase, 'toast', 'INCONCLUSIVE', 'captures missing')


def analyze_async(res, out):
    phase = 'async'
    on, off = out / 'async_on' / 'run.log', out / 'async_off' / 'run.log'
    if not on.exists() or not off.exists():
        return
    lon, loff = on.read_text(errors='replace'), off.read_text(errors='replace')
    changed = 'advanced.async_shaders On -> Off (applied now)' in loff
    skip_on = sum(int(x) for x in re.findall(r'\[pipelines\] frame \d+: (\d+) draws skipped', lon))
    skip_off = sum(int(x) for x in re.findall(r'\[pipelines\] frame \d+: (\d+) draws skipped', loff))
    wait_on = sum(float(x) for x in re.findall(r'pipelines \d+ \(([.\d]+) ms\)', lon))
    wait_off = sum(float(x) for x in re.findall(r'pipelines \d+ \(([.\d]+) ms\)', loff))
    if not changed:
        res.add('advanced.async_shaders', phase, 'off from the menu', 'FAIL', 'the change isn\'t logged')
    elif skip_on == 0:
        res.add('advanced.async_shaders', phase, 'off: no draw skipped for a compiling pipeline', 'INCONCLUSIVE',
                'the run left on skipped none either (a warm cache?)')
    else:
        res.add('advanced.async_shaders', phase, 'off: no draw skipped for a compiling pipeline (cold caches)',
                'PASS' if skip_off == 0 else 'FAIL', f'draws skipped: on {skip_on}, off {skip_off}; '
                f'pipeline waits in hitch reports: on {wait_on:.0f} ms, off {wait_off:.0f} ms')


def analyze_window(res, run, log):
    phase = 'window'
    check_navigation(res, phase, run, log)
    s = shots(run)
    if 'fullscreen' in s:
        size = {k: read_ppm(v).shape[:2] for k, v in s.items() if k in ('windowed', 'fullscreen', 'windowed_again')}
        text = lambda k: f'{size[k][1]}x{size[k][0]}' if k in size else '?'
        swaps = re.findall(r'\[video\] (?:\S+ \(.*\), )?swapchain (\d+x\d+)', log)
        ok = 'fullscreen on (setting)' in log and 'fullscreen' in size and size['fullscreen'] != size.get('windowed')
        res.add('video.fullscreen', phase, 'on from the menu: the window fills the screen', 'PASS' if ok else 'FAIL',
                f'captures {text("windowed")} -> {text("fullscreen")}; swapchains {swaps}')
        ok = 'fullscreen off (setting)' in log and size.get('windowed_again') == size.get('windowed')
        # gamescope's window manager doesn't give a window back its size
        # when it leaves fullscreen (and shows every window scaled anyway).
        gamescope = 'GAMESCOPE' in (run / 'plan.txt').read_text()
        res.add('video.fullscreen', phase, 'off again: back to the window',
                'PASS' if ok else 'INCONCLUSIVE' if gamescope else 'FAIL',
                f'captures {text("fullscreen")} -> {text("windowed_again")}' +
                ('' if ok or not gamescope else " (under gamescope, whose window manager doesn't restore the size)"))
    # (The title screen and attract movies run at 30 fps: no frame-rate check here.)
    vsync_results(res, phase, log, fps=False)


def analyze_gamemode(res, run, log):
    phase = 'gamemode'
    # No change is planned: one logged means the locked row changed.
    check_navigation(res, phase, run, log)
    m = re.search(r'^\[video\] screen .*$', log, re.M)
    ok = bool(m) and '(fullscreen)' in m.group(0) and 'Game Mode' in m.group(0)
    res.add('video.fullscreen', phase, 'Game Mode: the window starts fullscreen (none stored)', 'PASS' if ok else 'FAIL',
            m.group(0) if m else 'no [video] screen line')
    stored = toml_values(run / 'settings.after.toml').get('video.fullscreen', 'false (not saved)')
    locked = not any(g[0] == 'video.fullscreen' for g in menu_lines(log)) and stored.startswith('false')
    res.add('video.fullscreen', phase, 'Game Mode: the row stays On; Left, Right, Enter change nothing',
            'PASS' if locked and 'menu_locked' in shots(run) else 'FAIL',
            f'no change logged: {locked}; stored fullscreen = {stored}; see the menu_locked capture')


def analyze(out):
    res = Results()
    runs = {p.name: p for p in out.iterdir() if (p / 'run.log').exists()}
    for name, run in runs.items():
        log = (run / 'run.log').read_text(errors='replace')
        for crash in re.findall(r'.*(?:\[crash\]|Assertion|terminate called).*', log)[:1]:
            res.add('(stability)', name, 'no crash', 'FAIL', crash[:120])
        if name == 'video': analyze_video(res, run, log)
        elif name == 'upscale': analyze_upscale(res, run, log)
        elif name == 'relaunch': analyze_relaunch(res, run, log, out)
        elif name == 'relaunch2': analyze_relaunch2(res, run, log, out)
        elif name == 'relaunch3': analyze_relaunch3(res, run, log)
        elif name == 'audio': analyze_audio(res, run, log)
        elif name == 'corrupt': analyze_corrupt(res, run, log)
        elif name == 'window': analyze_window(res, run, log)
        elif name == 'gamemode': analyze_gamemode(res, run, log)
    analyze_async(res, out)
    width = max((len(r[0]) for r in res.rows), default=10)
    with open(out / 'results.tsv', 'w') as f:
        f.write('option\tphase\tcheck\tresult\tevidence\n')
        for r in res.rows:
            f.write('\t'.join(r) + '\n')
            print(f'{r[3]:<12} {r[0]:<{width}} {r[1]:<9} {r[2]}: {r[4]}')
    fails = sum(r[3] == 'FAIL' for r in res.rows)
    print(f'{len(res.rows)} checks: {sum(r[3] == "PASS" for r in res.rows)} PASS, {fails} FAIL, '
          f'{sum(r[3] == "INCONCLUSIVE" for r in res.rows)} INCONCLUSIVE ({out / "results.tsv"})')
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', default=str(ROOT / 'build/main/runtime/SpeedBreaker'))
    ap.add_argument('--out', default=str(ROOT / 'build/verify-settings'))
    ap.add_argument('--phases', default='video,relaunch,relaunch2,relaunch3,upscale,audio,corrupt,async_on,async_off' +
                    (',window' if MAC else ',gamemode'))
    ap.add_argument('--game-dir', default=os.environ.get('NFSMW_GAME_DIR') or (str(ROOT / 'game/files') if (ROOT / 'game/files').is_dir()
                    else None), help='extracted game files (default: NFSMW_GAME_DIR, else the repo\'s game/files); '
                    'each phase\'s profile is new, so an install in the player\'s profile isn\'t found')
    ap.add_argument('--cache', help='a warm cache folder to start each run from (the contents of a SpeedBreaker cache folder)')
    ap.add_argument('--keep-cache', action='store_true', help="keep each run's cache (its shaders and pipelines)")
    ap.add_argument('--display', action='store_true', help='Linux: use the current display instead of offscreen')
    ap.add_argument('--fullscreen', action='store_true', help='window phase: also toggle fullscreen')
    ap.add_argument('--analyze', metavar='OUT', help='only analyze the runs already in OUT')
    ap.add_argument('--print-plan', metavar='PHASE', help='print the environment a phase sets, and exit')
    args = ap.parse_args()
    if args.print_plan:
        plan = phase_plan(args.print_plan, args)
        print(f'NFSMW_INPUT_SCRIPT={plan["script"]}\nNFSMW_UI_KEYS={plan["keys"].env()}\nNFSMW_VIRTUAL_PAD={plan.get("pad", "")}\n'
              f'shots={",".join(plan["keys"].shots)}\nsecs={plan["secs"]:.0f}')
        return 0
    if np is None:
        raise SystemExit('needs numpy (pip install numpy, or the distro package)')
    if args.analyze:
        return 1 if analyze(pathlib.Path(args.analyze).resolve()) else 0
    out = pathlib.Path(args.out).resolve()
    # Each phase deletes its folders under --out (video, audio, profiles,
    # caches, ...) before it runs: never in a folder that isn't ours.
    mark = out / '.verify-settings'
    if out.is_dir() and any(out.iterdir()) and not mark.exists():
        raise SystemExit(f'{out} has files of its own: give --out a new or empty folder')
    out.mkdir(parents=True, exist_ok=True)
    mark.touch()
    for name in args.phases.split(','):
        if name == 'window' and not MAC and not args.display:
            print('[window] skipped: needs --display (see the top of this file)')
            continue
        if name == 'gamemode' and MAC and not args.fullscreen:
            print('[gamemode] skipped: on macOS it takes over the screen (--fullscreen to run it)')
            continue
        run_phase(name, args)
    return 1 if analyze(out) else 0


if __name__ == '__main__':
    sys.exit(main())
