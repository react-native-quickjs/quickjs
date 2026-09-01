import { Text, View, StyleSheet } from 'react-native';

const props =
  (globalThis as any).HermesInternal?.getRuntimeProperties?.() ?? {};

export default function App() {
  return (
    <View style={styles.container}>
      <Text style={styles.title}>react-native-quickjs</Text>
      <Text testID="engine">engine: {props.Engine ?? 'unknown'}</Text>
      <Text testID="math">2 ** 40 = {String(2 ** 40)}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8 },
  title: { fontWeight: '600' },
});
