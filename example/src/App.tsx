import { NativeModules, Text, View, StyleSheet } from 'react-native';
import { useRef } from 'react';
import { reportAfterLayout, reportBundleLoaded } from './startupTiming';

const engine =
  (NativeModules as any).RNQJSTti?.engine === 'hermes' ? 'Hermes' : 'QuickJS';
reportBundleLoaded();

export default function App() {
  const reported = useRef(false);
  return (
    <View
      style={styles.container}
      accessibilityLabel="rnqjs-ready"
      onLayout={() => reportAfterLayout(reported)}>
      <Text style={styles.title}>react-native-quickjs</Text>
      <Text testID="engine">engine: {engine}</Text>
      <Text testID="math">2 ** 40 = {String(2 ** 40)}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8, backgroundColor: 'white' },
  title: { fontWeight: '600' },
});
