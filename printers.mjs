// Live link to Bambu Lab printers on the home network (LAN mode + Developer Mode).
// Each printer publishes its status over MQTT (TLS, port 8883, user "bblp", password = access code).
// The AMS report lists every loaded roll with its tray UUID — the same ID the Flipper reads from
// the tag (block 9) — and the printer's estimate of how much is left (remain, %).
import tls from "node:tls";
import mqtt from "mqtt";

const ZERO_UUID = /^0*$/;
const PUSHALL_EVERY_MS = 5 * 60 * 1000; // Bambu asks clients not to request full reports too often

/** Read the printer's serial number from its TLS certificate (no login needed). */
export function readSerial(ip) {
  return new Promise((resolve, reject) => {
    const s = tls.connect({ host: ip, port: 8883, rejectUnauthorized: false, timeout: 6000 }, () => {
      const cn = s.getPeerCertificate()?.subject?.CN;
      s.end();
      cn ? resolve(String(cn)) : reject(new Error("Printer didn't report a serial number"));
    });
    s.on("timeout", () => { s.destroy(); reject(new Error(`No printer answering at ${ip}. Check the IP and that LAN mode is on.`)); });
    s.on("error", (e) => reject(new Error(`Couldn't reach ${ip}: ${e.message}`)));
  });
}

const hexOf = (c) => (/^[0-9A-Fa-f]{6}/.test(c ?? "") ? `#${c.slice(0, 6).toLowerCase()}` : "#888888");

/** Keep a picture of each AMS (units and slots, empty ones too), merging partial reports. */
function mergeAms(units, list, status) {
  for (const u of list) {
    const id = Number(u.id);
    if (!units.has(id)) units.set(id, { trays: new Map() });
    const unit = units.get(id);
    if (u.humidity !== undefined) unit.humidity = Number(u.humidity);
    for (const t of u.tray ?? []) {
      const slot = Number(t.id);
      const bare = Object.keys(t).every((k) => k === "id");
      unit.trays.set(slot, bare ? {} : { ...(unit.trays.get(slot) ?? {}), ...t });
    }
  }
  status.ams = [...units.entries()].sort((a, b) => a[0] - b[0]).map(([id, u]) => ({
    id,
    name: id >= 128 ? `AMS HT${id > 128 ? ` ${id - 127}` : ""}` : `AMS ${id + 1}`,
    humidity: u.humidity ?? null,
    slots: [...Array(id >= 128 ? 1 : 4).keys()].map((n) => {
      const t = u.trays.get(n) ?? {};
      if (!t.tray_type) return { slot: n + 1, empty: true };
      const raw = String(t.tray_uuid ?? "").toUpperCase();
      return {
        slot: n + 1,
        empty: false,
        type: t.tray_type,
        name: t.tray_sub_brands || t.tray_type,
        hex: hexOf(t.tray_color),
        remain: Number.isInteger(t.remain) && t.remain >= 0 && t.remain <= 100 ? t.remain : null,
        uuid: /^[0-9A-F]{32}$/.test(raw) && !ZERO_UUID.test(raw) ? raw : null,
      };
    }),
  }));
}

export function createPrinterHub(db) {
  db.exec(`
    CREATE TABLE IF NOT EXISTS printers (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT NOT NULL,
      ip TEXT NOT NULL,
      serial TEXT NOT NULL,
      access_code TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
  `);
  const cols = new Set(db.prepare("PRAGMA table_info(spools)").all().map((c) => c.name));
  for (const [c, type] of [["remain_pct", "INTEGER"], ["location", "TEXT"], ["printer_id", "INTEGER"], ["last_seen", "TEXT"]])
    if (!cols.has(c)) db.exec(`ALTER TABLE spools ADD COLUMN ${c} ${type}`);

  const live = new Map(); // printer id -> { client, timer, status }

  function applyAms(printer, ams, vtTray) {
    const now = new Date().toISOString();
    const upd = db.prepare(`UPDATE spools SET remain_pct = COALESCE(?, remain_pct), location = ?, printer_id = ?, last_seen = ?, used_up = 0
      WHERE tray_uid = ?`);
    const ins = db.prepare(`INSERT OR IGNORE INTO spools
      (tray_uid, tag_uid, type, detail, hex, weight_g, diameter_mm, scanned_at, remain_pct, location, printer_id, last_seen)
      VALUES (?,?,?,?,?,?,?,?,?,?,?,?)`);

    const trays = [];
    for (const unit of ams?.ams ?? []) {
      // Units 0-3 are regular AMS units; 128+ are single-slot AMS HT units.
      const id = Number(unit.id);
      const unitName = id >= 128 ? `AMS HT${id > 128 ? ` ${id - 127}` : ""}` : `AMS ${id + 1}`;
      for (const t of unit.tray ?? [])
        trays.push({ t, where: `${printer.name} · ${unitName}${id >= 128 ? "" : ` slot ${Number(t.id) + 1}`}` });
    }
    if (vtTray) trays.push({ t: vtTray, where: `${printer.name} · external spool` });

    const clearSlot = db.prepare("UPDATE spools SET location = NULL WHERE location = ? AND tray_uid != ?");
    for (const { t, where } of trays) {
      const raw = String(t.tray_uuid ?? "").toUpperCase();
      const uuid = !ZERO_UUID.test(raw) && /^[0-9A-F]{32}$/.test(raw) ? raw : null;
      // A slot that now holds a different roll (or none, or an untagged one): the old roll is back on the shelf.
      // Slots missing from a partial report are left alone.
      // Only trays that say what's in them count: a bare {id} is an empty slot, a tray with a uuid field
      // says which roll; a partial update (just remain, say) leaves the slot's roll where it is.
      const empty = Object.keys(t).every((k) => k === "id") || t.tray_type === "";
      if (empty || "tray_uuid" in t) clearSlot.run(where, uuid ?? "");
      if (!uuid) continue;
      const remain = Number.isInteger(t.remain) && t.remain >= 0 && t.remain <= 100 ? t.remain : null;
      const r = upd.run(remain, where, printer.id, now, uuid);
      if (!r.changes && t.tray_type) {
        // A Bambu roll the Flipper hasn't seen yet: the printer can add it on its own.
        const hex = /^[0-9A-Fa-f]{6}/.test(t.tray_color ?? "") ? `#${t.tray_color.slice(0, 6).toLowerCase()}` : "#888888";
        ins.run(uuid, t.tag_uid || null, t.tray_type, t.tray_sub_brands || null, hex,
          parseInt(t.tray_weight) || null, parseFloat(t.tray_diameter) || null, now, remain, where, printer.id, now);
      }
    }
  }

  function connect(printer) {
    disconnect(printer.id);
    const status = { connected: false, error: null, state: null, job: null, progress: null, lastReport: null, ams: [], external: null };
    const units = new Map(); // AMS unit id -> { trays: Map(slot id -> last known tray fields) }
    const client = mqtt.connect(`mqtts://${printer.ip}:8883`, {
      username: "bblp",
      password: printer.access_code,
      rejectUnauthorized: false, // printers use a self-signed Bambu certificate
      clientId: `spool-inventory-${Math.random().toString(16).slice(2, 10)}`,
      reconnectPeriod: 15000,
      connectTimeout: 10000,
    });
    const reqTopic = `device/${printer.serial}/request`;
    const pushall = () => client.publish(reqTopic, JSON.stringify({ pushing: { sequence_id: "0", command: "pushall" } }));

    client.on("connect", () => {
      status.connected = true;
      status.error = null;
      client.subscribe(`device/${printer.serial}/report`, (err) => { if (!err) pushall(); });
    });
    client.on("message", (_topic, buf) => {
      let msg;
      try { msg = JSON.parse(buf.toString()); } catch { return; }
      const p = msg.print;
      if (!p) return;
      status.lastReport = new Date().toISOString();
      if (p.gcode_state) status.state = p.gcode_state;
      if (p.subtask_name !== undefined) status.job = p.subtask_name || null;
      if (p.mc_percent !== undefined) status.progress = p.mc_percent;
      if (p.ams?.ams) mergeAms(units, p.ams.ams, status);
      if (p.vt_tray) status.external = p.vt_tray.tray_type ? { type: p.vt_tray.tray_type, hex: hexOf(p.vt_tray.tray_color) } : null;
      if (p.ams?.ams || p.vt_tray) {
        try { applyAms(printer, p.ams, p.vt_tray); } catch (e) { console.error("AMS update failed:", e.message); }
      }
    });
    client.on("error", (e) => {
      status.connected = false;
      status.error = /not authorized|bad user name or password/i.test(e.message)
        ? "Access code was rejected. Check it on the printer's screen."
        : e.message;
    });
    client.on("close", () => { status.connected = false; });

    const timer = setInterval(() => client.connected && pushall(), PUSHALL_EVERY_MS);
    live.set(printer.id, { client, timer, status, pushall });
  }

  function disconnect(id) {
    const l = live.get(id);
    if (!l) return;
    clearInterval(l.timer);
    l.client.end(true);
    live.delete(id);
  }

  const all = () => db.prepare("SELECT * FROM printers ORDER BY id").all();
  for (const p of all()) connect(p);

  return {
    applyAms, // exposed for tests
    /** Ask every printer for a full report now (the Refresh button). */
    refreshAll() { for (const l of live.values()) if (l.client.connected) l.pushall(); },
    /** Printers for the UI. The access code never leaves the server. */
    list: () => all().map(({ access_code: _code, ...p }) => ({ ...p, status: live.get(p.id)?.status ?? null })),
    async add({ name, ip, accessCode }) {
      ip = String(ip ?? "").trim();
      accessCode = String(accessCode ?? "").trim();
      name = String(name ?? "").trim().slice(0, 30) || "Printer";
      if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(ip)) throw new Error("Enter the printer's IP address, like 192.168.1.92");
      if (!/^[A-Za-z0-9]{6,12}$/.test(accessCode)) throw new Error("Access code should be the 8 characters shown on the printer");
      const serial = await readSerial(ip);
      const r = db.prepare("INSERT INTO printers (name, ip, serial, access_code, created_at) VALUES (?,?,?,?,?)")
        .run(name, ip, serial, accessCode, new Date().toISOString());
      connect(db.prepare("SELECT * FROM printers WHERE id = ?").get(r.lastInsertRowid));
    },
    /** Rolls the printer can't see (A1 external spool, non-AMS): mark them loaded or back on the shelf by hand. */
    loadByHand(trayUid, printerId) {
      if (printerId == null) {
        db.prepare("UPDATE spools SET location = NULL, printer_id = NULL WHERE tray_uid = ?").run(trayUid);
        return;
      }
      const p = db.prepare("SELECT * FROM printers WHERE id = ?").get(printerId);
      if (!p) throw new Error("No such printer");
      db.prepare("UPDATE spools SET location = ?, printer_id = ?, used_up = 0 WHERE tray_uid = ?")
        .run(`${p.name} · loaded by hand`, p.id, trayUid);
    },
    remove(id) {
      disconnect(id);
      db.prepare("UPDATE spools SET location = NULL, printer_id = NULL WHERE printer_id = ?").run(id);
      db.prepare("DELETE FROM printers WHERE id = ?").run(id);
    },
  };
}
