// Material grouping shared by the web page and the round display, plus the display's
// compact, read-only feed (the only thing reachable from the network).
const NAMES = [
  ["Black", 16, 16, 16], ["White", 245, 245, 245], ["Gray", 128, 128, 128], ["Silver", 192, 192, 192],
  ["Red", 208, 16, 32], ["Dark Red", 128, 16, 16], ["Orange", 255, 112, 16], ["Yellow", 248, 224, 32],
  ["Gold", 200, 160, 64], ["Green", 32, 160, 64], ["Dark Green", 16, 80, 32], ["Lime", 160, 224, 48],
  ["Teal", 0, 128, 128], ["Cyan", 48, 208, 224], ["Blue", 16, 80, 208], ["Navy", 16, 32, 96],
  ["Purple", 112, 48, 160], ["Pink", 240, 144, 192], ["Magenta", 224, 32, 160], ["Brown", 112, 64, 32],
  ["Beige", 232, 216, 176],
];
export function colorName(hex) {
  const n = parseInt(String(hex).slice(1), 16), r = n >> 16, g = (n >> 8) & 255, b = n & 255;
  let best = "", bd = Infinity;
  for (const [name, R, G, B] of NAMES) {
    const d = 3 * (r - R) ** 2 + 4 * (g - G) ** 2 + 2 * (b - B) ** 2;
    if (d < bd) { bd = d; best = name; }
  }
  return best;
}

// Material family + finish, e.g. "PETG" / "Translucent", "Support" / "for PLA".
export const FAMILY_ORDER = ["PLA", "PETG", "TPU", "ABS / ASA", "PC", "Nylon", "Support", "Other"];
const MATERIAL_PREFIX = /^(PLA|PETG|TPU|ABS|ASA|PC|PA[0-9]*)(?![A-Z])/i;
const HAS_CF = /CF(?![A-Z])/i;

export function classify(s) {
  const type = String(s.type || "").toUpperCase();
  const detail = String(s.detail || "").trim();
  let family;
  if (/SUPPORT|PVA|BVOH/.test(type) || /^support/i.test(detail) || /-S$/.test(type)) family = "Support";
  else if (type.startsWith("PLA")) family = "PLA";
  else if (type.startsWith("PETG") || type === "PET") family = "PETG";
  else if (type.startsWith("TPU")) family = "TPU";
  else if (type.startsWith("ABS") || type.startsWith("ASA")) family = "ABS / ASA";
  else if (type.startsWith("PC")) family = "PC";
  else if (type.startsWith("PA")) family = "Nylon";
  else family = "Other";

  let finish;
  if (family === "Support") {
    finish = detail.replace(/^support\s*/i, "") || "Support";
  } else {
    // Strip the material from the product name: "PETG Translucent" -> "Translucent".
    let rest = detail;
    if (rest.toUpperCase().startsWith(type)) rest = rest.slice(type.length);
    else rest = rest.replace(MATERIAL_PREFIX, "");
    rest = rest.replace(/^[\s-]+/, "").trim();
    const cf = HAS_CF.test(type) || /carbon/i.test(detail);
    if (cf && !HAS_CF.test(rest) && !/carbon/i.test(rest)) rest = rest ? `${rest} CF` : "Carbon Fiber";
    finish = rest || "Basic";
  }
  return { family, finish: finish.slice(0, 18) };
}
const famRank = (f) => { const i = FAMILY_ORDER.indexOf(f); return i < 0 ? 99 : i; };
export const byKind = (a, b) => famRank(a.family) - famRank(b.family) || a.finish.localeCompare(b.finish);

export function deviceFeed(spools, printers) {
  const onHand = spools.filter((s) => !s.used_up);
  const left = (s) => Math.round((s.weight_g || 0) * (s.remain_pct ?? 100) / 100);

  // One card per roll, grouped by family -> finish -> color. Identical untouched rolls on the
  // shelf share a card ("x2") so there's less swiping.
  const cards = [];
  const merge = new Map();
  // Rolls in a printer are checked out: they show on the printer, not in the material lists.
  for (const s of onHand.filter((r) => !r.location)) {
    const { family, finish } = classify(s);
    const card = {
      fam: family,
      finish,
      name: (s.detail || s.type).slice(0, 22),
      color: colorName(s.hex) + (s.hex2 ? ` / ${colorName(s.hex2)}` : ""),
      hex: s.hex.slice(1),
      hex2: s.hex2 ? s.hex2.slice(1) : "",
      g: left(s),
      pct: s.remain_pct ?? -1,
      where: s.location || "",
      qty: 1,
    };
    const key = !card.where && card.pct < 0 ? `${card.name}|${card.hex}|${card.hex2}|${card.g}` : null;
    if (key && merge.has(key)) { const c = merge.get(key); c.qty++; c.g += card.g; continue; }
    if (key) merge.set(key, card);
    cards.push(card);
  }
  // Most first: materials by roll count, then finishes within a material by roll count.
  const famN = {}, finN = {};
  for (const c of cards) {
    famN[c.fam] = (famN[c.fam] || 0) + c.qty;
    finN[`${c.fam}|${c.finish}`] = (finN[`${c.fam}|${c.finish}`] || 0) + c.qty;
  }
  cards.sort((a, b) => famN[b.fam] - famN[a.fam] || a.fam.localeCompare(b.fam)
    || finN[`${b.fam}|${b.finish}`] - finN[`${a.fam}|${a.finish}`] || a.finish.localeCompare(b.finish)
    || b.qty - a.qty || a.color.localeCompare(b.color) || (a.where ? 0 : 1) - (b.where ? 0 : 1));

  // Family summaries for the section title cards.
  const families = [];
  for (const c of cards) {
    let f = families.find((x) => x.name === c.fam);
    if (!f) families.push((f = { name: c.fam, rolls: 0, g: 0, finishes: {}, dots: [] }));
    f.rolls += c.qty;
    f.g += c.g;
    f.finishes[c.finish] = (f.finishes[c.finish] || 0) + c.qty;
    if (f.dots.length < 10 && !f.dots.includes(c.hex)) f.dots.push(c.hex);
  }

  return {
    rolls: onHand.length,
    grams: onHand.reduce((t, s) => t + left(s), 0),
    materials: families.length,
    families: families.map((f) => ({
      name: f.name,
      rolls: f.rolls,
      g: f.g,
      finishes: Object.entries(f.finishes).sort((a, b) => b[1] - a[1]).map(([n, c]) => ({ n, c })).slice(0, 8),
      dots: f.dots,
    })),
    printers: printers.map((p) => ({
      name: p.name,
      online: !!p.status?.connected,
      state: p.status?.state || "",
      job: (p.status?.job || "").slice(0, 28),
      progress: p.status?.progress ?? 0,
      slots: onHand
        .filter((s) => s.printer_id === p.id && s.location)
        .map((s) => ({ where: s.location.split(" · ")[1] || s.location, hex: s.hex.slice(1), pct: s.remain_pct ?? -1, type: s.type }))
        .sort((a, b) => a.where.localeCompare(b.where)),
    })),
    list: cards,
  };
}
