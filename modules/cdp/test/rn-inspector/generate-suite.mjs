#!/usr/bin/env node
/*
 * Copies React Native's JsiIntegrationTest.cpp and points its engine list at
 * ours.
 *
 * Nothing under node_modules/ is edited. The suite names its engine adapters in
 * three places and nowhere else, so redirecting those three is the whole
 * transform -- every assertion, every test body and every helper is used
 * exactly as React Native wrote it, which is the only reason its verdict means
 * anything.
 */
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..', '..', '..');
const source = join(
  root, 'node_modules', 'react-native', 'ReactCommon',
  'jsinspector-modern', 'tests', 'JsiIntegrationTest.cpp');
const out = process.argv[2] ?? join(here, 'generated', 'JsiIntegrationTest.cpp');

const substitutions = [
  [`#include "engines/JsiIntegrationTestGenericEngineAdapter.h"\n#include "engines/JsiIntegrationTestHermesEngineAdapter.h"\n`,
   `#include "QuickJSEngineAdapter.h"\n`],
  [`using AllEngines = Types<\n    JsiIntegrationTestHermesEngineAdapter,\n    JsiIntegrationTestGenericEngineAdapter>;`,
   `using AllEngines = Types<JsiIntegrationTestQuickJSEngineAdapter>;`],
  [`using AllHermesVariants = Types<JsiIntegrationTestHermesEngineAdapter>;`,
   `using AllHermesVariants = Types<JsiIntegrationTestQuickJSEngineAdapter>;`],
];

let text = readFileSync(source, 'utf8');
for (const [from, to] of substitutions) {
  if (!text.includes(from)) {
    console.error(
      `This substitution no longer matches JsiIntegrationTest.cpp:\n${from}\n` +
      `React Native's suite has moved. Update generate-suite.mjs rather than ` +
      `editing the copy, or the copy stops being their test.`);
    process.exit(1);
  }
  text = text.replaceAll(from, to);
}

mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, text);
console.log(`wrote ${out} (${substitutions.length} substitutions)`);
