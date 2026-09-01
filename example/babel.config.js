/*
 * Babel configuration for the example app.
 *
 * WHY THIS IS NOT `react-native-builder-bob/babel-config`
 *
 * It used to be, and it threw on every bundle:
 *
 *   error index.js: Couldn't determine the source directory.
 *                   Does your config specify a 'source' field?
 *   at getConfig (node_modules/react-native-builder-bob/configs/babel-config.js:28)
 *
 * `getConfig` reads `pkg['react-native-builder-bob'].source` from the root
 * package.json to alias the library's TypeScript sources for the example. The
 * root package.json of this repository has no `react-native-builder-bob` key at
 * all, and correctly so: `@react-native-quickjs/quickjs` ships no JavaScript.
 * Its `files` list is android/, apple/, common/, cmake/, vendor/quickjs-ng and
 * the podspecs -- it is a pure native package with no `main`, no `module` and no
 * src/. There is no source directory for bob to alias, so the helper cannot
 * succeed and the plain React Native preset is the correct configuration.
 *
 * WHAT THIS BLOCKED (MEASURED, 2026-07)
 *
 * Everything that runs Metro over the example app:
 *   ./gradlew :app:assembleRelease  ->  createBundleReleaseJsAndAssets fails,
 *                                       "command 'node' finished with non-zero
 *                                       exit value 1"
 *   npx react-native bundle          ->  the error above
 *
 * `assembleDebug` succeeded throughout, because a debug APK contains no bundle
 * and fetches JS from Metro at run time. That is why a broken bundler config
 * survived: the only build anyone ran was the one that does not bundle.
 *
 * Module resolution for the example still comes from metro.config.js, which
 * uses `react-native-monorepo-config` and the
 * `ammarahmed-react-native-quickjs-source` export condition. That is unaffected
 * by this file.
 */
module.exports = {
  presets: ['module:@react-native/babel-preset'],
};
