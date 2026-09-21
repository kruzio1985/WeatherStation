/* =============================================================================
 * Stacja Pogody - kontrola tlumaczen nazw kanalow (data/i18n.js)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * -----------------------------------------------------------------------------
 * Nazwa kanalu z firmware (ChanDef.name / mkChan / .name=) jest jednoczesnie
 * kluczem w SLOWNIK (data/i18n.js). Ten skrypt wypisuje nazwy, ktorych brakuje
 * w slowniku - bez nich strona pokaze nazwe po polsku takze w EN/DE.
 * Sprawdza cztery grupy nazw z firmware oraz liste z zakladki "Kalibracja"
 * (CALIB_ADV w data/index.html) razem z jej tlumaczeniami i z tym, czy kazdy
 * klucz kalibracji czytany przez driver (config.extraF) ma pole na stronie.
 *
 * Uruchomienie (z katalogu projektu):
 *     node tools/check_i18n_names.js
 * ========================================================================== */
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const I18N = path.join(ROOT, 'data', 'i18n.js');
const PINMAP = path.join(ROOT, 'src', 'pinmap.cpp');
const INDEX = path.join(ROOT, 'data', 'index.html');
const SRC = path.join(ROOT, 'src');

const CHAN_ROW = /^\s*\{\s*"([a-z0-9_]+)"\s*,\s*"((?:[^"\\]|\\.)*)"/;
const PIN_ROW = /^\s*\{\s*"([a-z0-9_]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"/;
const DOTNAME = /\.name\s*=\s*"((?:[^"\\]|\\.)*)"/g;

function read(p) { return fs.readFileSync(p, 'utf8'); }

function collectNames() {
  const names = new Map();          // nazwa -> plik
  const srcDir = path.join(ROOT, 'src');
  const files = fs.readdirSync(srcDir).filter((f) => {
    if (!/\.cpp$/.test(f)) return false;
    if (f === 'sensors.cpp' || f === 'sensors_extra.cpp') return true;
    return /^drv_/.test(f) && f !== 'drv_table.cpp' && f !== 'drv_mod.cpp';
  });
  for (const f of files) {
    const text = read(path.join(srcDir, f));
    text.split(/\r?\n/).forEach((line) => {
      const row = CHAN_ROW.exec(line);
      if (row) {
        if (!names.has(row[2])) names.set(row[2], f);
        return;
      }
      if (line.includes('mkChan(')) {
        const parts = line.split('"').filter((_, i) => i % 2 === 1);
        if (parts.length >= 2 && /[a-zA-Z]/.test(parts[1])) {
          if (!names.has(parts[1])) names.set(parts[1], f);
        }
        return;
      }
      let g;
      const re = new RegExp(DOTNAME.source, 'g');
      while ((g = re.exec(line)) !== null) {
        const nm = g[1];
        if (!nm) continue;
        const tail = line.slice(g.index + g[0].length);
        if (/^\s*\+/.test(tail)) continue;          // nazwa skladana dynamicznie
        if (!names.has(nm)) names.set(nm, f);
      }
    });
  }
  return names;
}

function collectKeys() {
  const text = read(I18N);
  const keys = new Set();
  const re = /'((?:[^'\\]|\\.)*)'\s*:\s*\[/g;
  let m;
  while ((m = re.exec(text)) !== null) keys.add(m[1]);
  return keys;
}

function collectPinLabels() {
  const labels = { nazwa: new Map(), grupa: new Map(), opis: new Map() };
  const text = read(PINMAP);
  text.split(/\r?\n/).forEach((line) => {
    const m = PIN_ROW.exec(line);
    if (!m) return;
    if (m[2]) labels.nazwa.set(m[2], line.trim().slice(0, 60));
    if (m[3]) labels.grupa.set(m[3], line.trim().slice(0, 60));
    if (m[4]) labels.opis.set(m[4], line.trim().slice(0, 60));
  });
  return labels;
}

function collectCalibUi() {
  const labels = new Map();
  const keySet = new Set();
  const text = read(INDEX);
  const block = /const CALIB_ADV = \[([\s\S]*?)\n\];/.exec(text);
  if (!block) return { labels, keySet };
  for (const row of block[1].split(/\r?\n/)) {
    if (!/\bk\s*:/.test(row)) continue;
    const k = /k:\s*'([^']+)'/.exec(row);
    if (k) keySet.add(k[1]);
    for (const m of row.matchAll(/\b[gl]:\s*'((?:[^'\\]|\\.)*)'/g)) labels.set(m[1], 'CALIB_ADV');
    const o = /o:\s*\[([^\]]*)\]/.exec(row);
    if (o) for (const m of o[1].matchAll(/'((?:[^'\\]|\\.)*)'/g)) labels.set(m[1], 'CALIB_ADV');
  }
  return { labels, keySet };
}

function collectDriverCalibKeys() {
  const found = new Map();
  for (const f of fs.readdirSync(SRC).filter((n) => /^drv_.*\.cpp$/.test(n))) {
    const re = /(?:extraF|hasExtra)\(\s*"([A-Za-z0-9_]+)"/g;
    for (const m of read(path.join(SRC, f)).matchAll(re)) {
      if (!found.has(m[1])) found.set(m[1], f);
    }
  }
  return found;
}

const names = collectNames();
const keys = collectKeys();
const pins = collectPinLabels();
const calib = collectCalibUi();
const drvKeys = collectDriverCalibKeys();

const groups = [
  { title: 'kanaly (ChanDef / mkChan)', map: names },
  { title: 'piny - nazwy', map: pins.nazwa },
  { title: 'piny - grupy', map: pins.grupa },
  { title: 'piny - opisy', map: pins.opis },
  { title: 'kalibracja ukladow - pola', map: calib.labels },
];

console.log('kluczy w i18n.js: ' + keys.size);
let issues = 0;
for (const g of groups) {
  const missing = [...g.map.keys()].filter((n) => !keys.has(n));
  if (!missing.length) {
    console.log('OK   ' + g.title + ' (' + g.map.size + ')');
    continue;
  }
  issues += missing.length;
  console.log('BRAK ' + g.title + ' (' + missing.length + '/' + g.map.size + '):');
  for (const n of missing) console.log('       - ' + n.slice(0, 80));
}

// Klucze kalibracji czytane przez drivery musza miec pole w formularzu CALIB_ADV.
// Wyjatki (klucze swiadomie bez pola) dopisujemy do CALIB_NO_UI.
const CALIB_NO_UI = new Set();
const noUi = [...drvKeys.keys()].filter((k) => !calib.keySet.has(k) && !CALIB_NO_UI.has(k));
if (!noUi.length) {
  console.log('OK   kalibracja ukladow - klucze driverow w formularzu (' + drvKeys.size + ')');
} else {
  issues += noUi.length;
  console.log('BRAK kalibracja ukladow - klucze bez formularza (' + noUi.length + '):');
  for (const k of noUi) console.log('       - ' + k + '  [' + drvKeys.get(k) + ']');
}
const noKey = [...calib.keySet].filter((k) => !drvKeys.has(k));
if (noKey.length) {
  console.log('INFO kalibracja ukladow - pola bez klucza w driverach: ' + noKey.join(', '));
}
process.exit(issues ? 1 : 0);
