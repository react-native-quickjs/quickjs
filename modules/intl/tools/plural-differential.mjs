#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The plural-rules shim, diffed against node's ICU.
 *
 * WHY THIS IS A SEPARATE INSTRUMENT
 *   modules/intl/js/plural-data.js is the only per-locale table this module
 *   ships, and it is shipped because Foundation has no plural-category API in
 *   Objective-C or in Swift (MEASURED, docs/intl-completeness-map.md) while
 *   android.icu has had one since API 24. Everything else in the module is
 *   checked through an engine binary; this table is not, because what is being
 *   checked is the *data*, and running it through an engine would only add the
 *   engine's availableLocales() as a confounder — the no-platform backend
 *   reports one locale, so every tag would negotiate down to en-US and 223 of
 *   the 224 locales would never be exercised at all.
 *
 * WHAT IT COMPARES
 *   For each locale the table knows and node also knows, the plural category
 *   of a fixed value set, for cardinals and for ordinals, with the fraction
 *   digits set so that node's operands match the decimal string the shim is
 *   given. A mismatch is a bug in the shim or a CLDR version difference, and
 *   either way it is a finding.
 *
 * USAGE
 *   node modules/intl/tools/plural-differential.mjs
 *   node modules/intl/tools/plural-differential.mjs --verbose
 *
 *   exit 0  every checked pair agrees
 *   exit 1  at least one mismatch, listed
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   It does not check locales node's ICU does not have — those are counted and
 *   reported as skipped rather than silently passed, because a run that
 *   compared nothing would otherwise print a clean bill of health.
 */

import { readFileSync } from 'node:fs';

const DATA = new URL('../js/plural-data.js', import.meta.url).pathname;
const verbose = process.argv.includes('--verbose');

// eslint-disable-next-line no-new-func
const PLURAL_DATA = new Function(
  readFileSync(DATA, 'utf8') + '\nreturn PLURAL_DATA;')();

const CARDINALS = [
  '0', '1', '2', '3', '5', '6', '7', '8', '10', '11', '17', '20', '21', '22',
  '100', '101', '102', '103', '111', '1000', '10000', '1000000', '1000001',
  '0.0', '1.0', '1.5', '0.5', '2.5', '1.1', '10.0', '3.14', '0.1', '2.0',
  '1000000.0', '0.00', '1.00'];
const ORDINALS = ['1', '2', '3', '4', '5', '11', '12', '13', '21', '22', '23',
                  '101', '111', '1000'];

let checked = 0;
let skipped = 0;
const mismatches = [];

for (const loc of Object.keys(PLURAL_DATA.cardinal)) {
  let known = false;
  try {
    // node falls back silently for a locale it does not have, which would make
    // every comparison a comparison against `en`. The resolved language subtag
    // is the only way to tell "supported" from "fell back".
    known = new Intl.PluralRules(loc).resolvedOptions().locale
      .split('-')[0] === loc;
  } catch {
    known = false;
  }
  if (!known) { skipped++; continue; }

  for (const s of CARDINALS) {
    const frac = s.includes('.') ? s.split('.')[1].length : 0;
    const ours = PLURAL_DATA.cardinal[loc](s, 0);
    const theirs = new Intl.PluralRules(loc, {
      minimumFractionDigits: frac, maximumFractionDigits: frac,
    }).select(Number(s));
    checked++;
    if (ours !== theirs) mismatches.push(['cardinal', loc, s, ours, theirs]);
  }

  if (PLURAL_DATA.ordinal[loc]) {
    for (const s of ORDINALS) {
      const ours = PLURAL_DATA.ordinal[loc](s);
      const theirs = new Intl.PluralRules(loc, { type: 'ordinal' })
        .select(Number(s));
      checked++;
      if (ours !== theirs) mismatches.push(['ordinal', loc, s, ours, theirs]);
    }
  }

  const catsOurs = [
    ...(PLURAL_DATA.categories[loc]?.cardinal ?? ['other'])].sort().join(',');
  const catsTheirs = [
    ...new Intl.PluralRules(loc).resolvedOptions().pluralCategories]
    .sort().join(',');
  checked++;
  if (catsOurs !== catsTheirs) {
    mismatches.push(['categories', loc, '-', catsOurs, catsTheirs]);
  }
}

const total = Object.keys(PLURAL_DATA.cardinal).length;
process.stdout.write(
  `plural-differential: node ${process.version}, ` +
  `${total} locales in the table, ${total - skipped} checked against ICU, ` +
  `${skipped} skipped (node has no such locale)\n` +
  `  ${checked} comparisons, ${mismatches.length} mismatches\n`);

if (checked < 1000) {
  process.stderr.write(
    'plural-differential: fewer than 1000 comparisons ran. A harness pointed ' +
    'at nothing reports a clean run; this is that check.\n');
  process.exit(1);
}

for (const [kind, loc, value, ours, theirs] of
     mismatches.slice(0, verbose ? mismatches.length : 40)) {
  process.stdout.write(
    `  ${kind} ${loc} ${value}: ours=${ours} node=${theirs}\n`);
}
process.exit(mismatches.length > 0 ? 1 : 0);
