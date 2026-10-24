// Drives one end-to-end run against a running Sunshine: serves the TV stand-in page to a headless
// Chromium browser, approves the PIN it shows through the Web UI API, and prints what it measured.
//
//   node driver.mjs --creds=user:password [--webui=https://127.0.0.1:47990] [--ws=ws://127.0.0.1:8000]
//                   [--codec=h264|hevc|av1] [--width=1920 --height=1080 --fps=60 --bitrate=20000 --hdr=1]
//                   [--summary=1] [--unpair=0] [--takeover=1] [--browser=path/to/msedge.exe]
//                   [--latency=SECONDS [--load=noise|none] [--tvTweaks=0] [--label=NAME] [--results=latency.jsonl]]
//
// --latency shows clock.html full screen on this PC's display for the length of the stream and
// prints the glass-to-glass latency the stand-in TV measured; --results appends it as one line.
//
// Sunshine's Web UI uses a self-signed certificate: run with NODE_TLS_REJECT_UNAUTHORIZED=0.
import { spawn, spawnSync } from "node:child_process";
import { appendFileSync, readFileSync, mkdtempSync, writeFileSync } from "node:fs";
import http from "node:http";
import https from "node:https";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Edge keeps its child processes alive when only the parent is killed.
const killEdge = () => {
  for (const browser of [edge, clock]) {
    if (browser) spawnSync("taskkill", ["/T", "/F", "/PID", String(browser.pid)], { stdio: "ignore" });
  }
};
const here = path.dirname(fileURLToPath(import.meta.url));
const args = Object.fromEntries(process.argv.slice(2).map((a) => a.replace(/^--/, "").split("=")));
const webUi = args.webui || "https://127.0.0.1:50001";
const auth = "Basic " + Buffer.from(args.creds || "e2e:e2e-password").toString("base64");

function api(method, route, payload) {
  return new Promise((resolve, reject) => {
    const headers = { Authorization: auth };
    if (payload) {
      // Sunshine's server reads no chunked bodies, which Node would send for a DELETE.
      headers["Content-Type"] = "application/json";
      headers["Content-Length"] = Buffer.byteLength(JSON.stringify(payload));
    }
    const request = https.request(webUi + route, { method, headers, rejectUnauthorized: false }, (response) => {
      let body = "";
      response.on("data", (chunk) => (body += chunk));
      response.on("end", () => resolve({ status: response.statusCode, body: body ? JSON.parse(body) : null }));
    });
    request.on("error", reject);
    request.end(payload ? JSON.stringify(payload) : undefined);
  });
}

// The page shows this PIN like a TV does; the driver plays the user typing it into the Web UI.
const pin = String(Math.floor(Math.random() * 10000)).padStart(4, "0");
const wrongPin = pin === "0000" ? "1111" : "0000";

async function waitForTvRequest(previousId) {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const pending = await api("GET", "/api/pin");
    const tv = (pending.body.pairings || []).find((pairing) => pairing.name === "E2E Edge" && pairing.id !== previousId);
    if (tv) {
      return tv;
    }
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  throw new Error("The TV's pairing request never appeared in /api/pin");
}

// A wrong PIN and a cancellation each end the request; the page asks again and the right PIN pairs it.
async function pairThroughWebUi() {
  const first = await waitForTvRequest();
  console.log("API pin GET: TV listed", JSON.stringify({ name: first.name, address: first.address, idLength: first.id.length }));
  console.log("API pin POST wrong PIN:", JSON.stringify((await api("POST", "/api/pin", { pairing_id: first.id, pin: wrongPin, name: "E2E Edge" })).body));
  const second = await waitForTvRequest(first.id);
  console.log("API pin DELETE:", JSON.stringify((await api("DELETE", "/api/pin", { pairing_id: second.id })).body));
  const third = await waitForTvRequest(second.id);
  console.log("API pin POST stale id:", JSON.stringify((await api("POST", "/api/pin", { pairing_id: first.id, pin, name: "E2E Edge" })).body));
  console.log("API pin POST right PIN:", JSON.stringify((await api("POST", "/api/pin", { pairing_id: third.id, pin, name: "E2E Living Room" })).body));
}

const tvClients = async () => ((await api("GET", "/api/clients/list")).body.named_certs || []).filter((client) => client.name.startsWith("E2E"));
const clientsBefore = await tvClients();
console.log("API clients before:", JSON.stringify(clientsBefore));

const query = new URLSearchParams({ ws: args.ws || "ws://127.0.0.1:8010", pin, codec: args.codec || "h264",
  width: args.width || "1280", height: args.height || "720", bitrate: args.bitrate || "12000", hdr: args.hdr || "0", fps: args.fps || "60", takeover: args.takeover || "0",
  latency: args.latency || "0", tvTweaks: args.tvTweaks || "1" });
let edge;
let clock;
const server = http.createServer((request, response) => {
  if (request.method === "POST" && request.url === "/result") {
    let body = "";
    request.on("data", (chunk) => (body += chunk));
    request.on("end", async () => {
      response.end("ok");
      const result = JSON.parse(body);
      if (result.latency) {
        const line = { label: args.label || "", codec: result.codec, size: result.width + "x" + result.height, fps: result.fps,
          bitrateKbps: Number(args.bitrate || 12000), tvTweaks: args.tvTweaks !== "0",
          playoutDelayOffered: Boolean((result.steps.find((s) => s.step === "offer") || {}).playoutDelay), ...result.latency, errors: result.errors };
        console.log("LATENCY " + JSON.stringify(line));
        if (args.results) appendFileSync(args.results, JSON.stringify(line) + "\n");
        if (result.debugFrame && args.debugFrame) {
          writeFileSync(args.debugFrame, Buffer.from(result.debugFrame.png.split(",")[1], "base64"));
          console.log("DEBUG " + JSON.stringify({ ...result.debugFrame, png: undefined }));
        }
      }
      if (args.summary === "1") {
        console.log(JSON.stringify({ codec: result.codec, size: result.width + "x" + result.height, fps: result.fps, hdr: result.hdr, errors: result.errors,
          states: result.steps.filter((s) => s.step.startsWith("session-") || s.step.startsWith("host-") || s.step.startsWith("takeover") || s.step.startsWith("replaced") || s.step === "offer").map((s) => s.step + (s.message ? "(" + s.message + ")" : "") + (s.rtpmap ? "[" + s.rtpmap + "]" : "")),
          video: result.stats?.video, audio: result.stats?.audio, codecs: result.stats?.codecs, measuredFps: result.measuredFps, measuredKbps: result.measuredKbps }, null, 1));
      } else {
        console.log(JSON.stringify(result, null, 1));
      }
      // The paired TV is listed, disabled and unpaired like any Moonlight client.
      const tv = (await tvClients()).find((client) => !clientsBefore.some((old) => old.uuid === client.uuid));
      console.log("API clients after:", JSON.stringify(tv));
      console.log("API clients disable:", JSON.stringify((await api("POST", "/api/clients/update", { uuid: tv.uuid, enabled: false })).body), JSON.stringify(await tvClients()));
      if (args.unpair !== "0") {
        console.log("API clients unpair:", JSON.stringify((await api("POST", "/api/clients/unpair", { uuid: tv.uuid })).body), JSON.stringify(await tvClients()));
      }
      killEdge();
      server.close();
      process.exit(0);
    });
    return;
  }
  response.setHeader("Content-Type", "text/html");
  response.end(readFileSync(path.join(here, request.url.startsWith("/clock") ? "clock.html" : "client.html")));
});
server.listen(8099, "127.0.0.1", () => {
  if (Number(args.latency || 0) > 0) {
    clock = spawn(args.browser || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe", [
      "--kiosk", "--edge-kiosk-type=fullscreen", "--no-first-run", "--user-data-dir=" + mkdtempSync(path.join(tmpdir(), "edge-clock-")),
      "http://127.0.0.1:8099/clock?load=" + (args.load || "noise")], { stdio: "ignore" });
  }
  const profile = mkdtempSync(path.join(tmpdir(), "edge-e2e-"));
  edge = spawn(args.browser || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe", [
    "--headless=new", "--no-first-run", "--user-data-dir=" + profile, "--autoplay-policy=no-user-gesture-required",
    "--remote-debugging-port=0", "--enable-features=WebRtcAllowH265Receive,PlatformHEVCDecoderSupport", "http://127.0.0.1:8099/?" + query], { stdio: "ignore" });
  pairThroughWebUi().catch((error) => { console.log("pairing failed:", error.message); killEdge(); process.exit(3); });
});
setTimeout(() => { console.log("driver timeout"); killEdge(); process.exit(2); }, 90000 + Number(args.latency || 0) * 1000);
