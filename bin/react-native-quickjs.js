#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

'use strict';

const run = require('../scripts/setup/run.js');
const [command, ...argv] = process.argv.slice(2);

if (command in run) {
  process.exit(run[command](argv));
}

console.log(`
react-native-quickjs

  install   configure this app to run on QuickJS
  revert    configure it back to Hermes
  doctor    report which parts are configured

  --project <path>   act on this app instead of the current directory
  --dry-run          show what would change, write nothing
`);
process.exit(command === undefined || command === '--help' || command === '-h' ? 0 : 1);
