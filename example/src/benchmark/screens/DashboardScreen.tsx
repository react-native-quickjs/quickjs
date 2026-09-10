import React, { useMemo } from 'react';
import { ScrollView, Text, View } from 'react-native';
import { feed, tasks } from '../data';
import { FeedRow, MetricCard, ScreenFrame, SectionTitle, styles } from '../components';
import { formatCount, selectRecommended } from '../utils';

export default function DashboardScreen({ open }: { open: (route: string) => void }) {
  const recommended = useMemo(() => selectRecommended(feed), []);
  const completed = tasks.filter(task => task.done).length;
  return <ScreenFrame title="Good morning, Alex"><ScrollView showsVerticalScrollIndicator={false}>
    <View style={styles.hero}><Text style={styles.heroTitle}>Your week at a glance</Text><Text style={styles.heroText}>A focused collection of ideas, people, and tasks selected from your local workspace.</Text></View>
    <View style={styles.metrics}><MetricCard label="Recommended" value={formatCount(recommended.length)} hint="for you" /><MetricCard label="Saved" value={formatCount(feed.filter(item => item.saved).length)} hint="to revisit" /><MetricCard label="Progress" value={`${completed}/${tasks.length}`} hint="tasks done" /></View>
    <SectionTitle>Continue exploring</SectionTitle>{recommended.slice(0, 4).map(item => <FeedRow key={item.id} item={item} onPress={() => open('article')} />)}
    <SectionTitle>Quick access</SectionTitle>{['feed', 'search', 'profile', 'settings'].map(route => <Text key={route} style={styles.plain} onPress={() => open(route)}>{route[0].toUpperCase() + route.slice(1)} ›</Text>)}
  </ScrollView></ScreenFrame>;
}
