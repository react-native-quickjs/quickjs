import React from 'react'; import { Text } from 'react-native'; import { ScreenFrame, styles } from '../components';
export default function SecurityScreen() { return <ScreenFrame title="Security"><Text style={styles.badge}>Two-step verification enabled</Text><Text style={styles.body}>Review active sessions and recovery options for this workspace.</Text></ScreenFrame>; }
