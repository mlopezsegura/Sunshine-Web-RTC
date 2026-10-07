# Moonlight WebRTC end-to-end test

`driver.mjs` checks the Moonlight WebRTC TV server of a running Sunshine from end to end. It opens a
pairing window through the Web UI API, then loads `client.html` in headless Edge, which plays the TV:
it pairs with the PIN (after a wrong one), reconnects and authenticates, lists the applications, fetches
artwork, starts a stream, announces a gamepad, measures the decoded video and audio through `getStats()`,
and finally stops the stream and the application. The driver prints the steps and statistics.

```sh
NODE_TLS_REJECT_UNAUTHORIZED=0 node driver.mjs --creds=user:password --webui=https://127.0.0.1:47990 \
  --ws=ws://127.0.0.1:8000 --codec=h264 --width=1920 --height=1080 --bitrate=20000 --summary=1
```

Headless Edge decodes H.264 and AV1 8-bit. It has no HEVC or AV1 10-bit WebRTC decoder, so those
streams are established but their frames are only received, not decoded; a Samsung TV decodes them.
