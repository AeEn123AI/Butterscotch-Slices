#!/usr/bin/env python3
"""Dynamic tests for the client/server architecture.

These start real server and client processes (clients render with OpenGL on private Xvfb displays), drive them with
scripted input (input recordings on the server, xdotool key presses on the clients) and compare what the clients drew
with a standalone run of the same game.

Requirements: Xvfb, xdotool (for the real-keyboard tests), Python with Pillow + numpy, and copies of the commercial
games (they are not part of the repository):

    BUTTERSCOTCH=build/butterscotch UNDERTALE=/path/to/Undertale/data.win DELTARUNE=/path/to/DELTARUNE/data.win \
        python3 tests/net/net_tests.py [equivalence] [multiclient] [latency] [bandwidth] [latejoin] [gamechange] [launcher]

With no arguments every scenario whose game is available runs. Each scenario prints PASS/FAIL; the exit code is the
number of failures.
"""
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image

BIN = os.path.abspath(os.environ.get('BUTTERSCOTCH', 'build/butterscotch'))
UNDERTALE = os.environ.get('UNDERTALE', os.path.expanduser('~/Undertale/data.win'))
DELTARUNE = os.environ.get('DELTARUNE', os.path.expanduser('~/DELTARUNE/data.win'))
PTBR_RECORDING = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'undertale-ptbr-winpack', 'gameplay.json')

WORK = tempfile.mkdtemp(prefix='bs-net-tests-')
procs = []
failures = 0


# ===[ helpers ]===

def cleanup():
    for p in procs:
        try:
            p.terminate()
        except Exception:
            pass
    time.sleep(0.3)
    for p in procs:
        try:
            p.kill()
        except Exception:
            pass
    procs.clear()


def spawn(cmd, log, env=None):
    f = open(log, 'wb')
    p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=os.path.dirname(log))
    procs.append(p)
    return p


class Display:
    """A private Xvfb display."""

    def __init__(self, workdir, size='1024x768x24'):
        r, w = os.pipe()
        self.proc = subprocess.Popen(['Xvfb', '-displayfd', str(w), '-screen', '0', size, '-nolisten', 'tcp'], pass_fds=(w,),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(w)
        procs.append(self.proc)
        self.name = ':' + os.read(r, 16).decode().strip()
        os.close(r)
        self.env = dict(os.environ, DISPLAY=self.name)

    def key(self, *keys, delay=40):
        subprocess.run(['xdotool', 'key', '--delay', str(delay)] + list(keys), env=self.env, check=False)


class Server:
    def __init__(self, name, game, extra=(), env=None):
        self.dir = os.path.join(WORK, name)
        os.makedirs(self.dir, exist_ok=True)
        self.portfile = os.path.join(self.dir, 'port.txt')
        self.log = os.path.join(self.dir, 'server.log')
        cmd = [BIN, '--server', game, '--port', '0', '--port-file', self.portfile, '--save-folder',
               os.path.join(self.dir, 'save')] + list(extra)
        self.proc = spawn(cmd, self.log, env)
        for _ in range(300):
            if os.path.exists(self.portfile) and open(self.portfile).read().strip():
                break
            time.sleep(0.1)
        self.port = int(open(self.portfile).read().strip())

    def text(self):
        return open(self.log, errors='replace').read()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def client(name, port, display, extra=(), game=None, netsim=()):
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    cmd = [BIN, '--connect', '127.0.0.1:%d' % port, '--name', name] + list(extra) + list(netsim)
    if game:
        cmd.append(game)
    log = os.path.join(d, 'client.log')
    p = spawn(cmd, log, dict(os.environ, DISPLAY=display.name))
    p.log = log
    return p


def wait_exit(p, timeout):
    try:
        return p.wait(timeout)
    except subprocess.TimeoutExpired:
        return None


def shots_args(prefix, frames):
    args = ['--screenshot', prefix + '%d.png']
    for f in frames:
        args += ['--screenshot-at-frame', str(f)]
    return args


def compare(a, b):
    A = np.asarray(Image.open(a).convert('RGB')).astype(int)
    B = np.asarray(Image.open(b).convert('RGB')).astype(int)
    if A.shape != B.shape:
        return None
    return int((np.abs(A - B).sum(axis=2) > 0).sum())


def stat(logtext, pattern, group=1):
    m = re.search(pattern, logtext)
    return float(m.group(group)) if m else None


def report(name, ok, detail=''):
    global failures
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  (' + detail + ')') if detail else ''), flush=True)
    if not ok:
        failures += 1


def make_gameplay(path, frames=6000):
    """Input recording for Undertale: the bundled PT-BR recording reaches the name confirmation, then we confirm it,
    mash Enter through the dialogue and walk around randomly. Deterministic."""
    d = json.load(open(PTBR_RECORDING))
    out = {str(f): {'keysPressed': [], 'keysReleased': []} for f in range(frames)}
    out.update(d)

    def press(f, key, dur=2):
        out[str(f)]['keysPressed'].append(key)
        out[str(f + dur)]['keysReleased'].append(key)

    press(740, 13)
    for f in range(800, frames - 100, 22):
        press(f, 13)
    rnd = random.Random(5)
    f = 1200
    while f < frames - 200:
        k = rnd.choice([40, 39, 38, 37])
        dur = rnd.randint(20, 70)
        out[str(f)]['keysPressed'].append(k)
        out[str(f + dur)]['keysReleased'].append(k)
        f += dur + rnd.randint(2, 15)
    json.dump(out, open(path, 'w'))


def make_zspam(path, frames=3000):
    out = {str(f): {'keysPressed': [], 'keysReleased': []} for f in range(frames)}
    for f in range(60, frames, 40):
        out[str(f)]['keysPressed'].append(90)
        out[str(f + 2)]['keysReleased'].append(90)
    json.dump(out, open(path, 'w'))


def standalone_shots(name, game, input_json, frames, speed=8):
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    disp = Display(d)
    cmd = [BIN, game, '--seed', '1', '--speed', str(speed), '--playback-inputs', input_json, '--save-folder', os.path.join(d, 'save'),
           '--exit-at-frame', str(max(frames) + 1)] + shots_args(os.path.join(d, 'base'), frames)
    p = spawn(cmd, os.path.join(d, 'standalone.log'), dict(os.environ, DISPLAY=disp.name))
    p.wait(600)
    return d


# ===[ scenarios ]===

def test_equivalence(game, label, recording, frames, speed=8):
    """The frames a network client draws must be pixel-identical to the standalone runner's."""
    base = standalone_shots('eq_' + label + '_base', game, recording, frames, speed)
    srv = Server('eq_' + label + '_srv', game, ['--seed', '1', '--speed', str(speed), '--playback-inputs', recording, '--net-max-lag', '0'])
    disp = Display(srv.dir)
    cdir = os.path.join(WORK, 'eq_' + label + '_cli')
    c = client('eq_' + label + '_cli', srv.port, disp,
               shots_args(os.path.join(cdir, 'net'), frames) + ['--exit-at-frame', str(max(frames) + 1)])
    code = wait_exit(c, 900)
    srv.stop()
    bad = []
    for f in frames:
        n = compare(os.path.join(cdir, 'net%d.png' % f), os.path.join(base, 'base%d.png' % f)) if os.path.exists(os.path.join(cdir, 'net%d.png' % f)) else None
        if n != 0:
            bad.append((f, n))
    log = open(c.log, errors='replace').read()
    wire = stat(log, r'([0-9.]+) KB on the wire')
    raw = stat(log, r'raw ([0-9.]+) KB')
    report('equivalence/%s: %d frames pixel-identical to standalone' % (label, len(frames)), code == 0 and not bad,
           'bad frames: %s, exit %s' % (bad, code) if bad or code != 0 else
           'wire %.0f KB vs raw %.0f KB (%.1fx smaller)' % (wire, raw, raw / wire) if wire and raw else '')


def test_multiclient(recording):
    """Several clients get identical frames and every client's keyboard input reaches the game."""
    srv = Server('multi_srv', UNDERTALE, ['--seed', '1', '--speed', '4', '--playback-inputs', recording, '--net-max-lag', '0'])
    n = 4
    disps = [Display(srv.dir) for _ in range(n)]
    cs = []
    for i in range(n):
        cdir = os.path.join(WORK, 'multi_c%d' % i)
        cs.append(client('multi_c%d' % i, srv.port, disps[i], shots_args(os.path.join(cdir, 'f'), [1500]) + ['--exit-at-frame', '1501']))
        time.sleep(0.3)
    codes = [wait_exit(c, 300) for c in cs]
    srv.stop()
    imgs = [os.path.join(WORK, 'multi_c%d' % i, 'f1500.png') for i in range(n)]
    diffs = [compare(imgs[0], p) if os.path.exists(p) else None for p in imgs[1:]]
    report('multiclient: %d clients draw identical frames' % n, all(c == 0 for c in codes) and all(d == 0 for d in diffs), 'exit %s, diffs %s' % (codes, diffs))


def test_input_merge():
    """Client A spams Z to reach the name screen, then B's arrow keys and A's arrow keys must both move the cursor."""
    if shutil.which('xdotool') is None:
        report('input merge', False, 'xdotool missing')
        return
    zspam = os.path.join(WORK, 'unused.json')
    srv = Server('merge_srv', UNDERTALE, ['--seed', '1', '--speed', '4', '--net-max-lag', '0'])
    da, db = Display(srv.dir), Display(srv.dir)
    ca = client('merge_a', srv.port, da, ['--screenshot', os.path.join(WORK, 'merge_a', 's%d.png')])
    cb = client('merge_b', srv.port, db, ['--screenshot', os.path.join(WORK, 'merge_b', 's%d.png')])
    time.sleep(4)

    def yellow_center(path):
        arr = np.asarray(Image.open(path).convert('RGB')).astype(int)
        mask = (arr[:, :, 0] > 200) & (arr[:, :, 1] > 200) & (arr[:, :, 2] < 80)
        ys, xs = np.nonzero(mask)
        return (xs.mean(), ys.mean()) if len(xs) else None

    # Mash Z through the intro until the name screen is up (cursor on "A"); poll with screenshots so slow builds just take longer
    shots = 0
    before = None
    for _ in range(40):
        for _ in range(8):
            da.key('z')
            time.sleep(0.35)
        da.key('F5')
        time.sleep(0.6)
        path = os.path.join(WORK, 'merge_a', 's%d.png' % shots)
        shots += 1
        if os.path.exists(path):
            c = yellow_center(path)
            if c and abs(c[0] - 126) < 8 and abs(c[1] - 167) < 8:
                before = path
                break
    time.sleep(2.0)
    for _ in range(3):
        db.key('Right')
        time.sleep(0.25)
    da.key('Down')
    time.sleep(0.8)
    db.key('F5')
    time.sleep(0.8)
    for c in (ca, cb):
        c.send_signal(signal.SIGINT)
    srv.stop()
    after = os.path.join(WORK, 'merge_b', 's0.png')
    ok = before is not None and os.path.exists(after)
    moved = compare(before, after) if ok else None
    # The highlighted letter is yellow: it has to have moved from "A" (top-left) to "K" (3 right, 1 down) between the two shots
    c0, c1 = (yellow_center(before), yellow_center(after)) if ok else (None, None)
    good = bool(c0 and c1 and c1[0] - c0[0] > 100 and c1[1] - c0[1] > 20)
    report('input merge: keys from two clients both drive the game', ok and good, 'cursor %s -> %s' % (c0, c1))


def test_latency(recording):
    """RTT and input->frame latency follow the simulated network delay; the game stays correct."""
    results = []
    for one_way, jitter in ((0, 0), (25, 0), (75, 10), (150, 30)):
        sim = ['--net-sim-latency', str(one_way), '--net-sim-jitter', str(jitter)]
        srv = Server('lat_%d_srv' % one_way, UNDERTALE, ['--seed', '1', '--playback-inputs', recording] + sim)
        disp = Display(srv.dir)
        c = client('lat_%d_cli' % one_way, srv.port, disp, ['--exit-at-frame', '700'], netsim=sim)
        if shutil.which('xdotool'):
            time.sleep(3)
            for _ in range(8):
                disp.key('z')
                time.sleep(0.4)
        code = wait_exit(c, 120)
        if code is None:
            c.send_signal(signal.SIGINT)
            code = wait_exit(c, 10)
        srv.stop()
        log = open(c.log, errors='replace').read()
        rtt = stat(log, r'round-trip time min [0-9.]+ / avg ([0-9.]+)')
        lat = stat(log, r'input -> next frame latency min [0-9.]+ / avg ([0-9.]+)')
        floor = 2.0 * one_way
        ok = rtt is not None and floor - 1 <= rtt <= floor + 2 * jitter + 40
        results.append((one_way, rtt, lat))
        report('latency: one-way %d ms (+%d jitter) -> RTT %.0f ms, input->frame %s ms' % (one_way, jitter, rtt or -1, '%.0f' % lat if lat else 'n/a'), ok and code in (0, None))


def test_bandwidth(recording):
    """Bandwidth limits: a link that carries the stream keeps latency low; one that cannot gets cut off with a clear reason
    instead of accumulating unbounded lag."""
    # Enough: ~3x the bandwidth Undertale gameplay needs
    srv = Server('bw_ok_srv', UNDERTALE, ['--seed', '1', '--playback-inputs', recording, '--net-sim-bandwidth', '120'])
    disp = Display(srv.dir)
    c = client('bw_ok_cli', srv.port, disp, ['--exit-at-frame', '2500'])
    code = wait_exit(c, 150)
    srv.stop()
    log = open(c.log, errors='replace').read()
    rtt_max = stat(log, r'round-trip time min [0-9.]+ / avg [0-9.]+ / max ([0-9.]+)')
    report('bandwidth: 120 kbit/s link carries Undertale gameplay (max ping %s ms)' % ('%.0f' % rtt_max if rtt_max else 'n/a'),
           code == 0 and rtt_max is not None and rtt_max < 1500)
    # Not enough: lag must stay bounded and the client must be told why
    srv = Server('bw_low_srv', UNDERTALE, ['--seed', '1', '--playback-inputs', recording, '--net-sim-bandwidth', '8'])
    disp = Display(srv.dir)
    c = client('bw_low_cli', srv.port, disp, ['--exit-at-frame', '5000'])
    code = wait_exit(c, 150)
    srv.stop()
    log = open(c.log, errors='replace').read()
    rtt_max = stat(log, r'round-trip time min [0-9.]+ / avg [0-9.]+ / max ([0-9.]+)')
    told = 'too slow' in log
    report('bandwidth: 8 kbit/s link is cut off after <= ~3 s of lag, with a reason (max ping %s ms)' % ('%.0f' % rtt_max if rtt_max else 'n/a'),
           code == 1 and told and (rtt_max is None or rtt_max < 6000))


def test_latejoin(recording):
    """A client joining mid-game ends up drawing exactly what the early client draws."""
    frames = [2400, 2600]
    srv = Server('late_srv', DELTARUNE if os.path.exists(DELTARUNE) else UNDERTALE,
                 ['--seed', '1', '--speed', '4', '--playback-inputs', recording, '--net-max-lag', '0'])
    da, db = Display(srv.dir), Display(srv.dir)
    ca = client('late_a', srv.port, da, shots_args(os.path.join(WORK, 'late_a', 'f'), frames))
    time.sleep(20)
    cb = client('late_b', srv.port, db, shots_args(os.path.join(WORK, 'late_b', 'f'), frames))
    time.sleep(40)
    for c in (ca, cb):
        c.send_signal(signal.SIGINT)
    srv.stop()
    diffs = []
    for f in frames:
        a = os.path.join(WORK, 'late_a', 'f%d.png' % f)
        b = os.path.join(WORK, 'late_b', 'f%d.png' % f)
        if os.path.exists(a) and os.path.exists(b):
            diffs.append(compare(a, b))
    report('late join: second client matches the first', len(diffs) >= 1 and all(d == 0 for d in diffs), 'diffs %s' % diffs)


def test_gamechange(recording):
    """DELTARUNE's chapter launcher calls game_change(): every client must follow to the new data.win."""
    srv = Server('gc_srv', DELTARUNE, ['--seed', '1', '--speed', '4', '--playback-inputs', recording, '--net-max-lag', '0'])
    disp = Display(srv.dir)
    c = client('gc_cli', srv.port, disp, shots_args(os.path.join(WORK, 'gc_cli', 'f'), [2400]) + ['--exit-at-frame', '2401'])
    code = wait_exit(c, 300)
    srv.stop()
    log = open(c.log, errors='replace').read()
    loaded = re.findall(r'Loading (\S+data\.win)', log)
    ok = code == 0 and len(loaded) >= 2 and 'chapter1' in loaded[-1] and os.path.exists(os.path.join(WORK, 'gc_cli', 'f2400.png'))
    report('game_change: client follows the server into chapter 1', ok, 'loaded %s, exit %s' % (loaded, code))


def test_launcher():
    """The launcher starts without arguments, finds the games and can host a game that a client then joins."""
    if shutil.which('xdotool') is None:
        report('launcher', False, 'xdotool missing')
        return
    d = os.path.join(WORK, 'launcher')
    os.makedirs(d, exist_ok=True)
    disp = Display(d, '1280x800x24')
    env = dict(os.environ, DISPLAY=disp.name, XDG_CONFIG_HOME=os.path.join(d, 'cfg'), BUTTERSCOTCH_LAUNCHER_SHOT=os.path.join(d, 'l%d.png'))
    p = subprocess.Popen([BIN], stdout=open(os.path.join(d, 'launcher.log'), 'wb'), stderr=subprocess.STDOUT, env=env, cwd=d)
    procs.append(p)
    time.sleep(5)
    subprocess.run(['xdotool', 'key', 'F5'], env=env)
    time.sleep(1)
    shot = os.path.join(d, 'l0.png')
    shown = os.path.exists(shot)
    # Click "Start hosting" with the first detected game selected; a server child process must appear and the client connect
    wid = subprocess.run(['xdotool', 'search', '--name', 'Butterscotch Slices'], env=env, capture_output=True, text=True).stdout.split()
    started = False
    if wid:
        subprocess.run(['xdotool', 'mousemove', '--window', wid[0], '740', '530', 'click', '1'], env=env)
        time.sleep(10)
        out = subprocess.run(['pgrep', '-P', str(p.pid), '-a'], capture_output=True, text=True).stdout
        started = '--server' in out
        subprocess.run(['xdotool', 'key', 'F5'], env=env)
        time.sleep(1)
    p.terminate()
    time.sleep(2)
    left = subprocess.run(['pgrep', '-P', str(p.pid)], capture_output=True, text=True).stdout.strip()
    report('launcher: shows up, hosts a game and cleans up its server', shown and started and not left, 'screenshot %s, server child %s, leftover %r' % (shown, started, left))


SCENARIOS = ['equivalence', 'multiclient', 'latency', 'bandwidth', 'latejoin', 'gamechange', 'launcher']


def main():
    wanted = [a for a in sys.argv[1:] if a in SCENARIOS] or SCENARIOS
    if not os.path.exists(BIN):
        sys.exit('butterscotch binary not found: %s (set BUTTERSCOTCH)' % BIN)
    for tool in ('Xvfb',):
        if shutil.which(tool) is None:
            sys.exit('%s is required' % tool)
    have_ut = os.path.exists(UNDERTALE)
    have_dr = os.path.exists(DELTARUNE)
    gameplay = os.path.join(WORK, 'gameplay.json')
    zspam = os.path.join(WORK, 'zspam.json')
    make_gameplay(gameplay)
    make_zspam(zspam)
    print('work dir:', WORK, flush=True)
    try:
        if 'equivalence' in wanted:
            if have_ut:
                test_equivalence(UNDERTALE, 'undertale', gameplay, [300, 900, 1500, 2500, 3500, 4500])
            if have_dr:
                test_equivalence(DELTARUNE, 'deltarune', zspam, [600, 1500, 2200, 2600], speed=8)
        if 'multiclient' in wanted and have_ut:
            test_multiclient(gameplay)
            test_input_merge()
        if 'latency' in wanted and have_ut:
            test_latency(gameplay)
        if 'bandwidth' in wanted and have_ut:
            test_bandwidth(gameplay)
        if 'latejoin' in wanted and (have_ut or have_dr):
            test_latejoin(zspam if have_dr else gameplay)
        if 'gamechange' in wanted and have_dr:
            test_gamechange(zspam)
        if 'launcher' in wanted:
            test_launcher()
    finally:
        cleanup()
    print('%d failure(s)' % failures)
    shutil.rmtree(WORK, ignore_errors=True) if failures == 0 else print('kept', WORK)
    sys.exit(failures)


if __name__ == '__main__':
    main()
