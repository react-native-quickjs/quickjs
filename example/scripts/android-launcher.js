#!/usr/bin/env node

'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {execFileSync, spawnSync} = require('child_process');

const root = path.resolve(__dirname, '..');
const android = path.join(root, 'android');
const command = process.argv[2];
const suppliedEngine = process.argv[3] || process.env.RNQJS_ENGINE;

if (!['info', 'release', 'benchmark'].includes(command)) {
  fail('usage: android-launcher.js <info|release|benchmark> [quickjs|hermes]');
}
if (process.argv[3] && !['quickjs', 'hermes'].includes(process.argv[3])) {
  fail(`engine must be quickjs or hermes (got '${process.argv[3]}')`);
}
if (command === 'benchmark' && !suppliedEngine) {
  fail('RNQJS_ENGINE must be set explicitly for benchmark runs');
}
const engine = suppliedEngine || 'quickjs';
if (!['quickjs', 'hermes'].includes(engine)) {
  fail(`RNQJS_ENGINE must be quickjs or hermes (got '${engine}')`);
}

const lazy = process.env.RNQJS_LAZY;
if (lazy !== undefined && !['0', '1'].includes(lazy)) {
  fail('RNQJS_LAZY must be 0 or 1');
}
if (engine === 'hermes' && lazy !== undefined) {
  fail('RNQJS_LAZY is only valid with RNQJS_ENGINE=quickjs');
}
const exampleMode = process.env.RNQJS_EXAMPLE_MODE || 'simple';
if (!['simple', 'benchmark'].includes(exampleMode)) {
  fail(`RNQJS_EXAMPLE_MODE must be simple or benchmark (got '${exampleMode}')`);
}
if (command === 'benchmark' && exampleMode !== 'benchmark') {
  fail('benchmark requires RNQJS_EXAMPLE_MODE=benchmark');
}
const compilation = process.env.RNQJS_COMPILATION || 'none';
if (!['none', 'full'].includes(compilation)) {
  fail('RNQJS_COMPILATION must be none or full');
}
const iterations = Number(process.env.RNQJS_BENCHMARK_ITERATIONS || 20);
if (!Number.isInteger(iterations) || iterations < 1 || iterations > 30) {
  fail('RNQJS_BENCHMARK_ITERATIONS must be an integer from 1 to 30');
}
const gapMs = Number(process.env.RNQJS_BENCHMARK_GAP_MS || 0);
if (!Number.isInteger(gapMs) || gapMs < 0) {
  fail('RNQJS_BENCHMARK_GAP_MS must be a non-negative integer');
}

const config = {
  engine,
  runtimeFactory: engine === 'quickjs' ? 'QuickJSInstance' : 'React Native Hermes',
  bundleCompiler: engine === 'quickjs' ? 'qjsc (NSBCNGS)' : 'React Native hermesc (HBC)',
  bytecodeFormat: engine === 'quickjs' ? 'NSBCNGS / BC_VERSION 28' : 'Hermes HBC',
  hermesCompatShim: engine === 'quickjs' ? 'enabled' : 'disabled',
  expectedNativeLibraries: engine === 'quickjs'
    ? 'libquickjsinstancejni.so; optional libhermesvm.so compatibility shim'
    : 'libhermesvm.so, libhermestooling.so (no QuickJS)',
  applicationId: `com.reactnativequickjs.example.${engine}${exampleMode === 'benchmark' ? '.benchmark' : ''}`,
  exampleMode,
  lazy: engine === 'quickjs' ? (lazy === '0' ? 'eager' : 'copied-lazy') : 'not applicable',
  compilation: compilation === 'full' ? 'Full (compiled ART)' : 'None (uncompiled ART)',
  iterations: command === 'benchmark' ? iterations : 'not applicable',
  gradleTask: ':app:assembleRelease',
};

if (command === 'info') {
  Object.entries(config).forEach(([key, value]) => console.log(`${key}: ${value}`));
  process.exit(0);
}

const gradlew = process.platform === 'win32' ? 'gradlew.bat' : './gradlew';
const props = [
  `-PbenchmarkEngine=${engine}`,
  `-PhermesEnabled=${engine === 'hermes'}`,
  `-PrnqjsBytecode=${engine === 'quickjs'}`,
  `-PrnqjsHermesCompat=${engine === 'quickjs'}`,
  `-PrnqjsLazy=${lazy !== '0'}`,
  '-PreactNativeArchitectures=arm64-v8a',
  `-PrnqjsExampleMode=${exampleMode}`,
];
const task = ':app:assembleRelease';
const gradleArgs = ['--no-daemon', '--console=plain', ...props];
const result = spawnSync(gradlew, [task, ...gradleArgs], {
  cwd: android,
  env: {...process.env, RNQJS_ENGINE: engine, RNQJS_EXAMPLE_MODE: exampleMode},
  stdio: 'inherit',
});
if (result.error) fail(result.error.message);
if (result.status !== 0) process.exit(result.status || 1);

const apk = path.join(android, `build-${engine}-${exampleMode}`, 'outputs/apk/release/app-release.apk');
if (command === 'release' || command === 'benchmark') {
  verifyApk(apk, config);
}
if (command === 'benchmark') {
  installAndCheck(apk, config.applicationId);
  runLogcatBenchmark(config.applicationId, iterations);
}

function verifyApk(apkPath, expected) {
  if (!fs.existsSync(apkPath)) fail(`APK not found: ${apkPath}`);
  const names = unzip(['-Z1', apkPath]).split(/\r?\n/).filter(Boolean);
  const bundle = unzip(['-p', apkPath, 'assets/index.android.bundle']);
  const isQuickJS = bundle.subarray(0, 7).toString() === 'NSBCNGS';
  const isHermes = bundle[0] === 0xc6 && bundle[1] === 0x1f &&
      bundle[2] === 0xbc && bundle[3] === 0x03;
  const hasQuickJS = names.some((name) => name.endsWith('/libquickjsinstancejni.so'));
  const hasHermes = names.some((name) => name.endsWith('/libhermesvm.so'));
  const hasHermesTooling = names.some((name) => name.endsWith('/libhermestooling.so'));
  if (expected.engine === 'quickjs' &&
      (!isQuickJS || !hasQuickJS || hasHermesTooling)) {
    fail('QuickJS APK verification failed');
  }
  if (expected.engine === 'hermes' && (!isHermes || !hasHermes || !hasHermesTooling || hasQuickJS)) {
    fail('Hermes APK verification failed');
  }
  console.log(JSON.stringify({
    engine: expected.engine,
    applicationId: expected.applicationId,
    apk: apkPath,
    apkSha256: sha256(fs.readFileSync(apkPath)),
    bundleSha256: sha256(bundle),
    bundleFormat: isQuickJS ? 'NSBCNGS/BC_VERSION-28' : isHermes ? 'Hermes HBC' : 'unknown',
    nativeLibraries: names.filter((name) => name.includes('/lib') && name.endsWith('.so')),
    buildType: expected.buildType || 'release',
    architecture: 'arm64-v8a',
    lazy: expected.lazy,
    exampleMode: expected.exampleMode,
  }, null, 2));
}

function installAndCheck(apkPath, applicationId) {
  const adb = process.env.ADB || 'adb';
  try {
    execFileSync(adb, ['install', '-r', apkPath], {stdio: 'inherit'});
    const installed = execFileSync(adb, ['shell', 'pm', 'path', applicationId], {
      encoding: 'utf8',
    }).trim();
    if (!installed.startsWith('package:')) {
      fail(`target package ${applicationId} is not installed`);
    }
    console.log(`benchmark target package: ${applicationId}`);
    console.log(`benchmark installed APK: ${installed}`);
  } catch (error) {
    fail(`could not install or inspect ${applicationId}: ${error.message}`);
  }
}

function runLogcatBenchmark(applicationId, count) {
  const adb = process.env.ADB || 'adb';
  console.log(`benchmark target package: ${applicationId}`);
  console.log(`benchmark installed APK: ${execFileSync(adb, ['shell', 'pm', 'path', applicationId], {encoding: 'utf8'}).trim()}`);
  console.log(`benchmark mode: release APK, ${count} cold launches, TTIModule appLoaded marker`);
  for (let i = 0; i < count; i += 1) {
    if (i > 0 && gapMs > 0) sleep(gapMs);
    execFileSync(adb, ['shell', 'am', 'force-stop', applicationId]);
    execFileSync(adb, ['logcat', '-c']);
    const started = process.hrtime.bigint();
    execFileSync(adb, [
      'shell', 'am', 'start', '-W',
      '-n', `${applicationId}/com.reactnativequickjs.example.MainActivity`,
    ], {stdio: 'ignore'});
    let marker = '';
    const deadline = Date.now() + 10_000;
    while (Date.now() < deadline) {
      marker = execFileSync(adb, [
        'logcat', '-d', '-v', 'epoch', '-s', 'RNQJS_TTI:I',
      ], {encoding: 'utf8'});
      if (marker.includes('TTI event=appLoaded')) break;
      sleep(50);
    }
    if (!marker.includes('TTI event=appLoaded')) {
      fail(`TTIModule appLoaded did not appear for ${applicationId} iteration ${i + 1}`);
    }
    const elapsedMs = Number(process.hrtime.bigint() - started) / 1e6;
    const bundleMatch = marker.match(/TTI event=bundleLoaded timeMs=([0-9.]+)/);
    const appMatch = marker.match(/TTI event=appLoaded timeMs=([0-9.]+)/);
    console.log(JSON.stringify({
      iteration: i + 1,
      bundleLoadedMs: bundleMatch ? Number(bundleMatch[1]) : null,
      appLoadedMs: appMatch ? Number(appMatch[1]) : null,
      markerHostElapsedMs: Number(elapsedMs.toFixed(3)),
    }));
    execFileSync(adb, ['shell', 'am', 'force-stop', applicationId]);
  }
}

function sleep(milliseconds) {
  const wait = new Int32Array(new SharedArrayBuffer(4));
  Atomics.wait(wait, 0, 0, milliseconds);
}

function unzip(args) {
  try {
    return execFileSync('unzip', args, {
      encoding: args[0] === '-p' ? null : 'utf8',
      maxBuffer: 32 * 1024 * 1024,
    });
  } catch (error) {
    fail(`APK inspection requires unzip: ${error.message}`);
  }
}

function sha256(data) {
  return crypto.createHash('sha256').update(data).digest('hex');
}

function fail(message) {
  console.error(`[android] ERROR: ${message}`);
  process.exit(2);
}
