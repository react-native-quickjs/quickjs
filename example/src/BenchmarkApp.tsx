import React, { useRef, useState } from 'react';
import { Pressable, SafeAreaView, Text, View } from 'react-native';
import DashboardScreen from './benchmark/screens/DashboardScreen';
import { Route, screenFactories } from './benchmark/router';
import { reportAfterLayout, reportBundleLoaded } from './startupTiming';
import { styles } from './benchmark/components';

reportBundleLoaded();

export default function BenchmarkApp() {
  const [route, setRoute] = useState<Route>('dashboard');
  const reported = useRef(false);
  const Screen = route === 'dashboard' ? DashboardScreen : screenFactories[route]();
  const goBack = () => setRoute('dashboard');
  return <SafeAreaView style={{ flex: 1 }}>
    <View accessibilityLabel="rnqjs-ready" onLayout={() => reportAfterLayout(reported)} style={{ flex: 1 }}>
      <Screen open={(next: string) => setRoute(next as Route)} back={goBack} />
    </View>
    {route !== 'dashboard' && <Pressable accessibilityLabel="benchmark-home" onPress={goBack} style={styles.button}><Text style={styles.buttonText}>Dashboard</Text></Pressable>}
  </SafeAreaView>;
}
