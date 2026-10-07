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
| Footprint in upstream files | 18 files, +360 / −9 lines; the rest is new files |

## Design in one paragraph

Sunshine's encoders publish every encoded frame to one process-wide queue that the GameStream
broadcast threads drain. The fork lets a session register its own video and audio queues in its
mailbox; the WebRTC stream does, so its frames go from the encoder straight to libdatachannel's RTP
packetizer and never reach the GameStream sender. Everything else, the TV protocol, pairing, sessions,
input and the WebSocket server, lives in the new `src/webrtc/` directory and only calls Sunshine's
existing APIs (`video::capture`, `audio::capture`, `input::alloc/passthrough`, `proc::proc`,
`display_device`). Moonlight clients behave exactly as on upstream.

## The patch set

`patches/` holds the whole fork as seven topic patches. Applied in order to the upstream base they
reproduce this repository exactly (apart from this file, `patches/` and `scripts/webrtc/`):

```sh
git apply patches/*.patch
```

`scripts/webrtc/export-patches.sh` regenerates them and verifies that property on a clean upstream
checkout; run it after every change.

| Patch | Files | Kind |
|---|---|---|
| `0001-core-per-session-packet-queues` | `src/thread_safe.h`, `src/globals.h`, `src/video.cpp`, `src/audio.cpp` | Upstream change |
| `0002-core-webrtc-server-integration` | `src/config.h`, `src/config.cpp`, `src/main.cpp`, `src/nvhttp.cpp`, `src/confighttp.cpp` | Upstream change |
| `0003-build-libdatachannel` | `cmake/dependencies/common.cmake`, `cmake/compile_definitions/common.cmake`, new `cmake/dependencies/libdatachannel.cmake` | Upstream change + new file |
| `0004-webrtc-module` | new `src/webrtc/*` | New files |
| `0005-web-ui-tv-pairing-and-options` | `Pin.vue`, `configs/config_tabs.json`, `configs/tabs/Network.vue`, `locale/en.json` | Upstream change |
| `0006-tests` | new `tests/unit/webrtc/*`, new `tests/webrtc_e2e/*` | New files |
| `0007-docs` | `README.md`, `docs/api.md`, `docs/configuration.md`, new `docs/moonlight_webrtc_tizen.md` | Upstream change + new file |

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
| `src/config.h`, `src/config.cpp` | New `config::webrtc_t { enabled, port }`, defaults `true` / `8000`, parsed from `webrtc_enabled` and `webrtc_port` (1024–65535). | 8000 is the TV app's default port. |
| `src/main.cpp` | Starts `webrtc_stream::start` on its own `std::jthread` next to the nvhttp, confighttp and RTSP threads. | The server returns when `mail::shutdown` is raised, so shutdown joins it like the others. |
| `src/nvhttp.cpp` | `launch` and `resume` treat a running TV stream like a running Moonlight session (`webrtc_stream::session_count()`). | Otherwise a Moonlight launch while a TV streams would reconfigure the display and re-probe encoders under the TV. |
| `src/confighttp.cpp` | New authenticated, CSRF-checked endpoints `GET /api/webrtc/tvs`, `POST /api/webrtc/pair`, `POST /api/webrtc/unpair-all`. | TV pairing used to live in the Gateway's tray; it now lives in the Web UI. |

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
| `src/webrtc/webrtc_stream.{h,cpp}` | The server: libdatachannel `WebSocketServer`, TV connections (up to 8 unauthenticated, one active), sessions, PeerConnection with send-only H.264/H.265/AV1 + Opus tracks and the `control` (reliable) and `gamepad` (unordered, no retransmits) DataChannels, application launch/resume/stop/switch through `proc::proc`, display configuration, the capture pipeline and the Web UI hooks. |
| `src/webrtc/protocol.{h,cpp}` | Gateway protocol version 2, ported unchanged: stream settings and supported modes, message parsing and construction. |
| `src/webrtc/tv_auth.{h,cpp}` | Pairing window (4-digit PIN, 2 minutes, 3 attempts), paired-TV store (`webrtc_tv_clients.json` in Sunshine's config directory) and HMAC-SHA256 nonce authentication. |
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
| `src_assets/common/assets/web/Pin.vue` | New **Moonlight WebRTC TVs** card: Pair TV (shows the PIN and its state), paired-TV list, Forget All TVs. |
| `configs/config_tabs.json`, `configs/tabs/Network.vue` | `webrtc_enabled` checkbox and `webrtc_port` field on the Network tab. |
| `public/assets/locale/en.json` | Strings for both. Other languages fall back to English, as upstream requires. |

### 0006 — Tests

- `tests/unit/webrtc/` — 31 GoogleTest cases: protocol parsing and capabilities, SDP checks, TV auth
  (including the HMAC vector shared with the TV's JavaScript), and the input bridge, whose packets
  are validated by Sunshine's own `input::testing::is_valid_input_packet()`, plus the packet routing of
  patch 0001.
- `tests/webrtc_e2e/` — a headless-Edge stand-in for the TV that pairs, authenticates, streams and
  measures `getStats()` against a running Sunshine.

The upstream config-consistency test requires every new option in `config_tabs.json`, `en.json` and
`docs/configuration.md`; patches 0005 and 0007 satisfy it.

### 0007 — Documentation

`README.md` gets a note pointing at the guide; `docs/configuration.md` documents both options;
`docs/api.md` lists the three endpoints; `docs/moonlight_webrtc_tizen.md` is the guide.

## Verification of this revision

- Builds with MSYS2 UCRT64 (GCC) without warnings.
- `test_sunshine`: 682 passed, 5 skipped for the environment (no NVIDIA or Intel GPU, no tray build,
  no virtual HID licence, no external shell command), 0 failed.
- End to end on an AMD RX 9060 XT (AMF) with headless Edge: pairing (wrong PIN and unauthenticated
  requests refused), re-authentication, application list and artwork, H.264 720p/1080p and AV1 1080p
  decoded with every frame and 0 packets lost, Opus audio with 0 lost, controller announced,
  stop-session and stop-host-session, PLI answered with IDR, paired TVs persisted across a restart,
  and a clean exit (code 0) when Sunshine shuts down mid-stream.
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
