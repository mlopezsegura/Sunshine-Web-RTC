# Moonlight WebRTC for Samsung Tizen

This fork of Sunshine streams directly to the
[Moonlight WebRTC](https://github.com/tsoas/moonlight-webrtc-tizen) app on Samsung Tizen TVs.
The TV talks to Sunshine itself; the separate Moonlight WebRTC Gateway is no longer needed.

## Why it is built into Sunshine

With the Gateway, every frame crossed two transports on the PC before reaching the TV:

```text
encoder â”€â–º GameStream RTP + FEC + AES â”€â–º loopback UDP â”€â–º moonlight-common-c
        (reassembly, FEC, decryption, frame queue) â”€â–º WebRTC packetizer â”€â–º TV
```

Built into Sunshine, the encoded frame goes from the encoder straight to the WebRTC packetizer:

```text
encoder â”€â–º WebRTC packetizer (RTP over DTLS-SRTP) â”€â–º TV
```

This removes the GameStream packetization, Reed-Solomon FEC, encryption and decryption, the loopback
hop, the client-side frame reassembly and its queue, and a second process. Keyframe requests (RTCP PLI)
reach the encoder directly instead of travelling back through the GameStream control channel. Nothing
else changes: the TV app, its protocol and the encoded bitstream are the same, so the gain is the
host-side pipeline only. The TV's decode time and the network are unaffected.

Because there is no GameStream FEC to pay for, the video encoder receives the bitrate the TV selected,
less the Opus audio and a small RTP allowance, instead of losing 20% to FEC as it did through the Gateway.

## Setup

1. Install this build of Sunshine and complete its normal first-run setup.
2. If the Moonlight WebRTC Gateway is installed on the same PC, uninstall it or stop its service: both
   use TCP port 8000.
3. On the TV, add the PC by its address. The TV app always connects to port 8000, so keep
   `webrtc_port` at its default for a TV.
4. The TV shows a four-digit PIN. In the Sunshine Web UI, open **PIN**, select the TV in the list of
   devices waiting to pair (it appears beside any Moonlight client), and enter the PIN. The name typed
   there is the TV's name in Sunshine.

Pairing works like Moonlight's: the TV picks the PIN and only a user signed in to the Web UI can approve
it. A wrong PIN ends the request and the TV shows a new PIN at once; an unused PIN is replaced after five
minutes. Older TV apps, which entered a PIN shown on the PC, are told to update.

There is no Moonlight pairing between the Gateway and Sunshine any more, so the TV only shows Sunshine as
available while Sunshine is running.

## Configuration

| Option           | Default   | Description                                                   |
|------------------|-----------|---------------------------------------------------------------|
| `webrtc_enabled` | `enabled` | Serve Moonlight WebRTC TVs.                                   |
| `webrtc_port`    | `8000`    | TCP port of the signaling WebSocket. Media uses ephemeral UDP. |

Both are on the **Network** tab of the configuration page.

## Behaviour

The protocol is version 2 of the Moonlight WebRTC Gateway protocol, unchanged, so the existing TV app
works as is. Sunshine implements it as follows:

- **Applications** come from Sunshine's application list, with their configured cover art.
- **Starting** an application launches it like a Moonlight launch, including display configuration and
  encoder probing; an application that is already running is resumed. Starting a different application
  while one runs is refused, as Moonlight does; the TV offers to switch instead.
- **Stopping the stream** leaves the application running. **Stopping the application** ends every
  stream, Moonlight's included, and terminates the application.
- **Codecs** are offered from what Sunshine's encoder probe found: AV1 only when the GPU encodes it, and
  AV1 HDR only with AV1 Main10. HEVC and HEVC HDR requests fail when the encoder lacks them.
- **HDR** requires the host display to be in HDR mode, or Sunshine's display device settings to enable
  it. A stream that asked for HDR ends with an error instead of silently sending SDR.
- **Controllers** are announced to Sunshine as Moonlight controllers, so Sunshine's controller emulation
  and rumble work as with any Moonlight client. Holding Start toggles mouse emulation.
- **Paired TVs** appear in the client list under **Troubleshooting**, like Moonlight clients: a TV can be
  disabled there, which keeps it paired but refuses it until it is enabled again, or unpaired. They are
  stored in `webrtc_tv_clients.json` in Sunshine's configuration directory.

## Web UI API

| Endpoint                       | Description                                         |
|--------------------------------|-----------------------------------------------------|
| `GET /api/pin`                 | Lists TVs waiting to pair beside Moonlight clients. |
| `POST /api/pin`                | Pairs the selected TV when the PIN matches its own. |
| `DELETE /api/pin`              | Declines the selected TV's pairing request.         |
| `GET /api/clients/list`        | Lists paired TVs beside Moonlight clients.          |
| `POST /api/clients/update`     | Enables or disables a TV; disabling disconnects it. |
| `POST /api/clients/unpair`     | Forgets one TV and disconnects it.                  |
| `POST /api/clients/unpair-all` | Forgets every client, TVs included.                 |

## Implementation

| File                           | Role                                                                 |
|--------------------------------|----------------------------------------------------------------------|
| `src/webrtc/webrtc_stream.cpp` | Signaling server, TV sessions, application lifecycle, media pumping. |
| `src/webrtc/protocol.cpp`      | Protocol messages and stream settings.                               |
| `src/webrtc/tv_auth.cpp`       | TV pairing requests, credential store and HMAC authentication.         |
| `src/webrtc/sdp.cpp`           | Samsung Game Mode `imageattr` and HEVC Main10 SDP handling.          |
| `src/webrtc/input_bridge.cpp`  | Gamepad snapshots to Moonlight input packets, mouse mode, rumble.    |

Sunshine's encoders publish to a process-wide queue drained by the GameStream sender. A session can now
register its own `video_packets` and `audio_packets` queues in its mailbox (`mail::packet_queue()`); the
WebRTC stream does, so its frames never reach the GameStream sender. Moonlight sessions are unaffected.

[libdatachannel](https://github.com/paullouisageneau/libdatachannel) provides WebRTC. When no system
package is found it is built from source with Sunshine (`cmake/dependencies/libdatachannel.cmake`).
