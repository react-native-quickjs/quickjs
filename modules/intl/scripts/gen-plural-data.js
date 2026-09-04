#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Generates modules/intl/js/plural-data.js from `make-plural`.
 *
 * WHY THIS IS THE ONE PLACE THE MODULE SHIPS CLDR DATA
 *   The rule for this module is "never ship data the OS already has".
 *   Intl.PluralRules is the single ECMA-402 service where one of the two
 *   operating systems does not have it: MEASURED in
 *   docs/intl-completeness-map.md, Foundation exposes no plural-category API in
 *   Objective-C *or* in Swift, while android.icu.text.PluralRules has existed
 *   since API 24. So the choice is between
 *
 *     (a) android.icu's rule engine on Android and a JavaScript one on Apple,
 *         which means two rule engines, two CLDR vintages, and an app whose
 *         iOS and Android builds can select different *sentences*; or
 *     (b) the same JavaScript rule engine on both.
 *
 *   (b) is chosen, and the data is not duplication because half the platforms
 *   do not have it. It costs ~36 KB of source for all 224 CLDR locales,
 *   against @formatjs/intl-pluralrules' 306 KB for the same coverage — smaller
 *   than formatjs's *15-locale* subset.
 *
 * INPUT   the `make-plural` npm package (ESM: cardinals.js, ordinals.js,
 *         ranges.js, pluralCategories.js), located by --input.
 * OUTPUT  modules/intl/js/plural-data.js, a plain script (no ESM, no
 *         template literals) defining one `var PLURAL_DATA`.
 *
 * CALLING CONVENTION OF THE GENERATED SELECTORS — load-bearing
 *   make-plural's selectors take the number as a **string** and derive the
 *   ECMA-402 operands (i, v, w, f, t) from it, which is exactly what
 *   Intl.PluralRules needs: the category depends on the *formatted* value, so
 *   `1.0` is "other" in English while `1` is "one". js/intl.js therefore passes
 *   the already-rounded decimal string it computed for format(), and the
 *   operands can never disagree with the digits the user sees.
 *
 *     PLURAL_DATA.cardinal[loc](decimalString, compactExponent)
 *     PLURAL_DATA.ordinal[loc](decimalString)
 *     PLURAL_DATA.range[loc](startCategory, endCategory)
 *     PLURAL_DATA.categories[loc] -> {cardinal: [...], ordinal: [...]}
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   No minification and no renaming. The output is `make-plural`'s own
 *   deduplicated form with the ESM syntax rewritten, so a reader can diff it
 *   against the upstream package and see that nothing was invented.
 *
 * REGENERATING
 *   node modules/intl/scripts/gen-plural-data.js \
 *       --input bench/spikes/intl/node_modules/make-plural
 *   The output is committed. It is not regenerated during the build, because a
 *   build that reaches into node_modules for locale data is a build that can
 *   silently change its answers when a lockfile moves.
 */

'use strict';

const fs = require('fs');
const path = require('path');

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
}

const input = arg(
  '--input',
  path.join(__dirname, '../../../bench/spikes/intl/node_modules/make-plural'));
const output = arg('--out', path.join(__dirname, '../js/plural-data.js'));

/**
 * Rewrites one make-plural ESM file into (helpers, exportNames).
 *
 * The package's shape is stable and simple: a run of `const <id> = <expr>;`
 * helper definitions, then a run of `export const <locale> = <expr>;`. Both
 * `<expr>` forms can span lines, so the split is on the `export const` token
 * rather than on newlines.
 */
function convert(file) {
  const src = fs.readFileSync(path.join(input, file), 'utf8');
  const marker = '\nexport const ';
  const first = src.indexOf(marker);
  if (first < 0) throw new Error('no exports found in ' + file);

  const helpers = src.slice(0, first).trim();
  const rest = src.slice(first);

  const names = [];
  const bodies = [];
  const chunks = rest.split(marker).slice(1);
  for (const chunk of chunks) {
    const eq = chunk.indexOf(' = ');
    if (eq < 0) throw new Error('unparsable export in ' + file + ': ' + chunk.slice(0, 40));
    const name = chunk.slice(0, eq).trim();
    let body = chunk.slice(eq + 3).trim();
    if (body.endsWith(';')) body = body.slice(0, -1);
    if (!/^[A-Za-z][A-Za-z0-9_]*$/.test(name)) {
      throw new Error('unexpected export name in ' + file + ': ' + name);
    }
    names.push(name);
    bodies.push([name, body]);
  }
  return { helpers, bodies };
}

const cardinals = convert('cardinals.js');
const ordinals = convert('ordinals.js');
const ranges = convert('ranges.js');
const categories = convert('pluralCategories.js');

/*
 * Each of the four tables becomes its own IIFE with make-plural's helper
 * definitions copied in verbatim.
 *
 * The first version of this generator renamed the helpers instead, to
 * namespace `a`, `b`, `c` across the four files. That was wrong twice over and
 * both failures were silent: the rename regex also caught the *inner*
 * `const s = String(n).split('.')` inside a selector body while leaving the
 * `s[1]` references alone, and `pluralCategories.js` declares six helpers in
 * one `const z = "zero", o = "one", ...` statement of which only the first was
 * renamed. A closure needs no renaming at all, so the class of bug is removed
 * rather than the instance.
 */
function emitTable(varName, conv, count) {
  const entries = conv.bodies.map(([name, body]) =>
    // Quote every key: some CLDR locale codes are JavaScript reserved words,
    // and an unquoted one is a syntax error rather than a wrong answer.
    '    ' + JSON.stringify(name) + ': ' + body);
  return (
    'var ' + varName + ' = (function () {\n' +
    conv.helpers + '\n' +
    '  return {\n' + entries.join(',\n') + '\n  };\n' +
    '})();\n');
}

const parts = [
  ['PLURAL_CARDINAL', cardinals, 'cardinal'],
  ['PLURAL_ORDINAL', ordinals, 'ordinal'],
  ['PLURAL_RANGE', ranges, 'range'],
  ['PLURAL_CATEGORIES', categories, 'categories'],
];

const header = `/*
 * GENERATED by modules/intl/scripts/gen-plural-data.js. DO NOT EDIT.
 *
 * Source: make-plural ${JSON.parse(fs.readFileSync(path.join(input, 'package.json'), 'utf8')).version}
 *         (Unicode-DFS-2016 licence), which compiles the CLDR plural rules.
 *
 * This is the only per-locale table in modules/intl, and it is here because
 * Apple has no plural-category API in Objective-C or in Swift while Android
 * does — see modules/intl/scripts/gen-plural-data.js for the full argument,
 * and docs/intl-completeness-map.md for the measurement behind it.
 *
 * Selectors take the number as a decimal STRING, which is how the ECMA-402
 * operands v/w/f/t (visible fraction digits) can be correct: js/intl.js passes
 * the same rounded string it hands to the platform for formatting.
 */
`;

const body = parts
  .map(([varName, conv, key]) =>
    '/* ' + key + ': ' + conv.bodies.length + ' locales */\n' +
    emitTable(varName, conv))
  .join('\n');

const tail = `
var PLURAL_DATA = {
  cardinal: PLURAL_CARDINAL,
  ordinal: PLURAL_ORDINAL,
  range: PLURAL_RANGE,
  categories: PLURAL_CATEGORIES
};
`;

fs.writeFileSync(output, header + '\n' + body + tail);

const size = fs.statSync(output).size;
process.stdout.write(
  'plural-data.js  ' + size + ' bytes  (' +
  parts.map(([, conv, key]) => key + ' ' + conv.bodies.length).join(', ') + ')\n');
