#!/usr/bin/env python3
"""Hostile-client fuzzer for the Butterscotch server.

Starts a server (ideally an ASAN/UBSan build) and throws garbage and mutated protocol messages at it: random bytes, valid
HELLOs followed by malformed INPUT / READY / ACK / PING / SNAPSHOT_DATA messages, absurd message lengths, compression bombs
and slow-loris style connections. Afterwards the server must still be alive and serve a normal client, must not have grown
excessively, and its log must contain no sanitizer reports.

    BUTTERSCOTCH=build-asan/butterscotch UNDERTALE=/path/to/data.win python3 tests/net/net_fuzz_server.py [iterations] [seed]
"""
import os, random, socket, struct, subprocess, sys, tempfile, time, zlib

BIN = os.path.abspath(os.environ.get('BUTTERSCOTCH', 'build/butterscotch'))
GAME = os.environ.get('UNDERTALE', os.path.expanduser('~/Undertale/data.win'))
ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 400
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
rnd = random.Random(SEED)

NM_HELLO, NM_READY, NM_INPUT, NM_PING, NM_BYE, NM_ERROR, NM_SNAPSHOT_DATA, NM_ACK = 1, 2, 3, 4, 5, 6, 7, 8


def varint(n):
    out = bytearray()
    while n >= 0x80:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n)
    return bytes(out)


def zigzag(n):
    return varint(((n << 1) ^ (n >> 31)) & 0xFFFFFFFF)


def msg(mtype, payload=b''):
    body = bytes([mtype]) + payload
    return varint(len(body)) + body


def hello(name=b'fuzz', w=640, h=480, version=1):
    return msg(NM_HELLO, varint(version) + varint(len(name) + 1) + name + varint(w) + varint(h))


class Deflater:
    def __init__(self):
        self.c = zlib.compressobj(6)

    def message(self, mtype, payload):
        body = self.c.compress(payload) + self.c.flush(zlib.Z_SYNC_FLUSH)
        full = bytes([mtype | 0x80]) + body
        return varint(len(full)) + full


def input_msg(rnd):
    flags = rnd.choice([0, 1, 2, 4, 8, 15, 255, rnd.randrange(256)])
    p = bytes([flags]) + varint(rnd.randrange(1 << 32))
    n = rnd.randrange(0, 40)
    for _ in range(n):
        p += bytes([rnd.randrange(256)]) if rnd.random() < 0.5 else varint(rnd.randrange(1 << 32))
    return msg(NM_INPUT, p)


def mutate(b, rnd):
    b = bytearray(b)
    for _ in range(rnd.randint(1, 4)):
        if not b:
            break
        i = rnd.randrange(len(b))
        k = rnd.randrange(5)
        if k == 0:
            b[i] ^= 1 << rnd.randrange(8)
        elif k == 1:
            b[i] = rnd.randrange(256)
        elif k == 2:
            del b[i:i + rnd.randint(1, 6)]
        elif k == 3:
            b[i:i] = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, 6)))
        else:
            b[i:i + 5] = b'\xff\xff\xff\xff\x0f'
    return bytes(b)


def connect(port):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def drain(s, t=0.05):
    s.settimeout(t)
    data = b''
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            data += d
            if len(data) > 1 << 22:
                break
    except (socket.timeout, OSError):
        pass
    return data


def rss_kb(pid):
    try:
        for line in open('/proc/%d/status' % pid):
            if line.startswith('VmRSS'):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


def healthy(port):
    """A normal client can still complete the handshake."""
    try:
        s = connect(port)
        s.sendall(hello(b'health'))
        data = b''
        for _ in range(40):
            data += drain(s, 0.25)
            if data and data[1] & 0x7F == 16:  # GAME_INFO
                break
        s.close()
        return bool(data) and (data[1] & 0x7F) == 16
    except OSError:
        return False


def main():
    work = tempfile.mkdtemp(prefix='bs-fuzz-')
    portfile = os.path.join(work, 'port')
    log = open(os.path.join(work, 'server.log'), 'wb')
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:log_path=%s/asan' % work, UBSAN_OPTIONS='print_stacktrace=1:log_path=%s/ubsan' % work)
    srv = subprocess.Popen([BIN, '--server', GAME, '--port', '0', '--port-file', portfile, '--save-folder', work + '/save', '--run-without-clients'],
                           stdout=log, stderr=subprocess.STDOUT, env=env)
    for _ in range(600):
        if os.path.exists(portfile) and open(portfile).read().strip():
            break
        time.sleep(0.1)
    port = int(open(portfile).read())
    time.sleep(2)
    rss0 = rss_kb(srv.pid)
    print('server pid %d port %d (work %s), start RSS %d MB' % (srv.pid, port, work, rss0 // 1024), flush=True)
    ok = True
    try:
        assert healthy(port), 'server not healthy at start'
        for i in range(ITER):
            kind = rnd.randrange(9)
            try:
                s = connect(port)
                if kind == 0:  # pure garbage
                    s.sendall(bytes(rnd.randrange(256) for _ in range(rnd.randint(1, 600))))
                elif kind == 1:  # absurd length prefix
                    s.sendall(varint(rnd.choice([0, 1, 4097, 1 << 20, 1 << 26, (1 << 32) - 1])) + bytes(rnd.randrange(256) for _ in range(rnd.randint(0, 50))))
                elif kind == 2:  # mutated hello
                    s.sendall(mutate(hello(bytes(rnd.randrange(256) for _ in range(rnd.randint(0, 40))), rnd.randrange(1 << 32), rnd.randrange(1 << 32)), rnd))
                elif kind == 3:  # hello then junk inputs while still loading
                    s.sendall(hello())
                    for _ in range(rnd.randint(1, 20)):
                        s.sendall(input_msg(rnd))
                elif kind == 4:  # complete handshake, become active, then fuzz as an active client
                    s.sendall(hello(b'a\x1b[31mred\nline'))
                    drain(s, 0.5)
                    s.sendall(msg(NM_READY))
                    drain(s, 0.5)
                    for _ in range(rnd.randint(1, 30)):
                        r = rnd.randrange(6)
                        if r == 0:
                            s.sendall(input_msg(rnd))
                        elif r == 1:
                            s.sendall(msg(NM_ACK, varint(rnd.randrange(1 << 32))))
                        elif r == 2:
                            s.sendall(msg(NM_PING, bytes(rnd.randrange(256) for _ in range(rnd.randint(0, 64)))))
                        elif r == 3:
                            s.sendall(msg(NM_SNAPSHOT_DATA, bytes(rnd.randrange(256) for _ in range(rnd.randint(0, 200)))))
                        elif r == 4:
                            s.sendall(msg(NM_READY))
                        else:
                            s.sendall(mutate(input_msg(rnd), rnd))
                        if rnd.random() < 0.1:
                            drain(s, 0.02)
                elif kind == 5:  # compression bomb from an active client (must be cut off, not buffered)
                    s.sendall(hello())
                    drain(s, 0.5)
                    s.sendall(msg(NM_READY))
                    drain(s, 0.5)
                    d = Deflater()
                    chunk = b'\x00' * (1 << 20)
                    try:
                        for _ in range(300):  # would inflate to 300 MB
                            s.sendall(d.message(NM_SNAPSHOT_DATA, chunk))
                    except OSError:
                        pass
                elif kind == 6:  # compressed garbage as a stranger (compressed stream before handshake)
                    s.sendall(Deflater().message(rnd.randrange(1, 30), bytes(rnd.randrange(256) for _ in range(rnd.randint(0, 300)))))
                elif kind == 7:  # ping flood without reading replies
                    s.sendall(hello())
                    for _ in range(2000):
                        s.sendall(msg(NM_PING, b'\x00' * 1000))
                else:  # truncated message, then hang up
                    m = hello()
                    s.sendall(m[:rnd.randrange(1, len(m))])
                time.sleep(0.01)
                s.close()
            except OSError:
                pass
            if srv.poll() is not None:
                print('SERVER DIED at iteration %d (kind %d), exit %s' % (i, kind, srv.returncode))
                ok = False
                break
            if i % 50 == 49:
                print('  %d iterations, RSS %d MB' % (i + 1, rss_kb(srv.pid) // 1024), flush=True)
        time.sleep(1)
        if ok:
            alive = srv.poll() is None and healthy(port)
            rss1 = rss_kb(srv.pid)
            print('server alive and serving: %s; RSS %d -> %d MB' % (alive, rss0 // 1024, rss1 // 1024))
            ok = alive and rss1 < rss0 + 1500 * 1024
    finally:
        srv.terminate()
        try:
            srv.wait(10)
        except subprocess.TimeoutExpired:
            srv.kill()
    reports = [f for f in os.listdir(work) if f.startswith(('asan', 'ubsan'))]
    text = open(os.path.join(work, 'server.log'), errors='replace').read()
    if reports or 'runtime error' in text or 'AddressSanitizer' in text:
        ok = False
        print('SANITIZER REPORTS:', reports)
        for r in reports[:3]:
            print(open(os.path.join(work, r), errors='replace').read()[:3000])
    print('PASS' if ok else 'FAIL', '(work dir %s)' % work)
    sys.exit(0 if ok else 1)


main()
