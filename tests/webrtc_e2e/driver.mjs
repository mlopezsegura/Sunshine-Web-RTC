// Drives one end-to-end run against a running Sunshine: pairs through the Web UI API, serves the
// TV stand-in page to a headless Chromium browser, and prints what the page measured.
//
//   node driver.mjs --creds=user:password [--webui=https://127.0.0.1:47990] [--ws=ws://127.0.0.1:8000]
//                   [--codec=h264|hevc|av1] [--width=1920 --height=1080 --bitrate=20000 --hdr=1]
//                   [--summary=1] [--unpair=0] [--browser=path/to/msedge.exe]
//
// Sunshine's Web UI uses a self-signed certificate: run with NODE_TLS_REJECT_UNAUTHORIZED=0.
import { spawn } from "node:child_process";
import { readFileSync, mkdtempSync } from "node:fs";
import http from "node:http";
import https from "node:https";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = Object.fromEntries(process.argv.slice(2).map((a) => a.replace(/^--/, "").split("=")));
const webUi = args.webui || "https://127.0.0.1:50001";
const auth = "Basic " + Buffer.from(args.creds || "e2e:e2e-password").toString("base64");

function api(method, route) {
  return new Promise((resolve, reject) => {
    const request = https.request(webUi + route, { method, headers: { Authorization: auth }, rejectUnauthorized: false }, (response) => {
      let body = "";
      response.on("data", (chunk) => (body += chunk));
      response.on("end", () => resolve({ status: response.statusCode, body: body ? JSON.parse(body) : null }));
    });
    request.on("error", reject);
    request.end();
  });
}

const before = await api("GET", "/api/webrtc/tvs");
const pairing = await api("POST", "/api/webrtc/pair");
console.log("API tvs:", JSON.stringify(before.body), "pair:", pairing.status, pairing.body.status, "pin length", (pairing.body.pin || "").length);

const query = new URLSearchParams({ ws: args.ws || "ws://127.0.0.1:8010", pin: pairing.body.pin, codec: args.codec || "h264",
  width: args.width || "1280", height: args.height || "720", bitrate: args.bitrate || "12000", hdr: args.hdr || "0" });
let edge;
const server = http.createServer((request, response) => {
  if (request.method === "POST" && request.url === "/result") {
    let body = "";
    request.on("data", (chunk) => (body += chunk));
    request.on("end", async () => {
      response.end("ok");
      const result = JSON.parse(body);
      if (args.summary === "1") {
        console.log(JSON.stringify({ codec: result.codec, size: result.width + "x" + result.height, hdr: result.hdr, errors: result.errors,
          states: result.steps.filter((s) => s.step.startsWith("session-") || s.step.startsWith("host-") || s.step === "offer").map((s) => s.step + (s.message ? "(" + s.message + ")" : "") + (s.rtpmap ? "[" + s.rtpmap + "]" : "")),
          video: result.stats?.video, audio: result.stats?.audio, codecs: result.stats?.codecs, measuredFps: result.measuredFps, measuredKbps: result.measuredKbps }, null, 1));
      } else {
        console.log(JSON.stringify(result, null, 1));
      }
      const after = await api("GET", "/api/webrtc/tvs");
      console.log("API tvs after:", JSON.stringify(after.body));
      if (args.unpair !== "0") {
        console.log("API unpair-all:", JSON.stringify((await api("POST", "/api/webrtc/unpair-all")).body));
      }
      edge.kill();
      server.close();
      process.exit(0);
    });
    return;
  }
  response.setHeader("Content-Type", "text/html");
  response.end(readFileSync(path.join(here, "client.html")));
});
server.listen(8099, "127.0.0.1", () => {
  const profile = mkdtempSync(path.join(tmpdir(), "edge-e2e-"));
  edge = spawn(args.browser || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe", [
    "--headless=new", "--no-first-run", "--user-data-dir=" + profile, "--autoplay-policy=no-user-gesture-required",
    "--remote-debugging-port=0", "--enable-features=WebRtcAllowH265Receive,PlatformHEVCDecoderSupport", "http://127.0.0.1:8099/?" + query], { stdio: "ignore" });
});
setTimeout(() => { console.log("driver timeout"); edge?.kill(); process.exit(2); }, 90000);
