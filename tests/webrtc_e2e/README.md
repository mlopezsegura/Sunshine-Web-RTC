# Moonlight WebRTC end-to-end test

`driver.mjs` checks the Moonlight WebRTC TV server of a running Sunshine from end to end. It loads
`client.html` in headless Edge, which plays the TV: it asks to pair showing a PIN, which the driver
approves through `/api/pin` like a user in the Web UI would (after a wrong PIN and a cancellation); the
page then reconnects and authenticates, lists the applications, fetches
artwork, starts a stream, announces a gamepad, measures the decoded video and audio through `getStats()`,
and finally stops the stream and the application. The driver prints the steps and statistics.

```sh
NODE_TLS_REJECT_UNAUTHORIZED=0 node driver.mjs --creds=user:password --webui=https://127.0.0.1:47990 \
  --ws=ws://127.0.0.1:8000 --codec=h264 --width=1920 --height=1080 --bitrate=20000 --summary=1
```

Headless Edge decodes H.264 and AV1 8-bit. It has no HEVC or AV1 10-bit WebRTC decoder, so those
streams are established but their frames are only received, not decoded; a Samsung TV decodes them.

## Glass-to-glass latency

`--latency=SECONDS` measures the whole path from this PC's screen to the stand-in TV's: capture,
encode, RTP, jitter buffer, decode and presentation. The driver shows `clock.html` full screen on the
captured display; it paints the wall clock as a row of cells along the top edge, over moving noise
(`--load=none` for a still picture) so the encoder runs at the requested bitrate. The page reads the
row back from every frame it presents and subtracts. Both pages run on the same PC, so they share
the clock. `--tvTweaks=0` measures without what the TV app does to its receiver (no lip sync,
shortest jitter buffer).

```sh
node driver.mjs --creds=user:password --webui=https://127.0.0.1:50001 --ws=ws://127.0.0.1:8010 \
  --codec=av1 --width=3840 --height=2160 --bitrate=80000 --latency=20 --results=latency.jsonl
```

`latency.ps1` compares Sunshine builds: it starts each one with a test configuration, runs every
mode and appends one JSON line per run. Nothing else should be on screen while it runs.

```powershell
./latency.ps1 -Sunshine ([ordered]@{ before = 'C:\old\sunshine.exe'; after = 'C:\new\sunshine.exe' }) `
  -Config C:\e2e\sunshine.conf -Modes 'h264:1920:1080:60:20000', 'av1:3840:2160:60:80000' -Repeat 2
```

On a Ryzen 9 5950X with a Radeon RX 9060 XT (AMF), zero playout delay took H.264 1080p60 at 20 Mbps
from 54 ms to 38 ms, and AV1 2160p60 at 80 Mbps from 73 ms to about 50 ms. The figures include
headless Edge's software decoder and its 60 Hz compositor; a TV decodes in hardware.
