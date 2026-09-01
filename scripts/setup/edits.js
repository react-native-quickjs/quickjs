/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The build-config edits that move an app between Hermes and QuickJS, written
 * once. `install` applies them, `revert` undoes them, `doctor` reports them,
 * and the Expo plugin runs the same `apply` inside Expo's own mods.
 *
 * `apply` returns null when it cannot find what it expects. That is not a
 * failure to report as an error -- the file has been customised, and the
 * caller prints `manual` instead.
 */

'use strict';

const fs = require('fs');
const path = require('path');

const PKG = '@react-native-quickjs/quickjs';
const KOTLIN_IMPORT = `import com.reactnativequickjs.quickjs.QuickJSInstance`;
const SWIFT_IMPORT = 'import ReactNativeQuickJS';
const PODS_REQUIRE = `require_relative '../node_modules/${PKG}/scripts/react_native_quickjs_pods.rb'`;

/** First file matching `name` under `dir`, breadth-first. */
function findUnder(dir, name, depth = 7) {
  if (depth < 0 || !fs.existsSync(dir)) return null;
  let dirs = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.name === 'build' || entry.name === 'node_modules') continue;
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) dirs.push(full);
    else if (entry.name === name) return full;
  }
  for (const d of dirs) {
    const hit = findUnder(d, name, depth - 1);
    if (hit) return hit;
  }
  return null;
}

/** Index just past the `)` closing the call whose `(` is at `open`. */
function endOfCall(source, open) {
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '(') depth++;
    else if (source[i] === ')' && --depth === 0) return i;
  }
  return -1;
}

/** Insert `line` after the last `import` in a Kotlin or Swift file. */
function afterImports(source, line) {
  if (source.includes(line)) return source;
  const imports = [...source.matchAll(/^import .*$/gm)];
  if (imports.length === 0) return null;
  const last = imports[imports.length - 1];
  const at = last.index + last[0].length;
  return source.slice(0, at) + '\n' + line + source.slice(at);
}

const edits = [
  {
    label: 'android/gradle.properties',
    locate: (root) => path.join(root, 'android', 'gradle.properties'),
    done: (s) => /^[ \t]*hermesEnabled[ \t]*=[ \t]*false[ \t]*$/m.test(s),
    apply: (s) =>
      /^[ \t]*hermesEnabled[ \t]*=/m.test(s)
        ? s.replace(/^([ \t]*hermesEnabled[ \t]*=[ \t]*).*$/m, '$1false')
        : `${s.replace(/\s*$/, '')}\n\nhermesEnabled=false\n`,
    revert: (s) => s.replace(/^([ \t]*hermesEnabled[ \t]*=[ \t]*).*$/m, '$1true'),
    manual: ['Set hermesEnabled=false in android/gradle.properties'],
  },

  {
    // The template selects Hermes or JavaScriptCore here. An app on QuickJS
    // needs neither, and the JavaScriptCore branch is the one hermesEnabled=false
    // would otherwise take.
    label: 'android/app/build.gradle',
    locate: (root) => path.join(root, 'android', 'app', 'build.gradle'),
    done: (s) => !/hermes-android|jscFlavor/.test(s),
    apply(s) {
      const out = s
        .replace(
          /[ \t]*if \([ \t]*hermesEnabled\.toBoolean\(\)[ \t]*\) \{[\s\S]*?\n[ \t]*\}[ \t]*\n/,
          ''
        )
        .replace(/[ \t]*def jscFlavor[ \t]*=.*\n/, '');
      return out === s ? null : out;
    },
    revert(s) {
      const anchor = /([ \t]*)implementation\("com\.facebook\.react:react-android"\)\n/;
      if (!anchor.test(s)) return null;
      return s.replace(
        anchor,
        (line, indent) =>
          `${line}\n${indent}if (hermesEnabled.toBoolean()) {\n` +
          `${indent}    implementation("com.facebook.react:hermes-android")\n` +
          `${indent}} else {\n` +
          `${indent}    implementation jscFlavor\n` +
          `${indent}}\n`
      );
    },
    manual: [
      'In android/app/build.gradle, delete the dependencies block that picks',
      'between com.facebook.react:hermes-android and jscFlavor.',
    ],
  },

  {
    label: 'MainApplication.kt',
    locate: (root) => findUnder(path.join(root, 'android', 'app', 'src'), 'MainApplication.kt'),
    done: (s) => s.includes('QuickJSInstance('),
    apply(s) {
      const withImport = afterImports(s, KOTLIN_IMPORT);
      if (!withImport) return null;
      const open = /getDefaultReactHost\(\n/.exec(withImport);
      if (!open) return null;
      // Added as the first argument, not the last: Kotlin named arguments may
      // be given in any order, and the first one's indent is the only one that
      // can be read without matching parens through a nested lambda.
      const at = open.index + open[0].length;
      const indent = (/^([ \t]+)\S/.exec(withImport.slice(at)) || [, '      '])[1];
      return (
        withImport.slice(0, at) +
        `${indent}jsRuntimeFactory = QuickJSInstance(),\n` +
        withImport.slice(at)
      );
    },
    revert: (s) =>
      s
        .replace(new RegExp(`^${KOTLIN_IMPORT}\\n`, 'm'), '')
        .replace(/^[ \t]*jsRuntimeFactory = QuickJSInstance\(\),?[ \t]*\n/m, ''),
    manual: [
      'In MainApplication.kt add:',
      `  ${KOTLIN_IMPORT}`,
      'and pass jsRuntimeFactory = QuickJSInstance() to getDefaultReactHost().',
    ],
  },

  {
    label: 'ios/Podfile',
    locate: (root) => path.join(root, 'ios', 'Podfile'),
    done: (s) => s.includes('use_quickjs!'),
    apply(s) {
      if (!/^[ \t]*use_react_native!\(/m.test(s)) return null;
      let out = s.includes(PODS_REQUIRE)
        ? s
        : s.replace(/^(platform :ios)/m, `${PODS_REQUIRE}\n\n$1`);
      out = out.replace(
        /^([ \t]*)(use_react_native!\()/m,
        `$1use_quickjs!\n\n$1$2`
      );
      // After React Native's own hook, which writes the value this overwrites.
      const rn = out.indexOf('react_native_post_install(');
      if (rn === -1) return null;
      const close = endOfCall(out, out.indexOf('(', rn));
      if (close === -1) return null;
      const eol = out.indexOf('\n', close);
      const indent = (out.slice(0, rn).match(/\n([ \t]*)$/) || [, '    '])[1];
      return (
        out.slice(0, eol + 1) +
        `\n${indent}react_native_quickjs_post_install(installer)\n` +
        out.slice(eol + 1)
      );
    },
    revert: (s) =>
      s
        .replace(new RegExp(`^${PODS_REQUIRE.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\n\\n?`, 'm'), '')
        .replace(/^[ \t]*use_quickjs!\n\n?/m, '')
        .replace(/\n?[ \t]*react_native_quickjs_post_install\(installer\)\n/, '\n'),
    manual: [
      'In ios/Podfile add, before use_react_native!:',
      `  ${PODS_REQUIRE}`,
      '  use_quickjs!',
      'and react_native_quickjs_post_install(installer) after react_native_post_install.',
    ],
  },

  {
    label: 'AppDelegate.swift',
    locate: (root) => findUnder(path.join(root, 'ios'), 'AppDelegate.swift'),
    done: (s) => s.includes('jsrt_create_quickjs_factory'),
    apply(s) {
      const withImport = afterImports(s, SWIFT_IMPORT);
      if (!withImport) return null;
      const cls = /class\s+\w+\s*:\s*RCTDefaultReactNativeFactoryDelegate\s*\{/.exec(withImport);
      if (!cls) return null;
      const at = cls.index + cls[0].length;
      return (
        withImport.slice(0, at) +
        '\n  override func createJSRuntimeFactory() -> JSRuntimeFactoryRef {\n' +
        '    jsrt_create_quickjs_factory()\n' +
        '  }\n' +
        withImport.slice(at)
      );
    },
    revert: (s) =>
      s
        .replace(new RegExp(`^${SWIFT_IMPORT}\\n`, 'm'), '')
        .replace(
          /\n[ \t]*override func createJSRuntimeFactory\(\)[^\n]*\{\n[ \t]*jsrt_create_quickjs_factory\(\)\n[ \t]*\}\n/,
          '\n'
        ),
    manual: [
      'In AppDelegate.swift add:',
      `  ${SWIFT_IMPORT}`,
      'and override createJSRuntimeFactory() to return jsrt_create_quickjs_factory().',
    ],
  },
];

module.exports = { edits, PKG, KOTLIN_IMPORT, SWIFT_IMPORT, PODS_REQUIRE, findUnder };
