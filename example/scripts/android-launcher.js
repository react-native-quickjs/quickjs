#!/usr/bin/env node

'use strict';

const fs = require('fs');
const path = require('path');
const {spawnSync} = require('child_process');

const root = path.resolve(__dirname, '..');
const android = path.join(root, 'android');
const command = process.argv[2];
const engine = process.argv[3] || process.env.RNQJS_ENGINE || 'quickjs';

if (!['info', 'release'].includes(command))
  fail('usage: android-launcher.js <info|release> [quickjs|hermes]');
if (!['quickjs', 'hermes'].includes(engine))
  fail(`RNQJS_ENGINE must be quickjs or hermes (got '${engine}')`);

const lazy = process.env.RNQJS_LAZY;
if (lazy !== undefined && !['0', '1'].includes(lazy)) fail('RNQJS_LAZY must be 0 or 1');

const config = {
  engine,
  runtimeFactory: engine === 'quickjs' ? 'QuickJSInstance' : 'React Native Hermes',
  bundleCompiler: engine === 'quickjs' ? 'qjsc (NSBCNGS)' : 'React Native hermesc (HBC)',
  bytecodeFormat: engine === 'quickjs' ? 'NSBCNGS / BC_VERSION 28' : 'Hermes HBC',
  applicationId: `com.reactnativequickjs.example.${engine}`,
  gradleTask: ':app:assembleRelease',
};

if (command === 'info') {
  Object.entries(config).forEach(([key, value]) => console.log(`${key}: ${value}`));
  process.exit(0);
}

const gradlew = process.platform === 'win32' ? 'gradlew.bat' : './gradlew';
const result = spawnSync(gradlew, ['--no-daemon', '--console=plain', ':app:assembleRelease',
  `-PbenchmarkEngine=${engine}`, `-PhermesEnabled=${engine === 'hermes'}`,
  `-PrnqjsBytecode=${engine === 'quickjs'}`, `-PrnqjsHermesCompat=${engine === 'quickjs'}`,
  `-PrnqjsLazy=${lazy !== '0'}`, '-PreactNativeArchitectures=arm64-v8a',
  ], {
  cwd: android,
  env: {...process.env, RNQJS_ENGINE: engine},
  stdio: 'inherit',
});
if (result.error) fail(result.error.message);
if (result.status !== 0) process.exit(result.status || 1);
console.log(`built ${config.applicationId} with ${config.bundleCompiler}`);

function fail(message) { console.error(`[android] ERROR: ${message}`); process.exit(2); }
