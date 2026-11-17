// Spool Inventory by Noventia.studio — local filament inventory fed by the Flipper Zero "Spool Scanner" app.
// Node 22.5+ (node:sqlite); one npm dependency (mqtt) for the printer link.
// The web page and its API answer only on this computer (localhost). The round display's
// read-only feed (/api/device) is the one thing reachable from the rest of the network.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { DatabaseSync } from "node:sqlite";
import { fileURLToPath } from "node:url";
import { createPrinterHub } from "./printers.mjs";
import { deviceFeed, classify } from "./device-feed.mjs";
import os from "node:os";

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT) || 3100;
const DATA = path.join(ROOT, "data");
fs.mkdirSync(DATA, { recursive: true });

const db = new DatabaseSync(path.join(DATA, "inventory.db"));
db.exec(`
  PRAGMA journal_mode = WAL;
  CREATE TABLE IF NOT EXISTS spools (
    tray_uid TEXT PRIMARY KEY,         -- unique per roll (from the tag), or MANUAL-xxxx
    tag_uid TEXT,
    brand TEXT NOT NULL DEFAULT 'Bambu Lab',
    material_id TEXT,
    variant_id TEXT,
    type TEXT NOT NULL,
    detail TEXT,
    hex TEXT NOT NULL,
    hex2 TEXT,
    weight_g INTEGER,
    diameter_mm REAL,
    length_m INTEGER,
    produced TEXT,
    scanned_at TEXT NOT NULL,
    used_up INTEGER NOT NULL DEFAULT 0
  );
`);

const printers = createPrinterHub(db);
let displaySeen = null; // last time the round display fetched its feed

const list = () => db.prepare("SELECT * FROM spools ORDER BY used_up, type, detail, hex, scanned_at").all().map((s) => ({ ...s, ...classify(s) }));

function importCsv(csv) {
  const lines = csv.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
  const header = (lines.shift() ?? "").split(",");
  const col = (n) => header.indexOf(n);
  if (col("tray_uid") < 0 || col("color_rgba") < 0) throw new Error("That doesn't look like a Spool Scanner file.");
  const ins = db.prepare(`INSERT OR IGNORE INTO spools
    (tray_uid, tag_uid, material_id, variant_id, type, detail, hex, hex2, weight_g, diameter_mm, length_m, produced, scanned_at)
    VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`);
  let added = 0, known = 0, skipped = 0;
  db.exec("BEGIN");
  try {
    for (const line of lines) {
      const f = line.split(",");
      const get = (n) => (f[col(n)] ?? "").trim();
      const tray = get("tray_uid").toUpperCase();
      const rgba = get("color_rgba"), rgba2 = get("color2_rgba");
      if (!/^[0-9A-F]{32}$/.test(tray) || !/^[0-9A-Fa-f]{8}$/.test(rgba) || !get("type")) { skipped++; continue; }
      const multi = Number(get("color_count")) > 1 && /^[0-9A-Fa-f]{8}$/.test(rgba2);
      const ts = Number(get("scanned_unix"));
      const num = (s) => (s === "" || isNaN(Number(s)) ? null : Number(s));
      const r = ins.run(
        tray, get("tag_uid"), get("material_id") || null, get("variant_id") || null, get("type"), get("detail") || null,
        `#${rgba.slice(0, 6).toLowerCase()}`, multi ? `#${rgba2.slice(0, 6).toLowerCase()}` : null,
        num(get("weight_g")), num(get("diameter_mm")) || null, num(get("length_m")) || null, get("produced") || null,
        // The Flipper's clock may never have been set; fall back to "now".
        new Date(ts > 1_600_000_000 ? ts * 1000 : Date.now()).toISOString(),
      );
      r.changes ? added++ : known++;
    }
    db.exec("COMMIT");
  } catch (e) {
    db.exec("ROLLBACK");
    throw e;
  }
  return { added, known, skipped };
}

function addManual(b) {
  const type = String(b.type ?? "").trim().slice(0, 30);
  const hex = String(b.hex ?? "");
  const count = Math.min(Math.max(parseInt(b.count) || 1, 1), 50);
  if (!type || !/^#[0-9a-f]{6}$/i.test(hex)) throw new Error("Need a material and a color.");
  const ins = db.prepare(`INSERT INTO spools (tray_uid, brand, type, detail, hex, weight_g, diameter_mm, scanned_at)
    VALUES (?,?,?,?,?,?,?,?)`);
  for (let i = 0; i < count; i++)
    ins.run(`MANUAL-${crypto.randomUUID()}`, String(b.brand || "Other").slice(0, 30), type,
      String(b.detail || "").slice(0, 40) || null, hex.toLowerCase(), parseInt(b.weight_g) || null, 1.75, new Date().toISOString());
  return count;
}

const MIME = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css", ".png": "image/png", ".svg": "image/svg+xml" };

async function body(req) {
  let s = "";
  for await (const c of req) { s += c; if (s.length > 3_000_000) throw new Error("Too big"); }
  return JSON.parse(s || "{}");
}

const send = (res, code, obj) => { res.writeHead(code, { "Content-Type": "application/json" }); res.end(JSON.stringify(obj)); };

http.createServer(async (req, res) => {
  const url = new URL(req.url, "http://x");
  // The round display on the home network may only read its feed. Everything else is this PC only.
  const local = ["127.0.0.1", "::1", "::ffff:127.0.0.1"].includes(req.socket.remoteAddress);
  try {
    if (url.pathname === "/api/device" && req.method === "GET") {
      displaySeen = { at: Date.now(), ip: String(req.socket.remoteAddress).replace(/^::ffff:/, "") };
      return send(res, 200, deviceFeed(list(), printers.list()));
    }
    if (!local) { res.writeHead(403); return res.end("Forbidden"); }
    if (url.pathname === "/api/server-info" && req.method === "GET") {
      const ips = Object.values(os.networkInterfaces()).flat()
        .filter((i) => i && i.family === "IPv4" && !i.internal && !/^169\.254\./.test(i.address)).map((i) => i.address);
      // The display checks in every 15 s; treat it as connected if it did so in the last minute.
      const display = displaySeen && Date.now() - displaySeen.at < 60000 ? displaySeen : null;
      return send(res, 200, { ips, port: PORT, display });
    }
    if (url.pathname === "/api/spools" && req.method === "GET") return send(res, 200, { spools: list() });
    if (url.pathname === "/api/spools/import" && req.method === "POST") {
      const { csv } = await body(req);
      if (typeof csv !== "string") return send(res, 400, { error: "Missing spool list" });
      return send(res, 200, { ...importCsv(csv), spools: list() });
    }
    if (url.pathname === "/api/spools/manual" && req.method === "POST") {
      const added = addManual(await body(req));
      return send(res, 200, { added, spools: list() });
    }
    const m = url.pathname.match(/^\/api\/spools\/([A-Za-z0-9-]+)$/);
    if (m && req.method === "PATCH") {
      const b = await body(req);
      if ("loadOn" in b) printers.loadByHand(m[1], b.loadOn == null ? null : Number(b.loadOn));
      // A used-up roll is off the printer too.
      if ("usedUp" in b) db.prepare(`UPDATE spools SET used_up = ?${b.usedUp ? ", location = NULL, printer_id = NULL" : ""} WHERE tray_uid = ?`).run(b.usedUp ? 1 : 0, m[1]);
      return send(res, 200, { spools: list() });
    }
    if (m && req.method === "DELETE") {
      db.prepare("DELETE FROM spools WHERE tray_uid = ?").run(m[1]);
      return send(res, 200, { spools: list() });
    }
    if (url.pathname === "/api/printers/refresh" && req.method === "POST") {
      printers.refreshAll();
      return send(res, 200, { ok: true });
    }
    if (url.pathname === "/api/printers" && req.method === "GET") return send(res, 200, { printers: printers.list() });
    if (url.pathname === "/api/printers" && req.method === "POST") {
      await printers.add(await body(req));
      return send(res, 200, { printers: printers.list() });
    }
    const pm = url.pathname.match(/^\/api\/printers\/(\d+)$/);
    if (pm && req.method === "DELETE") {
      printers.remove(Number(pm[1]));
      return send(res, 200, { printers: printers.list() });
    }
    if (url.pathname.startsWith("/api/")) return send(res, 404, { error: "Not found" });

    // Static files
    const file = path.join(ROOT, "public", url.pathname === "/" ? "index.html" : path.normalize(url.pathname));
    if (!file.startsWith(path.join(ROOT, "public")) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      res.writeHead(404); return res.end("Not found");
    }
    res.writeHead(200, { "Content-Type": MIME[path.extname(file)] ?? "application/octet-stream", "Cache-Control": "no-cache" });
    fs.createReadStream(file).pipe(res);
  } catch (e) {
    send(res, 400, { error: e.message });
  }
}).listen(PORT, "0.0.0.0", () => console.log(`Spool Inventory by Noventia.studio: http://localhost:${PORT} (display feed open on the LAN, read-only)`));
