/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The Expo config plugin. It runs the same edits as the CLI, but through
 * Expo's mods, so a prebuild regenerates them rather than losing them.
 *
 *   { "expo": { "plugins": ["@react-native-quickjs/quickjs"] } }
 */

'use strict';

const fs = require('fs');
const path = require('path');
const {
  withAppDelegate,
  withDangerousMod,
  withGradleProperties,
  withMainApplication,
  withAppBuildGradle,
} = require('@expo/config-plugins');

const { edits } = require('../setup/edits');

const editFor = (label) => edits.find((edit) => edit.label === label);

function warnCouldNotEdit(label) {
  console.warn(
    `[@react-native-quickjs/quickjs] could not edit ${label}. ` +
      'Run `npx react-native-quickjs doctor` after prebuild.'
  );
}

/** Run one edit from edits.js over the contents of a string mod. */
function addQuickJS(label, source) {
  const edit = editFor(label);
  if (edit.isApplied(source)) return source;

  const edited = edit.addQuickJS(source);
  if (edited == null) {
    warnCouldNotEdit(label);
    return source;
  }
  return edited;
}

// withGradleProperties hands over parsed items rather than text, so this one
// does not go through edits.js.
function withHermesOff(config) {
  return withGradleProperties(config, (cfg) => {
    const items = cfg.modResults.filter(
      (i) => !(i.type === 'property' && i.key === 'hermesEnabled')
    );
    items.push({ type: 'property', key: 'hermesEnabled', value: 'false' });
    cfg.modResults = items;
    return cfg;
  });
}

const withNoEngineDependency = (config) =>
  withAppBuildGradle(config, (cfg) => {
    cfg.modResults.contents = addQuickJS('android/app/build.gradle', cfg.modResults.contents);
    return cfg;
  });

const withAndroidFactory = (config) =>
  withMainApplication(config, (cfg) => {
    cfg.modResults.contents = addQuickJS('MainApplication.kt', cfg.modResults.contents);
    return cfg;
  });

const withIosFactory = (config) =>
  withAppDelegate(config, (cfg) => {
    cfg.modResults.contents = addQuickJS('AppDelegate.swift', cfg.modResults.contents);
    return cfg;
  });

// There is no withPodfile, so the Podfile is edited on disk after prebuild has
// written it.
const withPodfile = (config) =>
  withDangerousMod(config, [
    'ios',
    (cfg) => {
      const file = path.join(cfg.modRequest.platformProjectRoot, 'Podfile');
      if (!fs.existsSync(file)) {
        warnCouldNotEdit('ios/Podfile');
        return cfg;
      }
      fs.writeFileSync(file, addQuickJS('ios/Podfile', fs.readFileSync(file, 'utf8')));
      return cfg;
    },
  ]);

module.exports = function withQuickJS(config) {
  return [
    withHermesOff,
    withNoEngineDependency,
    withAndroidFactory,
    withIosFactory,
    withPodfile,
  ].reduce((c, mod) => mod(c), config);
};
