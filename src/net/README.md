# Butterscotch Slices: client / server mode

> **Butterscotch Slices** is a fork of [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch). Its purpose is
> to let several people play one GameMaker game session over the network; this document describes that feature.
>
> **AI-generated:** this feature was written by an AI (Claude); new commits may not have been verified by a human.

Butterscotch can run a game as a **server** that executes all game logic and any number of **clients** that only render,
play sound and send input. Several people can play the same game session: every client's keyboard / mouse / gamepad is
merged and drives the one game running on the server.

```
 ┌──────────── server (headless) ────────────┐                    ┌────── client (window) ──────┐
 │ VM + Runner: game logic, saves, RNG       │   FRAME (deflate)  │ NetRenderDecoder → OpenGL   │
 │   draw calls ──► RemoteRenderer ──────────┼───────────────────►│ NetAudioDecoder  → speakers │
 │   audio calls ─► NetAudioTee    ──────────┼───────────────────►│ local copy of data.win,     │
 │ (silent audio engine, answers all queries)│                    │ textures, fonts, sounds     │
 │ input merge ◄─────────────────────────────┼◄───────────────────┤ keyboard / mouse / gamepad  │
 └───────────────────────────────────────────┘      INPUT          └─────────────────────────────┘
                 ▲                                                   (× N clients, joinable any time)
```

No audio or video is ever streamed. Both sides have the game files; the server only sends *what to draw and play*
("sprite 1234 at (80, 100) scaled 2×", "play sound 17 looping") and the clients render that with their own renderer
and audio engine.

## Using it

**Launcher** – start `butterscotch` with no arguments. It scans for games (Steam / GOG libraries, the current and home
directories, ...) and lists them; you can also type the path of a `data.win` / `game.unx` / `game.ios` (or the game
folder) and press *Add*. Then pick *Host*, *Join* or *Offline*:

* *Host* starts a server for the selected game (as a child process) and joins it, so you play like everybody else.
* *Join* takes `host[:port]`. You need your own copy of the **same** game files; if you don't select one, the client
  finds the copy that matches the server's game by itself (size + content fingerprint).
* *Offline* is the normal single-player runner.

**Command line**

```
butterscotch --server  /path/to/data.win [--port 7777] [--bind 0.0.0.0] [--max-clients 16]
butterscotch --connect host[:port] [/path/to/data.win] [--name alice]
```

All the usual options still work (`--seed`, `--speed`, `--playback-inputs` on the server, `--screenshot-at-frame` on a
client, ...). The server freezes the game while nobody is connected (`--run-without-clients` changes that). A client can
press `F5` to save a screenshot (`screenshot-N.png`, or the `--screenshot` pattern).

Testing helpers (they apply to everything the endpoint *sends*, so pass the same values to both sides for RTT = 2×):
`--net-sim-latency <ms>`, `--net-sim-jitter <ms>`, `--net-sim-bandwidth <kbps>`.

Build option: `-DENABLE_NETPLAY=OFF` (CMake) / `DISABLE_NETPLAY=1` (Makefile) leaves it all out. It is only built for the
desktop CLI platform with a window backend (not for `BACKEND=noop`, consoles or the web build).

## How it works

### Server: record, don't render
* `RemoteRenderer` implements the `Renderer` vtable by serializing every call into a compact command stream.
  `tools/gen_net_render.py` generates the encoder/decoder pair for every plain call from `src/renderer.h`; calls that return
  values the game needs (surface sizes, texture handles, blend state, shader uniforms, ...) are answered from **shadow
  state** on the server, so the game never waits for a client. If `renderer.h` changes, re-run the script.
* State that the renderers read outside the vtable (`draw_set_colour/font/halign`, views and cameras) is sent as deltas.
* `NetAudioTee` wraps the real audio engine running on miniaudio's *null device*: mixing, timing and
  `audio_is_playing()` etc. behave exactly as with a sound card, nothing is audible, and every mutating call is mirrored
  to the clients.
* Surface IDs, sprite slots (`sprite_create_from_surface`), shader uniform handles and sound instance IDs are translated
  by each client, so a client doesn't need identical allocation history (late join).

### Client: replay
`NetRenderDecoder` replays the commands on the normal GL renderer (modern or legacy), the window's letterboxing is done
by the client for its *own* window size, and input (key / mouse / gamepad events) is sent immediately.

### Input merge (several players, one game)
A key is down while **any** client holds it; a press is reported once, even if two clients press together, and a tap that
starts and ends within one game step is not lost. Characters and mouse buttons are applied in arrival order, the mouse
cursor follows the most recently active client, gamepad 0 of every client is OR-merged into gamepad slot 0.

### Protocol (see `net_proto.h`)
TCP with `TCP_NODELAY`; framed messages; `HELLO → GAME_INFO → (client loads its local copy) → READY → SYNC → FRAME…`.
`GAME_INFO` carries a fingerprint (file size + hash of the head / tail / samples) so a client with different files is
refused with a clear message. `game_change()` (DELTARUNE's chapter launcher) makes the server tell its clients to switch;
each client resolves the new `data.win` relative to its own copy.

### Bandwidth
* Everything is lossless and encoded as varints; floats that are integral or multiples of 1/16 take 1–3 bytes.
* Cameras are sent as parameters, their matrices are recomputed by the client (`camera_set_view_mat` still works: those
  are sent in full); base state (colour/alpha/font/...) and view/camera changes are delta coded.
* All `FRAME`/`SYNC` messages of a client go through one persistent **deflate** stream flushed per frame, so consecutive
  nearly identical frames compress to almost nothing.
* Measured (30 fps, per client, TCP payload without TCP/IP headers): Undertale intro/menus ≈ 1 KB/s, Undertale
  overworld walking ≈ 2 KB/s, DELTARUNE chapter 1 intro ≈ 8 KB/s (compression ratio 4–11×).

### Latency
* The server sends each frame the moment it is produced, before sleeping to pace the next step; clients present a frame as
  soon as it arrives (no jitter buffer) and, if several are queued, replay all but only present the last.
* Input is sent on the next client loop iteration (~1–2 ms), not on a timer, and is applied at the start of the next step.
* Measured with the built-in simulator (one-way delay on both endpoints), real X11 key presses, Undertale:

| one-way delay | jitter | ping (RTT) | key press → frame that shows it |
|---|---|---|---|
| 0 ms | 0 | 3 ms | 15 ms (half a 30 fps step on average) |
| 25 ms | 0 | 52 ms | 72 ms |
| 75 ms | ±10 | 163 ms | 172 ms |
| 150 ms | ±30 | 332 ms | 347 ms |

  i.e. the game adds at most one logic step (33 ms at 30 fps) on top of the network round trip.

### Slow links and slow machines
Frames cannot be skipped (they build on each other: surfaces, state), so a connection that is slower than the stream would
accumulate lag without bound. Clients therefore report progress (`ACK`, ~15 per second) and the server watches how old the
oldest unacknowledged frame is: a client that is still responsive but more than **3 seconds** behind (or that has stopped
responding for 15 s) is cut off with a clear reason (`--net-max-lag <ms>` changes the limit, 0 disables it; use 0 when running
the server faster than real time with `--speed`) ("your connection (or machine) is too slow ..."; the launcher shows it).
Short stalls are fine: the client replays everything that queued up and only presents the newest frame. Measured with
`--net-sim-bandwidth`: a 120 kbit/s link carries Undertale gameplay with pings < 40 ms; an 8 kbit/s link is cut off after
about 3 s instead of growing to minutes of lag.

### Late join
A client may join at any time. It gets the persistent renderer state from the server (`SYNC`: surfaces, shader handles,
blend/alpha/shader state, the currently playing sounds with their positions) and the **pixels of all live surfaces from an already
connected client** (the server asks it in-band, so the snapshot matches the stream position exactly while the joiner's
frames are buffered). Not restored: pixels of sprites created with `sprite_create_from_surface()` before the join.

A hardened Docker setup for a dedicated server is described in the [main README](../../README.md#running-a-hardened-dedicated-server-docker).

## Limitations
* `surface_getpixel`, `buffer_get_surface` and other GPU read-backs return failure on the server (it has no GPU).
* With `application_surface_enable(false)` the game draws straight into the window, which only matches the first client's
  window size; other clients get a scaled view.
* Window-management calls from GML (`window_set_size`, fullscreen, ...) change the server's idea of the window but don't
  resize clients. `window_set_caption` is forwarded.
* The wire format is not versioned across builds (`NET_PROTOCOL_VERSION` must match).
* The host plays through a normal client, so closing the host's window ends the session for everybody (use
  `--server` for a dedicated server that outlives its players).
* TCP means one lost packet delays everything behind it (head-of-line blocking); there is no UDP mode.
* Verified on Linux (GCC, SDL2) and on Windows (mingw-w64 cross build run under Wine, both directions, pixel-identical
  to the Linux reference). The macOS (SDL1 / AppKit) and clang/tcc builds are written against the same portable API
  (Winsock / BSD sockets, the `platform*` backend functions) but were not compiled here.

## Security
Threat model: a server is trusted to run the game, but neither side trusts the bytes the other sends.
* Every length, count, handle and index read from the wire is bounds checked (frame size, decompressed size, strings,
  surface/vertex limits, shader and matrix indices); the client confines files it writes to its own directories and
  restricts audio stream names; the server validates client input (key codes, counts) and kicks clients that fall behind
  (`--net-max-lag`).
* Hardening is exercised by fuzzers: `tests/net/net_fuzz_server.py` (hostile clients against an ASAN/UBSAN server) and an
  in-process decoder fuzzer built with `-DENABLE_NET_FUZZ=ON` (`--net-fuzz-decoder`, `--net-fuzz-iterations`, `--net-fuzz-seed`), which mutates recorded server
  streams and replays hostile ones. Both ran clean (no sanitizer reports; server memory bounded) on the final code.
* Not protected: there is no authentication or encryption (use a VPN/SSH tunnel on untrusted networks), and a malicious
  server can still waste a client's CPU/GPU/memory, though not corrupt it. Game autodetect parses `data.win` files found on
  disk with the normal data-file parser.

## Tests
`tests/net/net_tests.py` starts real servers and clients (clients on private Xvfb displays) and checks:
pixel-identical output compared with the standalone runner (Undertale gameplay incl. walking around; DELTARUNE incl.
`game_change`), identical frames on four clients, key presses from two clients both reaching the game (xdotool),
the latency table above, bandwidth-limited links, late join, and the launcher hosting a game. It needs your own copies of the games
(`UNDERTALE=…/data.win DELTARUNE=…/data.win BUTTERSCOTCH=build/butterscotch python3 tests/net/net_tests.py`).

## Code map
| file | |
|---|---|
| `net_buf.h` | varint / lossless-float serialization |
| `net_link.[ch]` | sockets, framing, streaming deflate, latency/jitter/bandwidth simulation |
| `net_proto.h` | message types |
| `net_render.h`, `net_render_server.c`, `net_render_client.c`, `net_render_common.h` | recording renderer and replay decoder (`net_render_gen.inc`, `net_render_ops.h` are generated) |
| `net_audio.[ch]` | audio tee and decoder |
| `net_server.[ch]`, `net_client.[ch]` | the two main loops |
| `game_scan.[ch]` | game auto-detection |
| `launcher.[ch]` | the launcher |
