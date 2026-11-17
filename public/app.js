// Spool Inventory by Noventia.studio, front end. USB sync uses Web Serial (Chrome / Edge) to read the
// Flipper's saved list through its built-in command line.
const SPOOL_FILE = "/ext/apps_data/spool_scanner/spools.csv";
const $ = (id) => document.getElementById(id);
let spools = [];
let printerData = [];
let lastLoadedHtml = null;
const finishFilter = {}; // material -> finish shown (null = all)

const NAMES = [
  ["Black", 16, 16, 16], ["White", 245, 245, 245], ["Gray", 128, 128, 128], ["Silver", 192, 192, 192],
  ["Red", 208, 16, 32], ["Dark Red", 128, 16, 16], ["Orange", 255, 112, 16], ["Yellow", 248, 224, 32],
  ["Gold", 200, 160, 64], ["Green", 32, 160, 64], ["Dark Green", 16, 80, 32], ["Lime", 160, 224, 48],
  ["Teal", 0, 128, 128], ["Cyan", 48, 208, 224], ["Blue", 16, 80, 208], ["Navy", 16, 32, 96],
  ["Purple", 112, 48, 160], ["Pink", 240, 144, 192], ["Magenta", 224, 32, 160], ["Brown", 112, 64, 32],
  ["Beige", 232, 216, 176],
];
function colorName(hex) {
  const n = parseInt(hex.slice(1), 16), r = n >> 16, g = (n >> 8) & 255, b = n & 255;
  let best = "", bd = Infinity;
  for (const [name, R, G, B] of NAMES) {
    const d = 3 * (r - R) ** 2 + 4 * (g - G) ** 2 + 2 * (b - B) ** 2;
    if (d < bd) { bd = d; best = name; }
  }
  return best;
}
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

// Grams left: printer's estimate if it has reported this roll, else the starting weight.
const gramsLeft = (s) => (s.weight_g || 0) * (s.remain_pct ?? 100) / 100;

function msg(text, bad = false) { $("msg").textContent = text; $("msg").className = bad ? "bad" : ""; }

async function api(path, opts = {}) {
  const res = await fetch(path, { headers: { "Content-Type": "application/json" }, ...opts });
  const j = await res.json();
  if (!res.ok) throw new Error(j.error || "Something went wrong");
  if (j.spools) { spools = j.spools; render(); }
  return j;
}

function render() {
  const showUsed = $("showUsed").checked;
  const onHand = spools.filter((s) => !s.used_up);
  $("sRolls").textContent = onHand.length;
  $("sKg").textContent = `${(onHand.reduce((t, s) => t + gramsLeft(s), 0) / 1000).toFixed(1)} kg`;
  $("sTypes").textContent = new Set(onHand.map((s) => s.family)).size;
  const loadedN = onHand.filter((s) => s.location).length;
  $("sLoaded").textContent = loadedN ? `${onHand.length - loadedN} on the shelf · ${loadedN} in printers` : "";
  renderLoaded();

  // Color groups (same product + color), then sorted into family -> finish sections.
  const groups = new Map();
  for (const s of spools) {
    if (s.used_up && !showUsed) continue;
    if (s.location && !s.used_up) continue; // checked out: it's in a printer (shown above)
    const brand = s.brand && s.brand !== "Bambu Lab" ? `${s.brand} ` : "";
    const key = `${s.family}|${s.finish}|${brand}|${s.hex}|${s.hex2 || ""}`;
    if (!groups.has(key))
      groups.set(key, { label: `${brand}${colorName(s.hex)}${s.hex2 ? ` / ${colorName(s.hex2)}` : ""}`, s, rolls: [] });
    groups.get(key).rolls.push(s);
  }
  // Most first: materials by how many rolls you have, then finishes within each material the same way.
  const famN = {}, finN = {};
  for (const g of groups.values()) {
    const n = g.rolls.filter((r) => !r.used_up).length || 0.001; // used-up-only groups sort last
    famN[g.s.family] = (famN[g.s.family] || 0) + n;
    const fk = `${g.s.family}|${g.s.finish}`;
    finN[fk] = (finN[fk] || 0) + n;
  }
  const sorted = [...groups.values()].sort((a, b) =>
    famN[b.s.family] - famN[a.s.family] || a.s.family.localeCompare(b.s.family)
    || finN[`${b.s.family}|${b.s.finish}`] - finN[`${a.s.family}|${a.s.finish}`] || a.s.finish.localeCompare(b.s.finish)
    || b.rolls.length - a.rolls.length || a.label.localeCompare(b.label));
  $("empty").hidden = spools.length > 0;
  $("shelfTitle").hidden = !sorted.length || !loadedN;
  const open = new Set([...document.querySelectorAll("#groups details[open]")].map((d) => d.dataset.key));
  const count = (gs) => gs.reduce((t, g) => t + g.rolls.filter((r) => !r.used_up).length, 0);
  const kg = (gs) => (gs.reduce((t, g) => t + g.rolls.filter((r) => !r.used_up).reduce((u, r) => u + gramsLeft(r), 0), 0) / 1000).toFixed(1);

  // One continuous 3-across grid per material; each card carries its finish label.
  let html = "", fam = null;
  for (let i = 0; i < sorted.length; i++) {
    const g = sorted[i];
    if (g.s.family !== fam) {
      if (fam !== null) html += `</div>`;
      fam = g.s.family;
      const inFam = sorted.filter((x) => x.s.family === fam);
      const finishes = [...new Set(inFam.map((x) => x.s.finish))];
      const pick = finishFilter[fam];
      const chip = (label, value, n) =>
        `<button class="chip${(pick ?? "") === value ? " on" : ""}" data-fam="${esc(fam)}" data-finish="${esc(value)}">${esc(label)}${n != null ? ` ${n}` : ""}</button>`;
      const chips = (finishes.length > 1 ? chip("All", "", null) : "")
        + finishes.map((f) => chip(f, f, count(inFam.filter((x) => x.s.finish === f)))).join("");
      html += `<h2 class="fam"><span>${esc(fam)}</span><small>${count(inFam)} rolls · ${kg(inFam)} kg</small><span class="chips">${chips}</span></h2><div class="grid3">`;
    }
    // A finish chip that's switched on shows only that finish in this material.
    if (finishFilter[fam] && g.s.finish !== finishFilter[fam]) continue;
    html += card(g);
  }
  if (sorted.length) html += `</div>`;
  $("groups").innerHTML = html;

  function card(g) {
    const on = g.rolls.filter((r) => !r.used_up).length;
    const bg = g.s.hex2 ? `linear-gradient(135deg, ${g.s.hex} 50%, ${g.s.hex2} 50%)` : g.s.hex;
    const key = `${g.s.family}|${g.s.finish}|${g.label}|${g.s.hex}`;
    return `<details class="card" data-key="${esc(key)}" ${open.has(key) ? "open" : ""}>
      <summary><span class="dot" style="background:${bg}"></span><span class="names"><small>${esc(g.s.finish)}</small><b>${esc(g.label)}</b></span><span class="hex">${g.s.hex}</span>
        <span class="count">${on}</span><span class="unit">roll${on === 1 ? "" : "s"}</span></summary>
      <ul>${g.rolls.map((r) => `<li class="${r.used_up ? "used" : ""}">
        <span class="mono">${r.tray_uid.startsWith("MANUAL") ? "by hand" : esc(r.tray_uid.slice(0, 8))}</span>
        ${r.remain_pct != null
          ? `<span class="bar-left" title="${r.remain_pct}% left (printer estimate)"><i style="width:${r.remain_pct}%"></i></span><span>~${Math.round(gramsLeft(r))} g left</span>`
          : `<span>${r.weight_g ?? "?"} g</span>`}
        ${r.location ? `<span class="tag">${esc(r.location)}</span>` : ""}
        ${r.produced ? `<span>made ${esc(r.produced.split("_").slice(0, 3).join("-"))}</span>` : ""}
        <span class="actions">
          <button class="btn ghost small" data-act="used" data-id="${esc(r.tray_uid)}" data-v="${r.used_up ? 0 : 1}">${r.used_up ? "Back in stock" : "Used up"}</button>
          <button class="btn ghost small" data-act="del" data-id="${esc(r.tray_uid)}">Delete</button>
        </span></li>`).join("")}</ul></details>`;
  }
}

$("groups").addEventListener("click", async (e) => {
  const chip = e.target.closest("button.chip");
  if (chip) { // finish filter: click to show only that finish, click again (or All) to show everything
    const { fam, finish } = chip.dataset;
    finishFilter[fam] = !finish || finishFilter[fam] === finish ? null : finish;
    render();
    return;
  }
  const b = e.target.closest("button[data-act]");
  if (!b) return;
  if (b.dataset.act === "used") await api(`/api/spools/${b.dataset.id}`, { method: "PATCH", body: JSON.stringify({ usedUp: b.dataset.v === "1" }) });
  if (b.dataset.act === "del" && confirm("Remove this roll from the inventory?")) await api(`/api/spools/${b.dataset.id}`, { method: "DELETE" });
});

async function importCsv(csv) {
  if (csv === null) return msg("No spools on the Flipper yet. Open Apps → NFC → Spool Scanner and tap some rolls.");
  const j = await api("/api/spools/import", { method: "POST", body: JSON.stringify({ csv }) });
  msg(`Synced: ${j.added} new roll${j.added === 1 ? "" : "s"}, ${j.known} already here${j.skipped ? `, ${j.skipped} unreadable lines skipped` : ""}.`);
}

async function busy(btn, task) {
  btn.disabled = true; msg("");
  try { await task(); } catch (e) { msg(e.message, true); }
  btn.disabled = false;
}

// ---- Flipper over USB (Web Serial) ----
async function readFlipperFile(path) {
  if (!("serial" in navigator)) throw new Error("USB sync needs Chrome or Edge. You can import the file by hand instead.");
  let port;
  try { port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x0483, usbProductId: 0x5740 }] }); }
  catch { throw new Error("No Flipper picked. Plug it in, then choose it in the pop-up."); }
  try { await port.open({ baudRate: 230400 }); }
  catch { throw new Error("Couldn't open the Flipper. Close qFlipper (or anything else using it), unplug and replug it, then try again."); }

  const reader = port.readable.getReader(), writer = port.writable.getWriter();
  const enc = new TextEncoder(), dec = new TextDecoder();
  let buf = "";
  const pump = (async () => {
    try { for (;;) { const { value, done } = await reader.read(); if (done) break; buf += dec.decode(value, { stream: true }); } } catch {}
  })();
  const waitFor = async (check, ms, what) => {
    const end = Date.now() + ms;
    while (!check()) {
      if (Date.now() > end) throw new Error(`The Flipper didn't respond (${what}). Unlock it and try again.`);
      await new Promise((r) => setTimeout(r, 40));
    }
  };
  try {
    await writer.write(enc.encode("\r\n"));
    await waitFor(() => buf.includes(">: "), 4000, "no prompt");
    buf = "";
    await writer.write(enc.encode(`storage read ${path}\r\n`));
    let result = null;
    await waitFor(() => {
      if (/Storage error|not exist/i.test(buf)) return true;
      const m = buf.match(/Size: (\d+)\r?\n/);
      if (!m) return false;
      const start = m.index + m[0].length, size = Number(m[1]);
      if (buf.length - start < size) return false;
      result = buf.slice(start, start + size);
      return true;
    }, 30000, "reading the spool list");
    if (result === null && !/not exist/i.test(buf)) throw new Error(buf.trim().split("\n").pop() || "Read failed");
    return result;
  } finally {
    await reader.cancel().catch(() => {});
    await pump;
    reader.releaseLock(); writer.releaseLock();
    await port.close().catch(() => {});
  }
}

$("sync").addEventListener("click", (e) => busy(e.target, async () => importCsv(await readFlipperFile(SPOOL_FILE))));
$("file").addEventListener("change", (e) => {
  const f = e.target.files[0]; e.target.value = "";
  if (f) busy($("sync"), async () => importCsv(await f.text()));
});
$("showUsed").addEventListener("change", render);
$("addToggle").addEventListener("click", () => { $("addForm").hidden = !$("addForm").hidden; });
$("addForm").addEventListener("submit", (e) => {
  e.preventDefault();
  const data = Object.fromEntries(new FormData(e.target));
  busy(e.submitter, async () => {
    const j = await api("/api/spools/manual", { method: "POST", body: JSON.stringify(data) });
    msg(`Added ${j.added} roll${j.added === 1 ? "" : "s"}.`);
  });
});
if (!("serial" in navigator)) msg("USB sync needs Chrome or Edge. You can still import the file or add rolls by hand.");

// ---- Printers ----
const STATE = { RUNNING: "Printing", PAUSE: "Paused", FINISH: "Finished", FAILED: "Failed", IDLE: "Idle", PREPARE: "Preparing" };
function renderPrinters(list) {
  $("printerList").innerHTML = list.map((p) => {
    const st = p.status || {};
    const led = st.connected ? "on" : st.error ? "bad" : "";
    let what = st.connected ? (STATE[st.state] || st.state || "Connected") : st.error ? esc(st.error) : "Connecting…";
    if (st.connected && st.state === "RUNNING" && st.job) what += ` · ${esc(st.job)} ${st.progress ?? 0}%`;
    return `<div class="printer"><span class="led ${led}"></span><b>${esc(p.name)}</b><span>${esc(p.ip)}</span>
      <span class="mono">${esc(p.serial)}</span><span>${what}</span>
      <button class="btn ghost small grow" data-printer="${p.id}">Remove</button></div>`;
  }).join("");
}
async function loadPrinters() {
  const j = await fetch("/api/printers").then((r) => r.json());
  printerData = j.printers;
  renderPrinters(j.printers);
  renderLoaded();
}

// ---- Loaded: what's in each printer right now ----
// Rolls in an AMS are checked out automatically (the printer reports them); pull one out and it's
// back on the shelf. Printers without an AMS (the A1's external spool) are loaded by hand.
function spoolGraphic(hex, pct, empty = false) {
  if (empty) return `<span class="spool empty"></span>`;
  const p = pct == null ? 100 : pct;
  return `<span class="spool" style="--c:${hex};--p:${p}"><i></i></span>`;
}
function rollLabel(s) {
  return `${s.family} ${s.finish === "Basic" ? "" : s.finish}`.trim();
}
function renderLoaded() {
  const byUuid = new Map(spools.map((s) => [s.tray_uid, s]));
  const shelf = spools.filter((s) => !s.used_up && !s.location)
    .map((s) => ({ s, label: `${rollLabel(s)} · ${colorName(s.hex)}${s.hex2 ? ` / ${colorName(s.hex2)}` : ""}` }))
    .sort((a, b) => a.label.localeCompare(b.label));
  const html = printerData.map((p) => {
    const st = p.status || {};
    const led = st.connected ? "on" : st.error ? "bad" : "";
    let state = st.connected ? (STATE[st.state] || st.state || "Connected") : "Offline";
    if (st.connected && st.state === "RUNNING" && st.job) state += ` · ${esc(st.job)} ${st.progress ?? 0}%`;

    const units = (st.ams || []).map((u) => `<div class="ams">
      <div class="ams-name">${esc(u.name)}${u.humidity != null ? `<small>humidity ${u.humidity}</small>` : ""}</div>
      <div class="ams-slots">${u.slots.map((t) => {
        if (t.empty) return `<div class="slot">${spoolGraphic(null, null, true)}<b class="muted">Empty</b><small>slot ${t.slot}</small></div>`;
        const s = t.uuid && byUuid.get(t.uuid);
        const name = s ? rollLabel(s) : t.name;
        const color = colorName(s ? s.hex : t.hex);
        const pct = t.remain ?? s?.remain_pct ?? null;
        const g = s?.weight_g && pct != null ? ` · ~${Math.round(s.weight_g * pct / 100)} g` : "";
        return `<div class="slot" title="${esc(t.name)}, slot ${t.slot}">${spoolGraphic(s ? s.hex : t.hex, pct)}
          <b>${esc(name)}</b><small>${esc(color)}${pct != null ? ` · ${pct}%` : ""}${g}</small>
          ${s ? "" : `<small class="warn">not scanned yet</small>`}</div>`;
      }).join("")}</div></div>`).join("");

    const byHand = spools.filter((s) => s.printer_id === p.id && s.location && / by hand$/.test(s.location) && !s.used_up);
    const hand = byHand.map((s) => `<div class="slot">${spoolGraphic(s.hex, s.remain_pct)}
      <b>${esc(rollLabel(s))}</b><small>${esc(colorName(s.hex))}${s.weight_g ? ` · ${s.weight_g} g` : ""}</small>
      <button class="btn ghost small" data-unload="${esc(s.tray_uid)}">Back to shelf</button></div>`).join("");
    const ext = !units && st.external
      ? `<p class="muted small-note">The printer's external spool is set to ${esc(st.external.type)}.</p>` : "";
    const picker = shelf.length ? `<form class="loadform" data-printer="${p.id}">
        <select name="roll">${shelf.map(({ s, label }) => `<option value="${esc(s.tray_uid)}">${esc(label)}</option>`).join("")}</select>
        <button class="btn ghost small">Load on ${esc(p.name)}</button></form>` : "";
    const handBlock = `<div class="ams hand"><div class="ams-name">${units ? "Loaded by hand" : "External spool"}<small>you check these in and out</small></div>
      <div class="ams-slots">${hand || `<p class="muted small-note">Nothing loaded. Pick the roll you put on and press Load.</p>`}</div>${ext}${picker}</div>`;
    const tuckHand = units && !hand; // AMS printers: hide the by-hand loader unless it's in use

    return `<div class="card rig"><div class="rig-head"><span class="led ${led}"></span><b>${esc(p.name)}</b><span class="muted">${state}</span></div>
      <div class="rig-body">${units}${tuckHand ? "" : handBlock}</div>
      ${tuckHand ? `<details class="more"><summary>Load a roll by hand</summary>${handBlock}</details>` : ""}</div>`;
  }).join("");
  // Don't rebuild while a dropdown is open or focused.
  if (document.activeElement && document.activeElement.closest && document.activeElement.closest("#loaded select")) return;
  if (html === lastLoadedHtml) return; // nothing changed: leave the page alone (keeps clicks and open menus)
  lastLoadedHtml = html;
  const openMore = new Set([...document.querySelectorAll("#loaded .rig")].map((r, i) => (r.querySelector("details.more[open]") ? i : -1)));
  $("loaded").innerHTML = html ? `<h2 class="fam"><span>Loaded</span><small>in the printers now · updates on its own</small><button class="btn ghost small refresh" id="refreshAms">Refresh</button></h2><div class="rigs">${html}</div>` : "";
  document.querySelectorAll("#loaded .rig").forEach((r, i) => { if (openMore.has(i)) r.querySelector("details.more")?.setAttribute("open", ""); });
}
$("loaded").addEventListener("click", async (e) => {
  if (e.target.id === "refreshAms") {
    const btn = e.target;
    btn.disabled = true; btn.textContent = "Checking...";
    await fetch("/api/printers/refresh", { method: "POST" }).catch(() => {});
    await new Promise((r) => setTimeout(r, 2500)); // give the printers a moment to answer
    await refresh();
    btn.disabled = false; btn.textContent = "Refresh";
    msg("Checked the printers.");
    return;
  }
  const b = e.target.closest("button[data-unload]");
  if (b) await api(`/api/spools/${b.dataset.unload}`, { method: "PATCH", body: JSON.stringify({ loadOn: null }) }).catch((x) => msg(x.message, true));
});
$("loaded").addEventListener("submit", async (e) => {
  e.preventDefault();
  const roll = new FormData(e.target).get("roll");
  if (roll) await api(`/api/spools/${roll}`, { method: "PATCH", body: JSON.stringify({ loadOn: Number(e.target.dataset.printer) }) }).catch((x) => msg(x.message, true));
});
$("printerToggle").addEventListener("click", () => { $("printerForm").hidden = !$("printerForm").hidden; });
$("printerForm").addEventListener("submit", (e) => {
  e.preventDefault();
  const form = e.target;
  busy(e.submitter, async () => {
    const res = await fetch("/api/printers", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(Object.fromEntries(new FormData(form))) });
    const j = await res.json();
    if (!res.ok) throw new Error(j.error);
    form.reset(); form.hidden = true;
    renderPrinters(j.printers);
    msg("Printer added. Loaded rolls will show up within a few seconds.");
  });
});
$("printerList").addEventListener("click", async (e) => {
  const b = e.target.closest("button[data-printer]");
  if (!b || !confirm("Disconnect this printer?")) return;
  const j = await fetch(`/api/printers/${b.dataset.printer}`, { method: "DELETE" }).then((r) => r.json());
  renderPrinters(j.printers);
});

// ---- Round display setup (USB) ----
// One USB session: open the display, ask it to scan Wi-Fi, then send the chosen network.
let disp = null; // { port, reader, writer, buf, pump }
async function displayOpen() {
  if (!("serial" in navigator)) throw new Error("Needs Chrome or Edge.");
  let port;
  try { port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] }); }
  catch { throw new Error("No display picked. Plug it into this PC, then choose it in the pop-up."); }
  try { await port.open({ baudRate: 115200 }); }
  catch { throw new Error("Couldn't open the display. Close anything else using it and try again."); }
  const d = { port, reader: port.readable.getReader(), writer: port.writable.getWriter(), buf: "" };
  const dec = new TextDecoder();
  d.pump = (async () => { try { for (;;) { const { value, done } = await d.reader.read(); if (done) break; d.buf += dec.decode(value, { stream: true }); } } catch {} })();
  disp = d;
}
async function displayClose() {
  if (!disp) return;
  const d = disp; disp = null;
  await d.reader.cancel().catch(() => {}); await d.pump;
  d.reader.releaseLock(); d.writer.releaseLock(); await d.port.close().catch(() => {});
}
// Send a command line and wait for a reply line starting with `want` (the display may reboot on connect, so resend).
async function displayAsk(cmd, want, tries = 16, every = 700) {
  const line = new TextEncoder().encode(cmd + "\n");
  for (let i = 0; i < tries; i++) {
    disp.buf = "";
    await disp.writer.write(line);
    const end = Date.now() + every;
    while (Date.now() < end) {
      const at = disp.buf.indexOf(want);
      if (at >= 0) {
        const nl = disp.buf.indexOf("\n", at);
        if (nl >= 0) return disp.buf.slice(at + want.length, nl).trim();
      }
      await new Promise((r) => setTimeout(r, 50));
    }
  }
  return null;
}

$("displayToggle").addEventListener("click", (e) => busy(e.target, async () => {
  if (!$("displayForm").hidden) { $("displayForm").hidden = true; await displayClose(); return; }
  await displayClose();
  await displayOpen();
  msg("Scanning for Wi-Fi from the display...");
  const reply = await displayAsk("SCAN", "NETS ", 10, 4000); // a scan takes a few seconds
  if (reply === null) { await displayClose(); throw new Error("The display didn't answer. Make sure the Spool Display firmware is on it."); }
  const nets = JSON.parse(reply).sort((a, b) => b.rssi - a.rssi);
  if (!nets.length) { await displayClose(); throw new Error("The display didn't find any Wi-Fi networks."); }
  const bars = (r) => (r > -55 ? "▂▄▆█" : r > -67 ? "▂▄▆" : r > -78 ? "▂▄" : "▂");
  $("ssidSelect").innerHTML = nets.map((n) => `<option value="${esc(n.ssid)}">${esc(n.ssid)}  ${bars(n.rssi)}${n.open ? "  (open)" : ""}</option>`).join("");
  const info = await fetch("/api/server-info").then((r) => r.json());
  $("serverAddr").innerHTML = info.ips.map((ip) => `<option value="${ip}:${info.port}">${ip}:${info.port}</option>`).join("");
  $("displayForm").hidden = false;
  msg(`Found ${nets.length} network${nets.length === 1 ? "" : "s"}. Pick yours and enter the password.`);
}));

$("displayForm").addEventListener("submit", (e) => {
  e.preventDefault();
  const data = Object.fromEntries(new FormData(e.target));
  busy(e.submitter, async () => {
    if (!disp) await displayOpen();
    const ok = await displayAsk(`SETUP ${JSON.stringify(data)}`, "SETUP OK", 16, 600);
    await displayClose();
    if (ok === null) throw new Error("The display didn't confirm. Try Set up over USB again.");
    e.target.reset(); e.target.hidden = true;
    msg("Display set up. It's restarting and joining your Wi-Fi.");
    forceDisplaySetup = false;
  });
});

// Hide the setup box once the display is checking in; keep a small link to redo it.
let forceDisplaySetup = false;
async function loadDisplayStatus() {
  const info = await fetch("/api/server-info").then((r) => r.json());
  const connected = !!info.display && !forceDisplaySetup;
  $("displayCard").hidden = connected;
  $("displayFoot").hidden = !connected;
  if (info.display) $("displayIp").textContent = `(${info.display.ip})`;
}
$("displayRedo").addEventListener("click", (e) => {
  e.preventDefault();
  forceDisplaySetup = true;
  loadDisplayStatus();
  $("displayCard").scrollIntoView({ behavior: "smooth" });
});

// Keep the page live: printers update rolls in the background.
async function refresh() {
  await Promise.all([api("/api/spools"), loadPrinters(), loadDisplayStatus()]).catch((e) => msg(e.message, true));
}
refresh();
setInterval(refresh, 5000);
