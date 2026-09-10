import { NativeModules } from 'react-native';

const RNQJSTti = (NativeModules as any).RNQJSTti;

function report(event: string) {
  console.log(`RNQJSTti.report("${event}")`);
  RNQJSTti?.report?.(event);
}

export function reportBundleLoaded() {
  report('bundleLoaded');
}

export function reportAfterLayout(reported: { current: boolean }) {
  if (reported.current) return;
  reported.current = true;
  requestAnimationFrame(() => report('appLoaded'));
}
