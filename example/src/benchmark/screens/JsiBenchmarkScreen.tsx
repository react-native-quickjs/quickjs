import React, { useState } from 'react';
import { Pressable, ScrollView, Text } from 'react-native';
import { ScreenFrame, styles } from '../components';

type BenchFunction = (...args: number[]) => number;
type BenchBindings = {
  native0: BenchFunction;
  native1: BenchFunction;
  native4: BenchFunction;
  native8: BenchFunction;
};

const iterations = 10_000_000;

function runCalls(fn: BenchFunction, args: number[]) {
  switch (args.length) {
    case 0:
      for (let i = 0; i < iterations; i++) fn();
      return;
    case 1:
      for (let i = 0; i < iterations; i++) fn(args[0]);
      return;
    case 4:
      for (let i = 0; i < iterations; i++) fn(args[0], args[1], args[2], args[3]);
      return;
    case 8:
      for (let i = 0; i < iterations; i++) fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
      return;
    default: throw new Error(`unsupported arity ${args.length}`);
  }
}

function measure(name: string, fn: BenchFunction, args: number[]) {
  runCalls(fn, args);
  const start = globalThis.performance.now();
  runCalls(fn, args);
  const elapsed = globalThis.performance.now() - start;
  return { name, ns: (elapsed * 1_000_000) / iterations };
}

export default function JsiBenchmarkScreen({ back }: { back: () => void }) {
  const [output, setOutput] = useState('Tap Run to measure JSI calls.');
  const run = () => {
    const module = require('react-native').NativeModules.RNQJSJsiBench as
      | { install: () => boolean }
      | undefined;
    if (!module) {
      setOutput('JSI benchmark module is unavailable.');
      return;
    }
    if (!module.install()) {
      setOutput('JSI benchmark bindings could not be installed.');
      return;
    }
    const bench = (globalThis as typeof globalThis & { __RNQJSJsiBench?: BenchBindings }).__RNQJSJsiBench;
    if (!bench) {
      setOutput('JSI benchmark bindings are unavailable.');
      return;
    }
    const jsCall = (arity: number) => {
      const fn = (...values: number[]) => values.length + 1;
      return measure(`JS→JS ${arity}`, fn, Array.from({ length: arity }, (_, i) => i));
    };
      const rows = [
      measure('HostFunction 0', bench.native0, []),
      measure('HostFunction 1', bench.native1, [1]),
      measure('HostFunction 4', bench.native4, [1, 2, 3, 4]),
      measure('HostFunction 8', bench.native8, [1, 2, 3, 4, 5, 6, 7, 8]),
      jsCall(4),
    ];
    const text = rows.map(row => `${row.name}: ${row.ns.toFixed(1)} ns/call`).join('\n');
    console.log(`RNQJS_JSI_BENCH engine=${(globalThis as any).HermesInternal ? 'hermes' : 'quickjs'}\n${text}`);
    setOutput(text);
  };

  return <ScreenFrame title="JSI throughput" onBack={back}>
    <ScrollView>
      <Text style={styles.body}>The same JavaScript loop calls native HostFunctions under the selected engine. This runs only when requested, after startup.</Text>
      <Pressable accessibilityLabel="run-jsi-benchmark" onPress={run} style={styles.button}>
        <Text style={styles.buttonText}>Run benchmark</Text>
      </Pressable>
      <Text selectable style={[styles.body, { marginTop: 18 }]}>{output}</Text>
    </ScrollView>
  </ScreenFrame>;
}
