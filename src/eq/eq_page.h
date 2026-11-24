#pragma once

// Single-file measurement page for the phone. No frameworks, no CDN, no
// external requests. The server substitutes __TITLE__, __TOKEN__, __TOTAL__,
// __RATE__ and __SECURE__ before serving.
static const char *kRoomEqPageHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0e1014">
<title>crossfade · room calibration</title>
<style>
:root{--accent:#6ee7a8;--bg:#0e1014;--surface:#171a20;--surface2:#1e222a;--text:#f2f4f8;--dim:#a0a7b4;--muted:#6b7280;--line:#2a2f39;--warn:#f0b45c;--err:#ef7b7b}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{background:var(--bg);color:var(--text);font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;min-height:100vh;padding:calc(env(safe-area-inset-top) + 18px) 18px calc(env(safe-area-inset-bottom) + 24px)}
.wrap{max-width:520px;margin:0 auto}
header{display:flex;align-items:center;gap:10px;margin-bottom:6px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--accent);box-shadow:0 0 0 4px rgba(110,231,168,.15),0 0 22px rgba(110,231,168,.5)}
h1{font-size:17px;font-weight:650;margin:0}
.sub{color:var(--muted);font-size:13px;margin:0 0 18px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:16px;padding:16px;margin-bottom:14px}
.steps{display:flex;gap:8px;margin-bottom:16px}
.step{flex:1;height:4px;border-radius:99px;background:var(--surface2)}
.step.on{background:var(--accent)}
h2{font-size:15px;margin:0 0 8px;font-weight:600}
p{margin:0 0 10px;color:var(--dim);font-size:14px}
ol{margin:0;padding-left:20px;color:var(--dim);font-size:14px}
ol li{margin-bottom:6px}
button{font:inherit;cursor:pointer;touch-action:manipulation;width:100%;height:52px;border:0;border-radius:13px;background:var(--accent);color:#07130c;font-weight:700;font-size:16px}
button:disabled{opacity:.5}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--dim);height:46px;font-weight:600;margin-top:10px}
.meter{height:14px;border-radius:99px;background:var(--surface2);overflow:hidden;margin:12px 0 6px}
.meter i{display:block;height:100%;width:0;background:var(--accent);transition:width .08s linear}
.meter.warn i{background:var(--warn)}
.meter.bad i{background:var(--err)}
.row{display:flex;justify-content:space-between;font-size:12px;color:var(--muted)}
.big{font-size:34px;font-weight:700;letter-spacing:-.01em;margin:6px 0 2px}
.big small{font-size:15px;color:var(--muted);font-weight:500}
.note{font-size:12px;color:var(--muted);margin-top:12px}
.err{color:var(--err)}
.ok{color:var(--accent)}
input[type=file]{display:none}
.wave{display:flex;align-items:center;justify-content:center;gap:4px;height:56px;margin:10px 0}
.wave i{width:4px;border-radius:99px;background:var(--accent);opacity:.85}
</style>
</head>
<body>
<div class="wrap">
  <header><span class="dot"></span><h1>__TITLE__</h1></header>
  <p class="sub">Room correction · listening position</p>
  <div class="steps"><div class="step on" id="s1"></div><div class="step" id="s2"></div><div class="step" id="s3"></div></div>

  <div class="card" id="intro">
    <h2>Before you start</h2>
    <ol>
      <li>Put the phone where you normally sit, roughly ear height.</li>
      <li>Don't cover the microphone (usually the small hole at the bottom edge).</li>
      <li>Keep the room quiet and stay still during the sweep.</li>
      <li>Turn the speakers up to a comfortable volume — the sweep is short.</li>
    </ol>
    <p class="note">This page uses the phone microphone, so the result is an <b>approximate</b> in-room curve, best above ~100 Hz. A calibrated USB mic is more accurate.</p>
    <button id="start" style="margin-top:14px">Start calibration</button>
  </div>

  <div class="card" id="live" hidden>
    <h2 id="liveTitle">Check the level</h2>
    <div class="big" id="liveBig">— <small>dB</small></div>
    <div class="meter" id="meter"><i id="meterFill"></i></div>
    <div class="row"><span id="levelHint">Make some noise to test the mic</span><span id="countdown"></span></div>
    <div class="wave" id="wave"></div>
    <div class="note" id="liveNote">The sweep starts by itself. Keep quiet until it finishes.</div>
  </div>

  <div class="card" id="done" hidden>
    <h2 class="ok" id="doneTitle">Done</h2>
    <p id="doneText">Look at the crossfade window on your computer.</p>
    <button class="ghost" id="again">Measure again</button>
  </div>

  <div class="card" id="fail" hidden>
    <h2 class="err">Couldn't measure</h2>
    <p id="failText"></p>
    <button class="ghost" id="retry">Try again</button>
  </div>

  <div class="card" id="uploadCard" hidden>
    <h2>Use a recording</h2>
    <p>Already captured the sweep with another microphone or recorder? Upload the file. Any format your browser can decode works.</p>
    <button class="ghost" id="pick">Choose a recording</button>
    <input type="file" id="file" accept="audio/*">
    <p class="note" id="uploadNote"></p>
  </div>
</div>

<script>
"use strict";
const TOKEN = "__TOKEN__";
const TOTAL = __TOTAL__;
const RATE = __RATE__;
const SECURE = __SECURE__;
const $ = (id) => document.getElementById(id);
const base = "/e/" + TOKEN;

let ctx = null, stream = null, proc = null, analyser = null, chunks = [], rec = false;
let timer = null, poll = null;

function show(which) {
  for (const id of ["intro", "live", "done", "fail", "uploadCard"])
    $(id).hidden = (id !== which);
}
function setSteps(n) {
  $("s1").classList.toggle("on", n >= 1);
  $("s2").classList.toggle("on", n >= 2);
  $("s3").classList.toggle("on", n >= 3);
}
function fail(msg) {
  if (poll) { clearInterval(poll); poll = null; }
  $("failText").textContent = msg;
  show("fail");
}
function cleanup() {
  if (timer) { clearTimeout(timer); timer = null; }
  if (proc) { try { proc.disconnect(); } catch (e) {} proc = null; }
  if (stream) { stream.getTracks().forEach(t => t.stop()); stream = null; }
  if (ctx) { try { ctx.close(); } catch (e) {} ctx = null; }
  analyser = null;
}
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function meterStatus(dbfs) {
  if (dbfs < -48) return ["Too quiet", "warn"];
  if (dbfs < -14) return ["Good", ""];
  if (dbfs < -3) return ["Loud", "warn"];
  return ["Clipping", "bad"];
}

async function begin() {
  if (!SECURE || !navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    fail("This page needs a secure (HTTPS) connection to use the microphone. Scan the QR code again from the crossfade window, or use the PC microphone there.");
    return;
  }
  $("start").disabled = true;
  try {
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1 },
      video: false
    });
  } catch (e) {
    $("start").disabled = false;
    fail("Microphone permission was denied. Allow it in the browser's site settings and try again.");
    return;
  }
  try {
    ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: RATE });
  } catch (e) {
    ctx = new (window.AudioContext || window.webkitAudioContext)();
  }
  const src = ctx.createMediaStreamSource(stream);
  analyser = ctx.createAnalyser();
  analyser.fftSize = 2048;
  src.connect(analyser);
  // ScriptProcessor needs a sink to be pulled; a muted gain keeps it silent
  // (never route the mic back to the speaker).
  proc = ctx.createScriptProcessor(4096, 1, 1);
  const mute = ctx.createGain();
  mute.gain.value = 0;
  src.connect(proc);
  proc.connect(mute);
  mute.connect(ctx.destination);
  proc.onaudioprocess = (e) => {
    if (!rec) return;
    chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)));
  };
  if (ctx.state === "suspended") await ctx.resume();

  for (const id of ["wave"]) {
    const w = $(id);
    w.innerHTML = "";
    for (let i = 0; i < 24; i++) {
      const b = document.createElement("i");
      b.style.height = "6px";
      w.appendChild(b);
    }
  }
  show("live");
  setSteps(2);
  $("liveTitle").textContent = "Check the level";
  $("liveBig").innerHTML = "— <small>dB</small>";
  $("levelHint").textContent = "Make some noise to test the mic";
  $("liveNote").textContent = "The sweep starts by itself. Keep quiet until it finishes.";

  // Give the user a moment to see the meter before the sweep starts.
  let frames = 0;
  const tick = () => {
    if (!analyser || !ctx) return;
    const buf = new Float32Array(analyser.fftSize);
    analyser.getFloatTimeDomainData(buf);
    let sum = 0, peak = 0;
    for (let i = 0; i < buf.length; i++) {
      sum += buf[i] * buf[i];
      peak = Math.max(peak, Math.abs(buf[i]));
    }
    const db = 20 * Math.log10(Math.max(1e-8, Math.sqrt(sum / buf.length)));
    const [label, cls] = meterStatus(db);
    $("liveBig").innerHTML = (db > -90 ? db.toFixed(0) : "—") + " <small>dB</small>";
    $("levelHint").textContent = label;
    $("meter").className = "meter " + cls;
    $("meterFill").style.width = Math.max(0, Math.min(100, (db + 60) * 1.8)) + "%";
    const bars = $("wave").children;
    for (let i = 0; i < bars.length; i++) {
      const v = Math.max(0, Math.min(1, (peak * 6) * Math.abs(Math.sin(i * 0.7 + frames * 0.35))));
      bars[i].style.height = (6 + v * 46) + "px";
    }
    frames++;
    if (analyser && ctx) requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);

  await sleep(900);
  await run();
}

async function run() {
  chunks = [];
  rec = true;
  $("liveTitle").textContent = "Measuring";
  const started = performance.now();
  try {
    await fetch(base + "/ready", { method: "POST" });
  } catch (e) {
    cleanup();
    fail("Lost the connection to crossfade. Make sure the app is still open and try again.");
    return;
  }
  const dur = (TOTAL + 1.0) * 1000;
  timer = setTimeout(async () => {
    timer = null;
    rec = false;
    $("countdown").textContent = "";
    $("liveTitle").textContent = "Processing";
    $("liveNote").textContent = "Sending the recording…";
    await finish();
    void started;
  }, dur);
  const tickDown = () => {
    if (!timer) return;
    const left = Math.max(0, dur - (performance.now() - started)) / 1000;
    $("countdown").textContent = left > 0.5 ? left.toFixed(1) + " s" : "";
    requestAnimationFrame(tickDown);
  };
  requestAnimationFrame(tickDown);
}

function flattenPcm() {
  let len = 0;
  for (const c of chunks) len += c.length;
  const out = new Float32Array(len);
  let at = 0;
  for (const c of chunks) { out.set(c, at); at += c.length; }
  chunks = [];
  return out;
}

async function resampleTo(float32, rate) {
  if (Math.abs(rate - RATE) < 100) return float32;
  const frames = Math.max(1, Math.round(float32.length * RATE / rate));
  const off = new OfflineAudioContext(1, frames, RATE);
  const buf = off.createBuffer(1, float32.length, rate);
  buf.copyToChannel(float32, 0);
  const src = off.createBufferSource();
  src.buffer = buf;
  src.connect(off.destination);
  src.start();
  const rendered = await off.startRendering();
  return rendered.getChannelData(0);
}

function toInt16(f32) {
  const out = new Int16Array(f32.length);
  for (let i = 0; i < f32.length; i++) {
    const v = Math.max(-1, Math.min(1, f32[i]));
    out[i] = v < 0 ? v * 32768 : v * 32767;
  }
  return out;
}

async function uploadPcm(f32, rate) {
  const pcm = await resampleTo(f32, rate);
  const body = toInt16(pcm);
  try {
    const r = await fetch(base + "/upload?rate=" + RATE, {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body
    });
    if (!r.ok) throw new Error(await r.text());
  } catch (e) {
    cleanup();
    fail("Could not upload the recording to crossfade. Try again from a closer spot.");
    return;
  }
  pollStatus();
}

async function finish() {
  const rate = ctx ? ctx.sampleRate : RATE;
  const pcm = flattenPcm();
  cleanup();
  setSteps(3);
  if (pcm.length > 0) {
    await uploadPcm(pcm, rate);
  } else {
    fail("No audio was captured. Check the microphone permission and try again.");
  }
}

function pollStatus() {
  if (poll) clearInterval(poll);
  poll = setInterval(async () => {
    try {
      const r = await fetch(base + "/status");
      const s = await r.json();
      if (s.state === "done") {
        clearInterval(poll); poll = null;
        $("doneTitle").textContent = "Done";
        $("doneText").textContent = "Look at the crossfade window on your computer to review and apply the correction.";
        show("done");
      } else if (s.state === "error") {
        clearInterval(poll); poll = null;
        fail(s.message || "The measurement could not be used.");
      } else {
        $("liveTitle").textContent = "Processing";
        $("liveNote").textContent = s.message || "Analysing the room…";
        show("live");
      }
    } catch (e) {
      clearInterval(poll); poll = null;
      fail("Lost the connection to crossfade.");
    }
  }, 700);
}

async function uploadFile(file) {
  if (!file) return;
  $("uploadNote").textContent = "Decoding…";
  try {
    const data = await file.arrayBuffer();
    const dec = new (window.AudioContext || window.webkitAudioContext)();
    const buf = await dec.decodeAudioData(data);
    dec.close();
    const rate = buf.sampleRate;
    const mono = new Float32Array(buf.length);
    for (let ch = 0; ch < buf.numberOfChannels; ch++) {
      const d = buf.getChannelData(ch);
      for (let i = 0; i < d.length; i++) mono[i] += d[i] / buf.numberOfChannels;
    }
    // Normalise a little so a quiet recorder still has usable headroom.
    let peak = 0;
    for (let i = 0; i < mono.length; i++) peak = Math.max(peak, Math.abs(mono[i]));
    if (peak > 0.98) $("uploadNote").textContent = "That file is clipping; the result may be poor.";
    show("live");
    setSteps(3);
    $("liveTitle").textContent = "Processing";
    $("liveNote").textContent = "Sending the recording…";
    await uploadPcm(mono, rate);
  } catch (e) {
    $("uploadNote").textContent = "This browser could not decode that file. Try WAV or M4A.";
  }
}

$("start").addEventListener("click", begin);
$("again").addEventListener("click", () => { show("intro"); setSteps(1); $("start").disabled = false; });
$("retry").addEventListener("click", () => { show("intro"); setSteps(1); $("start").disabled = false; });
$("pick").addEventListener("click", () => $("file").click());
$("file").addEventListener("change", (e) => uploadFile(e.target.files && e.target.files[0]));

// Lets the host pick the right microphone profile (e.g. a Galaxy S24) when the
// user has not chosen one. Body, not a query string: UAs contain spaces.
fetch(base + "/hello", {
  method: "POST",
  headers: { "Content-Type": "text/plain" },
  body: navigator.userAgent
}).catch(function () {});

if (!SECURE) {
  $("uploadCard").hidden = false;
  $("start").disabled = true;
  $("start").textContent = "Microphone needs HTTPS";
  $("intro").insertAdjacentHTML("beforeend",
    "<p class='note err'>This address is plain HTTP, so browsers block microphone access. Reopen the link from the crossfade window (HTTPS), or use a recording.</p>");
} else {
  $("uploadCard").hidden = false;
}
</script>
</body>
</html>
)HTML";
