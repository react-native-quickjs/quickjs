import React, { PropsWithChildren } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { FeedItem } from './data';
import { formatMinutes, formatScore } from './utils';

export function ScreenFrame({ title, children, onBack }: PropsWithChildren<{ title: string; onBack?: () => void }>) {
  return <View style={styles.screen}>
    <View style={styles.header}>{onBack && <Pressable onPress={onBack}><Text style={styles.back}>‹</Text></Pressable>}<Text style={styles.heading}>{title}</Text></View>
    {children}
  </View>;
}

export function MetricCard({ label, value, hint }: { label: string; value: string; hint: string }) {
  return <View style={styles.metric}><Text style={styles.metricValue}>{value}</Text><Text style={styles.metricLabel}>{label}</Text><Text style={styles.hint}>{hint}</Text></View>;
}

export function FeedRow({ item, onPress }: { item: FeedItem; onPress?: () => void }) {
  return <Pressable style={styles.row} onPress={onPress}><View style={styles.rowTop}><Text style={styles.category}>{item.category}</Text><Text style={styles.score}>{formatScore(item.score)}</Text></View><Text style={styles.rowTitle}>{item.title}</Text><Text style={styles.muted}>{item.author} · {formatMinutes(item.minutes)}</Text></Pressable>;
}

export function SectionTitle({ children }: PropsWithChildren) { return <Text style={styles.section}>{children}</Text>; }

export const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: '#f6f7fb', padding: 20 }, header: { flexDirection: 'row', alignItems: 'center', marginBottom: 18 }, heading: { color: '#172033', fontSize: 25, fontWeight: '700' }, back: { color: '#4567d8', fontSize: 34, marginRight: 10 },
  hero: { backgroundColor: '#263b78', borderRadius: 18, padding: 20, marginBottom: 18 }, heroTitle: { color: '#fff', fontSize: 22, fontWeight: '700' }, heroText: { color: '#dbe4ff', marginTop: 8, lineHeight: 20 }, metrics: { flexDirection: 'row', gap: 10, marginBottom: 18 }, metric: { flex: 1, backgroundColor: '#fff', borderRadius: 14, padding: 12 }, metricValue: { color: '#172033', fontWeight: '700', fontSize: 21 }, metricLabel: { color: '#41506b', marginTop: 4, fontSize: 12 }, hint: { color: '#6e7b92', marginTop: 5, fontSize: 11 }, section: { color: '#172033', fontSize: 17, fontWeight: '700', marginBottom: 8, marginTop: 4 }, row: { backgroundColor: '#fff', borderRadius: 13, padding: 14, marginBottom: 9 }, rowTop: { flexDirection: 'row', justifyContent: 'space-between' }, category: { color: '#4567d8', fontSize: 12, fontWeight: '700' }, score: { color: '#2d8a62', fontSize: 12 }, rowTitle: { color: '#172033', fontSize: 16, fontWeight: '600', marginTop: 6 }, muted: { color: '#6e7b92', marginTop: 6, fontSize: 12 }, search: { backgroundColor: '#fff', borderRadius: 12, padding: 14, color: '#172033', marginBottom: 14 }, button: { backgroundColor: '#4567d8', borderRadius: 10, padding: 12, marginTop: 8 }, buttonText: { color: '#fff', textAlign: 'center', fontWeight: '700' }, plain: { color: '#4567d8', paddingVertical: 10, fontWeight: '600' }, body: { color: '#41506b', fontSize: 15, lineHeight: 22, marginBottom: 12 }, field: { backgroundColor: '#fff', borderRadius: 10, padding: 13, marginBottom: 10, color: '#172033' }, badge: { backgroundColor: '#e4eaff', color: '#4567d8', padding: 8, borderRadius: 8, overflow: 'hidden', marginBottom: 8 },
});
