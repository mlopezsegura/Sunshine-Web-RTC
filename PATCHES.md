# Patches on top of upstream Sunshine

This repository is [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) with the
[Moonlight WebRTC](https://github.com/tsoas/moonlight-webrtc-tizen) Gateway built in, so Samsung Tizen
TVs stream from Sunshine directly. This file lists every change made to upstream, why it was made and
how to carry it to a newer Sunshine. The user-facing guide is
[docs/moonlight_webrtc_tizen.md](docs/moonlight_webrtc_tizen.md).

| | |
|---|---|
| Upstream base | `0594f62d` — *chore: update global workflows (#5868)*, tag `v2026.1006.152353` |
| Fork branch | `master` (development on `webrtc-tizen`) |
| WebRTC library | libdatachannel v0.24.6, built from source when no system package exists |
| Footprint in upstream files | 17 files, +441 / −14 lines; the rest is new files |

## Design in one paragraph

Sunshine's encoders publish every encoded frame to one process-wide queue that the GameStream
broadcast threads drain. The fork lets a session register its own video and audio queues in its
mailbox; the WebRTC stream does, so its frames go from the encoder straight to libdatachannel's RTP
packetizer and never reach the GameStream sender. Everything else, the TV protocol, pairing, sessions,
input and the WebSocket server, lives in the new `src/webrtc/` directory and only calls Sunshine's
existing APIs (`video::capture`, `audio::capture`, `input::alloc/passthrough`, `proc::proc`,
`display_device`). Moonlight clients behave exactly as on upstream. One option, `stream_protocol`,
chooses which of the two Sunshine serves (`moonlight`, `webrtc` or `both`); the servers of the other
one are not started, so their ports stay closed.

## The patch set

`patches/` holds the whole fork as seven topic patches. Applied in order to the upstream base they
reproduce this repository exactly (apart from this file, `patches/` and `scripts/webrtc/`):

```sh
git apply patches/*.patch
```

`scripts/webrtc/export-patches.sh` regenerates them and verifies that property on a clean upstream
checkout. It reads committed history, so commit a change first, then run the script and commit
`patches/`.

| Patch | Files | Kind |
|---|---|---|
| `0001-core-per-session-packet-queues` | `src/thread_safe.h`, `src/globals.h`, `src/video.cpp`, `src/audio.cpp` | Upstream change |
| `0002-core-webrtc-server-integration` | `src/config.h`, `src/config.cpp`, `src/main.cpp`, `src/nvhttp.cpp`, `src/confighttp.cpp` | Upstream change |
| `0003-build-libdatachannel` | `cmake/dependencies/common.cmake`, `cmake/compile_definitions/common.cmake`, new `cmake/dependencies/libdatachannel.cmake` | Upstream change + new file |
| `0004-webrtc-module` | new `src/webrtc/*` | New files |
| `0005-web-ui-tv-pairing-and-options` | `Pin.vue`, `configs/config_tabs.json`, `configs/tabs/Network.vue`, `locale/en.json` | Upstream change |
| `0006-tests` | new `tests/unit/webrtc/*`, new `tests/webrtc_e2e/*` | New files |
| `0007-docs` | `README.md`, `docs/configuration.md`, new `docs/moonlight_webrtc_tizen.md` | Upstream change + new file |

### 0001 — Per-session packet queues (core)

The only change to Sunshine's streaming pipeline.

| File | Change | Why |
|---|---|---|
| `src/thread_safe.h` | New `safe::mail_raw_t::has(id)`: whether a live event or queue is registered under an ID. | Lets the encoders ask whether a session brought its own queue without creating one as a side effect, which `queue()` would do. |
| `src/globals.h` | New `mail::packet_queue<T>(session_mail, id)`: the session's queue when it registered one, otherwise the process-wide `mail::man` queue. | Single routing rule for video and audio. |
| `src/video.cpp` | `encode_run()` (parallel encoders) and `capture()` (the `sync_session_ctx_t` of synchronous encoders) take their packet queue from `mail::packet_queue()` instead of `mail::man`. | Both encoder paths must route, or AMF/QSV and NVENC would behave differently. `validate_config()` still uses the global queue: probing has no session. |
| `src/audio.cpp` | `encodeThread()` receives its packet queue as a parameter; `capture()` passes `mail::packet_queue()`. | `encodeThread()` had no access to the session mailbox. |

Moonlight sessions never register their own queues, so for them `packet_queue()` returns the same
global queue as before: behaviour is unchanged.

### 0002 — Starting the server, options and coordination (core)

| File | Change | Why |
|---|---|---|
| `src/config.h`, `src/config.cpp` | New `config::stream_protocol_e { moonlight, webrtc, both }` and `config::webrtc_t { protocol, port, media_port_min, media_port_max }`, defaults `both` / `8000` / `0` / `0`, parsed from `stream_protocol`, `webrtc_port` (1024–65535) and `webrtc_media_port_min/max` (0–65535); `config::moonlight_enabled()` and `config::webrtc_enabled()`. A legacy `webrtc_enabled = disabled` without `stream_protocol` still means `moonlight`. | 8000 is the TV app's default port. A media range lets a firewall that opens ports, rather than allowing the executable, admit WebRTC; several instances on one PC (OpenStreamMS) each get their own range. |
| `src/main.cpp` | Starts `webrtc_stream::start` on its own `std::jthread` next to the nvhttp, confighttp and RTSP threads. With `stream_protocol = webrtc` it starts neither the RTSP thread nor mDNS publishing nor UPnP. | The server returns when `mail::shutdown` is raised, so shutdown joins it like the others. A TV-only host leaves every GameStream port closed and is not advertised to Moonlight. |
| `src/nvhttp.cpp` | `launch` and `resume` treat a running TV stream like a running Moonlight session (`webrtc_stream::session_count()`). With `stream_protocol = webrtc`, `start()` loads the paired clients and certificates, then waits for shutdown without listening. | Otherwise a Moonlight launch while a TV streams would reconfigure the display and re-probe encoders under the TV. The Web UI still lists, and `unpair-all` still saves, the Moonlight clients when Moonlight is off, instead of overwriting them with an empty list. |
| `src/confighttp.cpp` | No new endpoints: `GET`/`POST`/`DELETE /api/pin` also list, approve and decline TVs waiting to pair, and `/api/clients/list`, `update`, `unpair` and `unpair-all` also list, enable or disable, and unpair paired TVs. | TV pairing used to live in the Gateway's tray; TVs are now paired and managed exactly like Moonlight clients, in the same Web UI. |

The reverse coordination is in the new module: a TV stream configures the display and probes encoders
only when no Moonlight session and no other TV stream is running, and calls
`platf::streaming_will_start/stop()` and the tray updates like `stream::session` does.

### 0003 — Building libdatachannel

| File | Change |
|---|---|
| `cmake/dependencies/libdatachannel.cmake` (new) | Uses a system `LibDataChannel` ≥ 0.24 when available, otherwise `FetchContent` of v0.24.6 with its `plog`, `usrsctp`, `libjuice` and `libsrtp` submodules, built static, with OpenSSL (Sunshine's), media and WebSocket support, and no examples or tests. libSRTP's `ENABLE_WARNINGS_AS_ERRORS` is turned off because MinGW printf-format warnings break it, and Sunshine's own nlohmann_json is reused. |
| `cmake/dependencies/common.cmake` | Includes it after nlohmann_json. |
| `cmake/compile_definitions/common.cmake` | Adds `src/webrtc/*` to `SUNSHINE_TARGET_FILES` and `${SUNSHINE_LIBDATACHANNEL_TARGET}` to `SUNSHINE_EXTERNAL_LIBRARIES`. |

MSYS2 does not package libdatachannel, so Windows builds always take the FetchContent path.

### 0004 — The WebRTC module (new files)

| File | Contents |
|---|---|
| `src/webrtc/webrtc_stream.{h,cpp}` | The server: libdatachannel `WebSocketServer`, TV connections (up to 8 unauthenticated, one active), sessions, PeerConnection with send-only H.264/H.265/AV1 + Opus tracks and the `control` (reliable) and `gamepad` (unordered, no retransmits) DataChannels, application launch/resume/stop/switch through `proc::proc`, display configuration, the capture pipeline and the Web UI hooks. `media_port_range()` turns `webrtc_media_port_min/max` into the PeerConnection's UDP port range; the discovery socket is opened with `SO_REUSEADDR`, so every Sunshine instance on a PC answers a TV's broadcast with its own name and port. |
| `src/webrtc/protocol.{h,cpp}` | Gateway protocol version 2, ported and extended: stream settings and supported modes (now with 30/60/90/120 fps per mode), message parsing and construction, TV-shown PIN pairing and LAN discovery messages. |
| `src/webrtc/tv_auth.{h,cpp}` | Pairing requests (the TV shows a 4-digit PIN that the Web UI approves; one attempt, 5 minutes), paired-TV store (`webrtc_tv_clients.json` in Sunshine's config directory) and HMAC-SHA256 nonce authentication. |
| `src/webrtc/sdp.{h,cpp}` | Samsung Game Mode `imageattr`, single-codec offers, HEVC Main10 `fmtp` and level checks. |
| `src/webrtc/input_bridge.{h,cpp}` | Gamepad snapshots → Moonlight controller packets fed to `input::passthrough()`, so Sunshine's own controller emulation handles them; long-press Start mouse mode; rumble relayed to the TV. |

Stream parameters, chosen to match what the Gateway negotiated through moonlight-common-c:

| Parameter | Value | Note |
|---|---|---|
| Frame rate | 60 (`framerateX100` 6000) | |
| Slices / reference frames | 1 / 1 | No reference-frame invalidation; losses recover through NACK and PLI→IDR. |
| Colour | Rec.709 limited (`encoderCscMode` 2); HDR: Rec.2020 PQ, 10-bit (`4`, `dynamicRange` 1) | HDR fails with an error when the captured display is not HDR, never silently SDR. |
| Video bitrate | TV bitrate − min(512, ⅕) − min(500, ⅒) kbps | Same audio and overhead deductions as Sunshine's RTSP path, without the 20 % FEC share. |
| Audio | Opus stereo, 48 kHz, 5 ms packets, high quality (512 kbps) | As Moonlight requests on a LAN. |
| RTP timestamps | From each frame's capture time | Monotonic; falls back to 1/60 s steps. |
| IDR byte replacements | Applied before packetizing | As `stream.cpp` does for encoders without VUI parameters. |
| RTCP color-space extension | Offered for HDR, never sent | As the Gateway did: Tizen decoded no frames with it present. |

### 0005 — Web UI

| File | Change |
|---|---|
| `src_assets/common/assets/web/Pin.vue` | TVs waiting to pair join the existing PIN form, which now suggests the name the selected device reported. Paired TVs need no UI change: they appear in the existing client list under Troubleshooting. |
| `configs/config_tabs.json`, `configs/tabs/Network.vue` | A Moonlight / WebRTC / Both switch (`stream_protocol`) at the top of the Network tab; the port table lists the ports each enabled protocol needs (the Web UI alone when Moonlight is off, plus WebRTC signaling, discovery and media); `webrtc_port`, `webrtc_media_port_min` and `webrtc_media_port_max` fields, disabled when WebRTC is off. |
| `public/assets/locale/en.json` | Strings for all of them. Other languages fall back to English, as upstream requires. |

### 0006 — Tests

- `tests/unit/webrtc/` — 46 GoogleTest cases: protocol parsing and capabilities, SDP checks, TV auth
  (including the HMAC vector shared with the TV's JavaScript), and the input bridge, whose packets
  are validated by Sunshine's own `input::testing::is_valid_input_packet()`, plus the packet routing of
  patch 0001, `stream_protocol` parsing (including the legacy `webrtc_enabled`) and the media port range.
- `tests/webrtc_e2e/` — a headless-Edge stand-in for the TV that shows a PIN, gets paired through
  `/api/pin` like a user would, authenticates, streams and measures `getStats()` against a running Sunshine.

The upstream config-consistency test requires every new option in `config_tabs.json`, `en.json` and
`docs/configuration.md`; patches 0005 and 0007 satisfy it.

### 0007 — Documentation

`README.md` gets a note pointing at the guide; `docs/configuration.md` documents the four options;
`docs/api.md` is untouched, since TVs use the existing endpoints; `docs/moonlight_webrtc_tizen.md` is the guide.

## Verification of this revision

- Builds with MSYS2 UCRT64 (GCC) without warnings.
- `test_sunshine`: 698 passed, 5 skipped for the environment (no NVIDIA or Intel GPU, no tray build,
  no virtual HID licence, no external shell command), 0 failed.
- Protocols: Sunshine started with each `stream_protocol` listens only on that protocol's ports. With
  `webrtc`: the Web UI (TCP port+1), the TV WebSocket and UDP 8000, and no GameStream HTTP/HTTPS or RTSP
  port; with `moonlight`: the GameStream ports and the Web UI, and neither the TV WebSocket nor UDP 8000;
  with `both`: all of them.
- End to end on an AMD RX 9060 XT (AMF) with headless Edge: pairing through the Web UI PIN form (the TV listed in
  `/api/pin`, a wrong PIN, a cancellation and a stale request ID refused, the right PIN pairing it under
  the name typed in the Web UI; unauthenticated requests refused), the real Tizen app showing its PIN
  and pairing in headless Edge, TVs listed, disabled (refused without losing their credentials), re-enabled
  and unpaired through the client list, a TV reconnecting mid-stream replacing its old connection
  (`--takeover=1`), re-authentication, application list and artwork, H.264 720p/1080p and AV1 1080p
  decoded with every frame and 0 packets lost, Opus audio with 0 lost, controller announced,
  stop-session and stop-host-session, PLI answered with IDR, paired TVs persisted across a restart,
  and a clean exit (code 0) when Sunshine shuts down mid-stream.
- Frame rates: H.264 1080p at 120 fps, H.264 720p at 30 fps and AV1 1080p at 90 fps accepted and streamed
  with 0 packets lost; the measured rate follows the 60 Hz test display, so above 60 fps needs a 120 Hz
  source and TV to judge.
- Discovery: a broadcast `{"version":2,"type":"discover"}` to UDP 8000 answered with the name, the
  configured `webrtc_port` and the Wake-on-LAN address of the interface facing the sender; malformed and
  version 1 requests ignored.
- Not verifiable with headless Edge, which has no HEVC or AV1 10-bit WebRTC decoder: decoding of HEVC
  and of 10-bit HDR streams. Sunshine established those sessions and encoded AV1 Main10 PQ; a Samsung
  TV is the reference.

## Updating to a newer Sunshine

```sh
git fetch upstream
git checkout -b update-upstream
git rebase upstream/master            # or: git merge upstream/master
scripts/webrtc/export-patches.sh      # regenerate patches/ and check them
```

Where conflicts are likely:

- **`src/video.cpp` / `src/audio.cpp`** — any refactor of `encode_run()`, the synchronous capture
  context or `encodeThread()`. Keep every place that obtains the encoded-packet queue for a session
  going through `mail::packet_queue()`; search for `mail::man->queue<packet_t>`.
- **`src/stream.cpp` session lifecycle** — `webrtc_stream.cpp` (`prepare_host`, `release_host`,
  `capture`) mirrors `stream::session::start/join` and `nvhttp::launch/resume`. If upstream changes
  what those do when the first session starts or the last one ends, mirror it.
- **`src/input.cpp` packet formats** — the input bridge builds `NV_MULTI_CONTROLLER_PACKET`,
  `SS_CONTROLLER_ARRIVAL_PACKET` and mouse packets; the unit tests catch format changes.
- **`video::active_hevc_mode` / `active_av1_mode`** — read to decide which codecs to offer.
- **`src/confighttp.cpp`** route table and **`config_tabs.json`** — usually trivial conflicts.

After updating, run the unit tests and the end-to-end harness, and test HEVC and HDR on a TV.
