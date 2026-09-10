import React from 'react'; import { ScrollView, Text } from 'react-native'; import { ScreenFrame, styles } from '../components';
export default function SettingsScreen() { return <ScreenFrame title="Settings"><ScrollView>{['Account', 'Notifications', 'Privacy', 'Appearance', 'Offline storage'].map(value => <Text key={value} style={styles.rowTitle}>{value} ›</Text>)}</ScrollView></ScreenFrame>; }
