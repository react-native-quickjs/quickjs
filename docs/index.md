---
layout: home
title: A QuickJS JavaScript engine for React Native
description: 'Run your React Native app on QuickJS instead of Hermes. Full Chrome DevTools debugging, smaller binaries, and an engine compiled from source in your app.'

hero:
  name: react-native-quickjs
  text: Run React Native on QuickJS.
  tagline: A drop-in JavaScript engine for React Native 0.85 and newer. One command and your app runs on QuickJS instead of Hermes.
  actions:
    - theme: brand
      text: Get started
      link: https://github.com/react-native-quickjs/quickjs#install
    - theme: alt
      text: View on GitHub
      link: https://github.com/react-native-quickjs/quickjs

features:
  - title: Fast
    details: A small, fast interpreter that starts quickly and stays light while it runs.
  - title: Small
    details: About 1 MB of compiled engine in your app.
  - title: Debugging support
    details: Chrome DevTools over React Native's own debugger — breakpoints, stepping, call stacks, variable inspection and console.
  - title: Built from source
    details: The engine compiles with your app, from the C sources in the package. Nothing prebuilt is downloaded.
---

<div style="max-width: 700px; margin: 3rem auto 0; padding: 0 1.5rem; text-align: center;">

React Native runs your JavaScript on Hermes. This replaces it with
[QuickJS](https://github.com/quickjs-ng/quickjs) — a small engine that aims at
the current ECMAScript specification — and gives React Native the runtime and
factory it needs to use it.

<div style="text-align: left; max-width: 460px; margin: 1.75rem auto;">

```sh
npm install @react-native-quickjs/quickjs@alpha
npx react-native-quickjs install
```

</div>

It is **alpha**, published under the `alpha` tag — hence the `@alpha` above.
On iOS, removing Hermes means React Native core is compiled from source, which
makes the first build slower.

[Install](https://github.com/react-native-quickjs/quickjs#install) ·
[Known limitations](https://github.com/react-native-quickjs/quickjs#known-limitations)

</div>
