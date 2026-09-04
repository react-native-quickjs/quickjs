import { Text, View, StyleSheet } from 'react-native';

const props =
  (globalThis as any).HermesInternal?.getRuntimeProperties?.() ?? {};

function intlProbe(): string[] {
  try {
    const backend = (Intl as any).__rnqjsBackend?.() ?? 'missing';
    const nf = new Intl.NumberFormat('de-DE').format(1234567.89);
    const dtf = new Intl.DateTimeFormat('de-DE', {
      dateStyle: 'full',
      timeZone: 'Europe/Berlin',
    }).format(new Date('2024-05-17T12:00:00Z'));
    const locale = new Intl.NumberFormat('de-DE').resolvedOptions().locale;
    console.log(`INTL_PROBE backend=${backend}`);
    console.log(`INTL_PROBE de-DE number=${nf}`);
    console.log(`INTL_PROBE de-DE date=${dtf}`);
    console.log(`INTL_PROBE locale=${locale}`);
    return [`intl backend: ${backend}`, `de-DE: ${nf}`, `date: ${dtf}`];
  } catch (e: any) {
    console.log(`INTL_PROBE error=${e?.message ?? String(e)}`);
    return [`intl error: ${e?.message ?? String(e)}`];
  }
}

declare class TE_ {
  encode(input?: string): Uint8Array;
}
declare class TD_ {
  decode(input?: Uint8Array, options?: { stream?: boolean }): string;
}

function encodingProbe(): string[] {
  try {
    const TE: typeof TE_ = (globalThis as any).TextEncoder;
    const TD: typeof TD_ = (globalThis as any).TextDecoder;
    const encoded = Array.from(new TE().encode('héllo'));
    const decoded = new TD().decode(new Uint8Array([104, 195, 169]));
    console.log(`ENC_PROBE encoded=${JSON.stringify(encoded)}`);
    console.log(`ENC_PROBE decoded=${decoded}`);
    return [`utf-8: ${JSON.stringify(encoded)}`, `round-trip: ${decoded}`];
  } catch (e: any) {
    console.log(`ENC_PROBE error=${e?.message ?? String(e)}`);
    return [`encoding error: ${e?.message ?? String(e)}`];
  }
}

export default function App() {
  const lines = [...intlProbe(), ...encodingProbe()];
  return (
    <View style={styles.container}>
      <Text style={styles.title}>react-native-quickjs</Text>
      <Text testID="engine">engine: {props.Engine ?? 'unknown'}</Text>
      <Text testID="math">2 ** 40 = {String(2 ** 40)}</Text>
      {lines.map((l, i) => (
        <Text key={i} testID={i === 0 ? 'intl' : undefined}>
          {l}
        </Text>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8 },
  title: { fontWeight: '600' },
});
