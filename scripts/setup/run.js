/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * install, revert and doctor. All three walk the same table in edits.js.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const { edits } = require('./edits');

const GREEN = '\x1b[32m';
const YELLOW = '\x1b[33m';
const RED = '\x1b[31m';
const DIM = '\x1b[2m';
const OFF = '\x1b[0m';

function resolveProjectDirectory(argv) {
  const at = argv.indexOf('--project');
  const dir = at === -1 ? process.cwd() : path.resolve(argv[at + 1] || '.');
  if (!fs.existsSync(path.join(dir, 'package.json'))) {
    console.error(`\nNo package.json in ${dir}. Run this from a React Native app.\n`);
    process.exit(1);
  }
  return dir;
}

/**
 * @param {'addQuickJS'|'removeQuickJS'} direction
 * @returns {number} exit code
 */
function applyEdits(direction, argv) {
  const project = resolveProjectDirectory(argv);
  const dryRun = argv.includes('--dry-run');
  const manual = [];
  let changed = 0;

  console.log('');
  for (const edit of edits) {
    const file = edit.findFile(project);
    if (!file || !fs.existsSync(file)) {
      console.log(`  ${DIM}skipped${OFF}  ${edit.label} ${DIM}(not in this project)${OFF}`);
      continue;
    }

    const before = fs.readFileSync(file, 'utf8');
    const satisfied = direction === 'addQuickJS' ? edit.isApplied(before) : !edit.isApplied(before);
    if (satisfied) {
      console.log(`  ${DIM}already${OFF}  ${edit.label}`);
      continue;
    }

    const after = edit[direction](before);
    if (after == null || after === before) {
      console.log(`  ${YELLOW}by hand${OFF}  ${edit.label}`);
      manual.push([edit.label, edit.manualSteps]);
      continue;
    }

    if (!dryRun) fs.writeFileSync(file, after);
    console.log(`  ${GREEN}${dryRun ? 'would' : 'wrote'}${OFF}    ${edit.label}`);
    changed++;
  }

  for (const [label, lines] of manual) {
    console.log(`\n${YELLOW}${label}${OFF} could not be edited automatically:`);
    for (const line of lines) console.log(`  ${line}`);
  }

  if (changed && !dryRun && direction === 'addQuickJS') {
    console.log(`\nNext: cd ios && pod install\n`);
  } else {
    console.log('');
  }
  return manual.length ? 1 : 0;
}

function doctor(argv) {
  const project = resolveProjectDirectory(argv);
  let notConfigured = 0;
  let found = 0;

  console.log('');
  for (const edit of edits) {
    const file = edit.findFile(project);
    if (!file || !fs.existsSync(file)) {
      console.log(`  ${DIM}—${OFF}  ${edit.label} ${DIM}(not in this project)${OFF}`);
      continue;
    }
    found++;
    if (edit.isApplied(fs.readFileSync(file, 'utf8'))) {
      console.log(`  ${GREEN}✓${OFF}  ${edit.label}`);
    } else {
      console.log(`  ${RED}✗${OFF}  ${edit.label}`);
      notConfigured++;
    }
  }

  if (!found) {
    console.log(`\nNo React Native app found in ${project}\nUse --project <path> to point at one.\n`);
    return 1;
  }

  console.log(
    notConfigured
      ? `\n${notConfigured} of ${edits.length} not configured for QuickJS. ` +
          `Run: npx react-native-quickjs install\n`
      : `\nConfigured to run on QuickJS.\n`
  );
  return notConfigured ? 1 : 0;
}

module.exports = {
  install: (argv) => applyEdits('addQuickJS', argv),
  revert: (argv) => applyEdits('removeQuickJS', argv),
  doctor,
};
