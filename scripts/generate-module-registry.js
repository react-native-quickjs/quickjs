#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Emits a C++ file that calls each autolinked QuickJS module's install
 * function by name.
 *
 * Naming the function is what stops the linker dropping a static library's
 * object file, which would otherwise leave the module silently uninstalled.
 *
 *   node scripts/generate-module-registry.js [--dir <pkg> ...] [--out <file>]
 */

const fs = require('node:fs');
const path = require('node:path');

const argv = process.argv.slice(2);
const opt = (name, dflt = null) => {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
};

if (argv.includes('--help') || argv.includes('-h')) {
  console.log(`
generate-module-registry — autolink QuickJS modules

  node scripts/generate-module-registry.js [options]

Options
  --root <dir>   project root to scan from (default: cwd)
  --out <file>   output path (default: <root>/build/QuickJSGeneratedModules.cpp)
  --dir <dir>    an explicit module package directory (repeatable). When given,
                 the registry is exactly these modules -- authoritative, for an
                 app build that must only reference linked modules.
  --only <names> keep only these discovered modules (comma-separated names)
  --json         print the discovered modules as JSON and write nothing
  --quiet        suppress the summary`);
  process.exit(0);
}

const root = path.resolve(opt('root', process.cwd()));
const outPath = path.resolve(
  opt('out', path.join(root, 'build', 'QuickJSGeneratedModules.cpp'))
);
const only = opt('only', null) ? new Set(opt('only').split(',')) : null;
const asJson = argv.includes('--json');
const quiet = argv.includes('--quiet');

/**
 * Directories to scan. Monorepos hoist to the root `node_modules`, and this
 * repository keeps its first-party modules in `modules/`, uninstalled.
 */
function candidateDirs() {
  const dirs = [];
  const nodeModules = path.join(root, 'node_modules');
  if (fs.existsSync(nodeModules)) dirs.push(nodeModules);

  const modules = path.join(root, 'modules');
  if (fs.existsSync(modules)) dirs.push(modules);

  return dirs;
}

/** Reads a package.json, or null when it is missing or invalid. */
function readPackage(dir) {
  try {
    const raw = fs.readFileSync(path.join(dir, 'package.json'), 'utf8');
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

function moduleFromDir(dir) {
  const pkg = readPackage(dir);
  const spec = pkg && pkg.reactNativeQuickJSModule;
  if (!spec || !spec.install) return null;
  return {
    name: pkg.name || path.basename(dir),
    install: spec.install,
    header: spec.header || null,
    priority: typeof spec.priority === 'number' ? spec.priority : 0,
    dir,
  };
}

function discover() {
  const found = new Map(); // name -> { name, install, header, dir }

  for (const base of candidateDirs()) {
    let entries;
    try {
      entries = fs.readdirSync(base, { withFileTypes: true });
    } catch {
      continue;
    }

    for (const entry of entries) {
      if (!entry.isDirectory() && !entry.isSymbolicLink()) continue;

    // Scoped packages nest one level deeper: node_modules/@scope/name.
    const dirs = entry.name.startsWith('@')
        ? fs
            .readdirSync(path.join(base, entry.name), { withFileTypes: true })
            .filter((e) => e.isDirectory() || e.isSymbolicLink())
            .map((e) => path.join(base, entry.name, e.name))
        : [path.join(base, entry.name)];

      for (const dir of dirs) {
        const m = moduleFromDir(dir);
        if (m && !found.has(m.name)) found.set(m.name, m); // first wins
      }
    }
  }

  // Deterministic output, so an unchanged build does not retrigger everything
  // downstream of the registry.
  return [...found.values()].sort(
    (a, b) => a.priority - b.priority || a.name.localeCompare(b.name)
  );
}

// --dir is authoritative: the registry becomes exactly these modules, which is
// what an app build wants. node_modules holds packages that are present but not
// linked (transitive dependencies), and referencing their symbols would fail
// the link.
const explicitDirs = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--dir' && argv[i + 1]) explicitDirs.push(path.resolve(argv[i + 1]));
}

let modules;
if (explicitDirs.length > 0) {
  const seen = new Map();
  for (const dir of explicitDirs) {
    const m = moduleFromDir(dir);
    if (m && !seen.has(m.name)) seen.set(m.name, m);
  }
  modules = [...seen.values()].sort(
    (a, b) => a.priority - b.priority || a.name.localeCompare(b.name)
  );
} else {
  modules = discover();
  if (only) {
    // An allow-list over the discovered modules.
    modules = modules.filter((m) => only.has(m.name));
  }
}

if (asJson) {
  console.log(JSON.stringify(modules, null, 2));
  process.exit(0);
}

const isValidIdentifier = (s) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(s);

for (const m of modules) {
  if (!isValidIdentifier(m.install)) {
    console.error(
      `[generate-module-registry] ${m.name}: reactNativeQuickJSModule.install ` +
        `must be a C identifier, got ${JSON.stringify(m.install)}`
    );
    process.exit(1);
  }
}

const lines = [];
lines.push('// Generated by scripts/generate-module-registry.js — do not edit.');
lines.push('');
lines.push('#include <jsi/jsi.h>');
lines.push('');
lines.push('#include "QuickJSModule.h"');
lines.push('');

if (modules.length === 0) {
  lines.push('// No autolinked modules were found.');
  lines.push('');
}

lines.push('extern "C" {');
for (const m of modules) {
  lines.push(`// ${m.name}`);
  lines.push(`void ${m.install}(facebook::jsi::Runtime &);`);
}
lines.push('}');
lines.push('');
lines.push('namespace qjs {');
lines.push('');
lines.push('void registerGeneratedModules() {');
if (modules.length === 0) {
  lines.push('  // Nothing to register.');
}
for (const m of modules) {
  lines.push(`  registerModule("${m.name}", ${m.install}, ${m.priority});`);
}
lines.push('}');
lines.push('');
lines.push('} // namespace qjs');
lines.push('');

const output = lines.join('\n');

fs.mkdirSync(path.dirname(outPath), { recursive: true });

// Only write when the content differs, so an unchanged registry does not
// retrigger a rebuild of everything downstream of it.
let existing = null;
try {
  existing = fs.readFileSync(outPath, 'utf8');
} catch {
  /* first run */
}

if (existing !== output) {
  fs.writeFileSync(outPath, output);
}

if (!quiet) {
  if (modules.length === 0) {
    console.log('[generate-module-registry] no QuickJS modules found');
  } else {
    console.log(
      `[generate-module-registry] ${modules.length} module(s) -> ${path.relative(root, outPath)}`
    );
    for (const m of modules) {
      console.log(`  ${m.name} -> ${m.install}()`);
    }
  }
}
