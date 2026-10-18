#pragma once

/* The single page served at "/" (web_server.cpp). Plain HTML/CSS/JS, no external files. */
static const char k_web_page[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>JC3248 panel</title>
<style>
:root { --bg:#f4f5f7; --card:#fff; --text:#1d2330; --muted:#6b7280; --line:#e3e6eb; --accent:#2563eb; --ok:#15803d; --err:#b91c1c; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#111418; --card:#1b2027; --text:#e7eaee; --muted:#9aa3ae; --line:#2b323b; --accent:#60a5fa; --ok:#4ade80; --err:#f87171; }
}
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:15px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width:720px; margin:0 auto; padding:16px; }
h1 { font-size:20px; margin:4px 0 16px; }
h2 { font-size:15px; margin:0 0 12px; color:var(--muted); font-weight:600; text-transform:uppercase; letter-spacing:.04em; }
section { background:var(--card); border:1px solid var(--line); border-radius:10px; padding:16px; margin-bottom:14px; }
.grid { display:grid; grid-template-columns:repeat(auto-fit, minmax(150px, 1fr)); gap:10px 18px; }
.k { color:var(--muted); font-size:13px; }
.v { font-size:17px; font-weight:600; overflow-wrap:anywhere; }
.big { font-size:28px; }
label { display:block; margin:10px 0 4px; font-size:13px; color:var(--muted); }
input, select { width:100%; padding:8px 10px; font:inherit; color:var(--text); background:var(--bg); border:1px solid var(--line); border-radius:6px; }
input[type=range] { padding:0; }
.row { display:flex; gap:10px; }
.row > * { flex:1; }
.seg { display:flex; gap:6px; flex-wrap:wrap; }
.seg label { display:inline-flex; align-items:center; gap:6px; margin:0; padding:6px 10px; border:1px solid var(--line); border-radius:6px; color:var(--text); font-size:14px; cursor:pointer; }
.seg input { width:auto; }
button { margin-top:14px; padding:9px 16px; font:inherit; font-weight:600; color:#fff; background:var(--accent); border:0; border-radius:6px; cursor:pointer; }
.msg { margin-left:10px; font-size:13px; }
.ok { color:var(--ok); } .err { color:var(--err); }
.hidden { display:none; }
footer { color:var(--muted); font-size:12px; text-align:center; margin:8px 0 20px; }
</style>
</head>
<body>
<main>
<h1>JC3248 panel</h1>

<section>
  <h2>Status</h2>
  <div class="grid">
    <div><div class="k">Time</div><div class="v" id="time">-</div></div>
    <div><div class="k">Wi-Fi</div><div class="v" id="wifi">-</div></div>
    <div><div class="k">Uptime</div><div class="v" id="uptime">-</div></div>
    <div><div class="k">CPU (core 0 / 1)</div><div class="v" id="cpu">-</div></div>
    <div><div class="k">Internal RAM free (min)</div><div class="v" id="ram">-</div></div>
    <div><div class="k">PSRAM free</div><div class="v" id="psram">-</div></div>
  </div>
</section>

<section>
  <h2>Weather</h2>
  <div class="grid">
    <div><div class="k" id="w_loc">-</div><div class="v big" id="w_temp">-</div></div>
    <div><div class="k">Conditions</div><div class="v" id="w_text">-</div></div>
    <div><div class="k">Humidity / wind</div><div class="v" id="w_hum">-</div></div>
    <div><div class="k">Updated</div><div class="v" id="w_upd">-</div></div>
  </div>
</section>

<section>
  <h2>Radon</h2>
  <div class="grid">
    <div><div class="k" id="a_model">-</div><div class="v big" id="a_st">-</div></div>
    <div><div class="k">Long-term average</div><div class="v" id="a_lt">-</div></div>
    <div><div class="k">Temperature / humidity</div><div class="v" id="a_th">-</div></div>
    <div><div class="k">Updated</div><div class="v" id="a_upd">-</div></div>
  </div>
</section>

<section>
  <h2>Settings</h2>
  <label for="bright">Brightness <span id="bright_v"></span></label>
  <input type="range" id="bright" min="10" max="100" step="5">
  <label>Temperature units</label>
  <div class="seg">
    <label><input type="radio" name="units" value="auto"> Auto</label>
    <label><input type="radio" name="units" value="celsius"> °C</label>
    <label><input type="radio" name="units" value="fahrenheit"> °F</label>
  </div>
  <label>Clock</label>
  <div class="seg">
    <label><input type="radio" name="clock" value="24"> 24 h</label>
    <label><input type="radio" name="clock" value="12"> 12 h</label>
  </div>
  <label for="tz">Timezone</label>
  <select id="tz"></select>
  <button id="save_settings">Save settings</button><span class="msg" id="msg_settings"></span>
</section>

<section>
  <h2>Weather location</h2>
  <div class="seg">
    <label><input type="radio" name="locmode" value="auto"> Automatic (from IP address)</label>
    <label><input type="radio" name="locmode" value="manual"> Fixed location</label>
  </div>
  <div id="loc_fields">
    <div class="row">
      <div><label for="lat">Latitude</label><input id="lat" type="number" step="0.0001" min="-90" max="90"></div>
      <div><label for="lon">Longitude</label><input id="lon" type="number" step="0.0001" min="-180" max="180"></div>
    </div>
    <label for="locname">Name shown on the panel</label>
    <input id="locname" maxlength="40">
  </div>
  <button id="save_loc">Save location</button><span class="msg" id="msg_loc"></span>
</section>

<footer>Refreshes every 10 s</footer>
</main>

<script>
const $ = id => document.getElementById(id);
const radio = (name, v) => { const r = document.querySelector(`input[name="${name}"][value="${v}"]`); if (r) r.checked = true; };
const picked = name => { const r = document.querySelector(`input[name="${name}"]:checked`); return r ? r.value : null; };
const kb = b => Math.round(b / 1024) + ' KB';
const ago = t => {
  if (!t) return '-';
  const s = Math.max(0, Math.round(Date.now() / 1000 - t));
  return s < 90 ? s + ' s ago' : s < 5400 ? Math.round(s / 60) + ' min ago' : Math.round(s / 3600) + ' h ago';
};
function say(id, text, ok) { const m = $(id); m.textContent = text; m.className = 'msg ' + (ok ? 'ok' : 'err'); }

async function postJson(url, body) {
  const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  if (!r.ok) throw new Error(await r.text() || r.statusText);
}

async function loadStatus() {
  try {
    const s = await (await fetch('/api/status')).json();
    $('time').textContent = s.time || 'not synced';
    $('wifi').textContent = s.wifi.ssid ? `${s.wifi.ssid} (${s.wifi.rssi} dBm)` : 'not connected';
    const up = s.uptime_s, d = Math.floor(up / 86400), h = Math.floor(up % 86400 / 3600), m = Math.floor(up % 3600 / 60);
    $('uptime').textContent = (d ? d + ' d ' : '') + h + ' h ' + m + ' min';
    $('cpu').textContent = s.cpu ? `${s.cpu[0]} % / ${s.cpu[1]} %` : '-';
    $('ram').textContent = `${kb(s.heap_free)} (${kb(s.heap_min)})`;
    $('psram').textContent = kb(s.psram_free);

    const w = s.weather;
    if (w.valid) {
      $('w_loc').textContent = w.location;
      $('w_temp').textContent = w.temp.toFixed(1) + '°' + w.unit;
      $('w_text').textContent = `${w.text}, feels ${w.feels.toFixed(0)}°`;
      $('w_hum').textContent = `${w.humidity} % / ${w.wind.toFixed(1)} ${w.unit === 'F' ? 'mph' : 'km/h'}`;
      $('w_upd').textContent = ago(w.updated) + (w.status ? ` (${w.status})` : '');
    } else {
      $('w_loc').textContent = 'Weather';
      $('w_temp').textContent = '-';
      $('w_text').textContent = w.status || 'no data yet';
    }

    const a = s.air;
    if (!a.configured) {
      $('a_model').textContent = 'No sensor chosen (Air app)';
    } else {
      $('a_model').textContent = `${a.model}, 24 h average`;
      if (a.valid) {
        $('a_st').textContent = a.radon_st >= 0 ? `${a.radon_st} Bq/m³` : 'not yet';
        $('a_lt').textContent = a.radon_lt >= 0 ? `${a.radon_lt} Bq/m³ (${(a.radon_lt / 37).toFixed(1)} pCi/L)` : 'not yet';
        $('a_th').textContent = `${a.temp_c.toFixed(1)} °C / ${a.humidity.toFixed(0)} %`;
        $('a_upd').textContent = ago(a.updated) + (a.status ? ` (${a.status})` : '');
      } else {
        $('a_st').textContent = '-';
        $('a_upd').textContent = a.status || 'no reading yet';
      }
    }
  } catch (e) {
    $('time').textContent = 'board not reachable';
  }
}

function showLocFields() { $('loc_fields').classList.toggle('hidden', picked('locmode') !== 'manual'); }

async function loadSettings() {
  const s = await (await fetch('/api/settings')).json();
  $('bright').value = s.brightness;
  $('bright_v').textContent = s.brightness + ' %';
  radio('units', s.units);
  radio('clock', s.clock_24h ? '24' : '12');
  const tz = $('tz');
  tz.innerHTML = '';
  for (const t of s.timezones) {
    const o = document.createElement('option');
    o.value = t.posix; o.textContent = t.label;
    tz.appendChild(o);
  }
  if (![...tz.options].some(o => o.value === s.timezone)) {
    const o = document.createElement('option');
    o.value = ''; o.textContent = s.timezone ? 'Other: ' + s.timezone : 'Not set';
    tz.prepend(o);
    tz.value = '';
  } else {
    tz.value = s.timezone;
  }
  const l = s.location;
  radio('locmode', l.manual ? 'manual' : 'auto');
  if (l.known) { $('lat').value = l.lat.toFixed(4); $('lon').value = l.lon.toFixed(4); }
  $('locname').value = l.name;
  showLocFields();
}

$('bright').addEventListener('input', () => { $('bright_v').textContent = $('bright').value + ' %'; });
document.querySelectorAll('input[name="locmode"]').forEach(r => r.addEventListener('change', showLocFields));

$('save_settings').addEventListener('click', async () => {
  const body = { brightness: +$('bright').value, units: picked('units'), clock_24h: picked('clock') === '24' };
  if ($('tz').value) body.timezone = $('tz').value;
  try { await postJson('/api/settings', body); say('msg_settings', 'Saved', true); loadStatus(); }
  catch (e) { say('msg_settings', 'Not saved: ' + e.message, false); }
});

$('save_loc').addEventListener('click', async () => {
  const manual = picked('locmode') === 'manual';
  const body = { manual };
  if (manual) {
    body.lat = parseFloat($('lat').value); body.lon = parseFloat($('lon').value); body.name = $('locname').value.trim();
    if (!(body.lat >= -90 && body.lat <= 90 && body.lon >= -180 && body.lon <= 180)) { say('msg_loc', 'Enter a valid latitude and longitude', false); return; }
  }
  try { await postJson('/api/weather', body); say('msg_loc', 'Saved - updating weather', true); setTimeout(loadStatus, 4000); }
  catch (e) { say('msg_loc', 'Not saved: ' + e.message, false); }
});

loadSettings();
loadStatus();
setInterval(loadStatus, 10000);
</script>
</body>
</html>
)HTML";
