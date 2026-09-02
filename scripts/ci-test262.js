#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Turns run-test262 output into the markdown that goes in the report, and
 * compares it against tests/test262/baseline.json.
 *
 *   node scripts/ci-test262.js <run-test262 output> <baseline.json> <out.md>
 *
 * A raw conformance number tells nobody anything. The delta against a recorded
 * baseline is the part worth reading, so this fails only when the engine gets
 * worse.
 */

'use strict';

const fs = require('fs');
const [, , outputPath, baselinePath, reportPath] = process.argv;

const output = fs.readFileSync(outputPath, 'utf8');
const baseline = JSON.parse(fs.readFileSync(baselinePath, 'utf8'));

// Result: 74/80896 errors, 5353 excluded, 5654 skipped
const found = /^Result: (\d+)\/(\d+) errors/m.exec(output);
if (!found) {
  fs.writeFileSync(
    reportPath,
    '### test262\n\nThe runner produced no result line. Its output:\n\n```\n' +
      output.split('\n').slice(-25).join('\n') +
      '\n```\n'
  );
  process.exit(1);
}

const errors = Number(found[1]);
const total = Number(found[2]);
const delta = errors - baseline.errors;

const lines = ['### test262', ''];

if (total !== baseline.total) {
  // A different test count means the suite moved, so the two failure counts
  // are not measuring the same thing.
  lines.push(
    `**${errors} failures of ${total}** — the baseline counted ${baseline.total} ` +
      `tests, so this run is not comparable. Update the baseline.`,
    ''
  );
} else if (delta > 0) {
  lines.push(`**${errors} failures of ${total}, ${delta} more than the baseline.**`, '');
} else if (delta < 0) {
  lines.push(
    `**${errors} failures of ${total}, ${-delta} fewer than the baseline.** ` +
      `Update tests/test262/baseline.json to keep the gate tight.`,
    ''
  );
} else {
  lines.push(`**${errors} failures of ${total}**, unchanged from the baseline.`, '');
}

lines.push('<details><summary>runner output</summary>', '', '```', output.trim(), '```', '', '</details>');
fs.writeFileSync(reportPath, lines.join('\n') + '\n');

// Only a regression fails. Fewer failures, or a moved suite, is for a human.
process.exit(delta > 0 && total === baseline.total ? 1 : 0);
