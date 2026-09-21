/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Analiza pokrycia słownika data/i18n.js:
//  - duplikaty kluczy w SLOWNIK,
//  - napisy z firmware (src/*.cpp, src/*.h), które nie mają tłumaczenia,
//  - napisy ze stron www (data/*.html), które nie mają tłumaczenia.
// Uruchomienie:  node tools/analyze_i18n.js
// Raport:        %TEMP%\i18n_check.txt  (Linux: /tmp/i18n_check.txt)
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const I18N = path.join(ROOT, 'data/i18n.js');

const norm = (s) => String(s).replace(/\s+/g, ' ').trim();

function loadDict() {
  const src = fs.readFileSync(I18N, 'utf8');
  const i = src.lastIndexOf('})(window);');
  if (i < 0) throw new Error('nie znaleziono zamknięcia IIFE');
  // eksport słowników z wnętrza IIFE pliku
  const patched = src.slice(0, i) +
    '  globalThis.__dict = { SLOWNIK: SLOWNIK, REGEX: REGEX, FRAZY: FRAZY };\n' + src.slice(i);
  const vm = require('vm');
  const sandbox = {
    console,
    navigator: { language: 'pl' },
    document: {
      readyState: 'complete',
      documentElement: {
        nodeType: 1,
        hasAttribute: () => false,
        getAttribute: () => null,
        setAttribute: () => { },
      },
      getElementById: () => null,
      addEventListener: () => { },
      createElement: () => ({ style: {}, dataset: {} }),
    },
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  vm.runInNewContext(patched, sandbox, { filename: 'i18n.js' });

  const s = src.indexOf('var SLOWNIK = {');
  const e = src.indexOf('\n  };', s);
  return { slownikSrc: src.slice(s, e), dict: sandbox.__dict };
}

// --- duplikaty kluczy w SLOWNIK (z tekstu źródłowego) ---
function findDuplicates(slownikSrc) {
  const re = /^[ \t]*(?:'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)")\s*:\s*(.*)$/gm;
  const list = [];
  let m;
  while ((m = re.exec(slownikSrc))) list.push({ key: m[1] !== undefined ? m[1] : m[2], line: m[3] });
  const seen = new Map();
  const dups = new Map();
  list.forEach((e) => {
    const n = norm(e.key);
    if (seen.has(n)) {
      if (!dups.has(n)) dups.set(n, [seen.get(n)]);
      dups.get(n).push(e.line);
    } else seen.set(n, e.line);
  });
  return { count: list.length, dups };
}

// --- literały napisowe z tekstu ---
function literals(s) {
  const out = [];
  [/"((?:[^"\\]|\\.)*)"/g, /`((?:[^`\\]|\\.)*)`/g, /'((?:[^'\\]|\\.)*)'/g].forEach((re) => {
    let m;
    while ((m = re.exec(s))) out.push(m[1]);
  });
  return out.map((v) => v.replace(/\\"/g, '"').replace(/\\'/g, "'").replace(/\\n/g, ' ').replace(/\\t/g, ' ').replace(/\\\\/g, '\\'));
}

const PL = /[ąćęłńóśźżĄĆĘŁŃÓŚŹŻ]/;
const PL_WORDS =
  /\b(nie|brak|wolne|wolny|wolna|wolnych|wolnego|ustawienia|ustaw|strona|strony|danych|dane|sieci|sieć|plik|pliku|pliki|karta|karty|hasło|adres|tryb|stan|czas|wersja|czujnik|czujniki|blok|bloki|wpisy|wpisów|kopi|kopia|zapis|dziennik|logów|błąd|błędy|nieznany|nieznana|trwa|uruchomień|powód|obsługa|włącz|wyłącz|nazwa|nazwy|klient|punkt|dostępu|ram|pamięć|pamięci|maska|brama|kanał|kanały|sygnał|numer|zajęte|dostępna|dostępne|zapisane|zapisano|usunięte|nieudane|gotowe|wykryto|wykryte|podłączon|zainicj|sformatowan|montaż|sterownik|jasność|diody|diod|kolejność|kolorów|kamera|wgran|restart|zmian|wybierz|wpisz|okres|interwał|zakładka|strefa|czasowa|puste|żądanie|błędny|nieprawidłow|sekcji|wymaga|wejścia|analogowego|zakres|przypisany|użyty|raz|pin|piny|pinów|stacji|pogody|pomiar|opad|wiatr|kierunek|temperatura|wilgotność|ciśnienie|światła|natężenie|dziś|zewn|przycisk|pierścień|aktywna|partycja|następna|aktualizacja|aktualizacji|rozmiar|slot|przeglądark|serwer|prefiks|tematów|magistrali|jakości|powietrza|program|kodowanie|dostęp|wysłane|czeka|aktywne)\b/i;

function looksPolish(v) {
  const t = norm(v);
  if (!t || t.length < 2) return false;
  if (PL.test(t)) return true;
  return PL_WORDS.test(t) && /[a-z]/i.test(t);
}

function buildMaps(dict) {
  const en = new Map();
  Object.keys(dict.SLOWNIK).forEach((k) => en.set(norm(k), dict.SLOWNIK[k][0]));
  const ph = Object.keys(dict.FRAZY).map(norm).sort((a, b) => b.length - a.length);
  return { en, ph };
}

function status(key, maps, dict) {
  const n = norm(key);
  if (maps.en.has(n)) return 'exact';
  for (const r of dict.REGEX) {
    r[0].lastIndex = 0;
    if (r[0].test(n)) return 'regex';
  }
  for (const p of maps.ph) if (n.indexOf(p) >= 0) return 'fragment';
  return 'BRAK';
}

const LOG_MARK = /(LOG_[A-Z]|Serial\s*\.|snprintf_?|printf_?|addLog|logf)\s*\(?[^;]{0,40}$/;

// czy literał występuje tylko w wywołaniu logu (Serial/LOG_x) - takich nie tłumaczymy
function isLogOnly(fileSrc, lit) {
  const forms = ['"' + lit + '"', '`' + lit + '`', "'" + lit + "'"];
  let idx = -1;
  for (const f of forms) {
    idx = fileSrc.indexOf(f);
    if (idx >= 0) break;
  }
  if (idx < 0) return false;
  const before = fileSrc.slice(Math.max(0, idx - 400), idx);
  const cut = Math.max(before.lastIndexOf(';'), before.lastIndexOf('{'), before.lastIndexOf('}'));
  return LOG_MARK.test(before.slice(cut + 1));
}

// literały, które są fragmentami/formatami i nie da się ich sensownie tłumaczyć
function noise(lit) {
  if (/%[a-zA-Z]/.test(lit)) return true;
  if (lit.length > 160) return true;
  if (/^[^A-Za-zÀ-ž]+$/.test(lit)) return true;
  return false;
}

(function main() {
  const out = [];
  const say = (s) => out.push(s);

  const { slownikSrc, dict } = loadDict();
  const dup = findDuplicates(slownikSrc);
  say('=== SLOWNIK: kluczy ' + dup.count + ' ===');
  if (dup.dups.size) {
    dup.dups.forEach((vals, k) => say('  DUPLIKAT "' + k + '" x' + vals.length + ': ' + vals.join(' | ')));
  } else say('  brak duplikatów');
  say('  REGEX: ' + dict.REGEX.length + ', FRAZY: ' + Object.keys(dict.FRAZY).length);

  const maps = buildMaps(dict);

  const missing = new Map();
  const srcdir = path.join(ROOT, 'src');
  fs.readdirSync(srcdir)
    .filter((f) => /\.(cpp|h)$/.test(f))
    .forEach((f) => {
      const fileSrc = fs.readFileSync(path.join(srcdir, f), 'utf8');
      literals(fileSrc).forEach((v) => {
        if (noise(v) || !looksPolish(v) || isLogOnly(fileSrc, v)) return;
        const st = status(v, maps, dict);
        if (st !== 'exact') {
          const k = st + ' || ' + norm(v);
          if (!missing.has(k)) missing.set(k, f);
        }
      });
    });
  say('');
  say('=== FW (do www/JSON, bez logów i formatów): ' + missing.size + ' ===');
  [...missing.entries()].sort().forEach(([k, f]) => say('  [' + k + ']  <' + f + '>'));

  const htmlFiles = ['index.html', 'public.html'];
  const htm = new Map();
  htmlFiles.forEach((f) => {
    const s = fs.readFileSync(path.join(ROOT, 'data', f), 'utf8');
    const chunks = literals(s);
    s.replace(/<script[\s\S]*?<\/script>/gi, ' ')
      .replace(/<style[\s\S]*?<\/style>/gi, ' ')
      .split(/[<>]/)
      .forEach((seg) => chunks.push(seg));
    chunks.forEach((v) => {
      const t = norm(v);
      if (/[=&]/.test(t) || /^\s*[.#]/.test(t)) return; // fragmenty HTML/atrybutów
      if (noise(t) || !looksPolish(t)) return;
      const st = status(t, maps, dict);
      if (st !== 'exact') {
        const k = st + ' || ' + t;
        if (!htm.has(k)) htm.set(k, f);
      }
    });
  });
  say('');
  say('=== WWW: napisy bez dokładnego pokrycia (' + htm.size + ') ===');
  [...htm.entries()].sort().forEach(([k, f]) => say('  [' + k + ']  <' + f + '>'));

  const file = path.join(require('os').tmpdir(), 'i18n_check.txt');
  fs.writeFileSync(file, out.join('\n') + '\n', 'utf8');
  console.log('raport: ' + file);
  console.log('duplikaty: ' + dup.dups.size + ', FW: ' + missing.size + ', WWW: ' + htm.size);
})();
