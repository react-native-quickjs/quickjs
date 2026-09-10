export type FeedItem = {
  id: string;
  title: string;
  category: string;
  author: string;
  minutes: number;
  saved: boolean;
  score: number;
};

export const accounts = [
  { id: 'a1', name: 'Mina Patel', role: 'Product design', initials: 'MP' },
  { id: 'a2', name: 'Jon Bell', role: 'Engineering', initials: 'JB' },
  { id: 'a3', name: 'Aya Chen', role: 'Research', initials: 'AC' },
  { id: 'a4', name: 'Luis Mora', role: 'Operations', initials: 'LM' },
];

export const feed: FeedItem[] = [
  { id: 'f1', title: 'Designing calmer notification flows', category: 'Design', author: 'Mina Patel', minutes: 6, saved: true, score: 94 },
  { id: 'f2', title: 'A practical guide to offline-first state', category: 'Engineering', author: 'Jon Bell', minutes: 9, saved: false, score: 91 },
  { id: 'f3', title: 'What makes a useful dashboard?', category: 'Product', author: 'Aya Chen', minutes: 5, saved: true, score: 87 },
  { id: 'f4', title: 'Small rituals for focused teams', category: 'Culture', author: 'Luis Mora', minutes: 4, saved: false, score: 82 },
  { id: 'f5', title: 'Measuring quality without vanity metrics', category: 'Research', author: 'Aya Chen', minutes: 8, saved: false, score: 79 },
  { id: 'f6', title: 'A field guide to useful empty states', category: 'Design', author: 'Mina Patel', minutes: 7, saved: true, score: 76 },
  { id: 'f7', title: 'Keeping forms friendly and resilient', category: 'Product', author: 'Jon Bell', minutes: 6, saved: false, score: 73 },
  { id: 'f8', title: 'The case for deliberate defaults', category: 'Operations', author: 'Luis Mora', minutes: 3, saved: false, score: 68 },
];

export const tasks = [
  { id: 't1', label: 'Review the onboarding notes', done: true },
  { id: 't2', label: 'Share the dashboard draft', done: false },
  { id: 't3', label: 'Schedule user interviews', done: false },
];

export const notifications = [
  { id: 'n1', title: 'Mina mentioned you', detail: 'in Design review', unread: true },
  { id: 'n2', title: 'Your saved item is updated', detail: 'Offline-first state', unread: true },
  { id: 'n3', title: 'Weekly digest is ready', detail: '8 items matched your interests', unread: false },
];

export const navItems = [
  ['feed', 'Feed'], ['search', 'Search'], ['saved', 'Saved'], ['notifications', 'Notifications'],
  ['profile', 'Profile'], ['settings', 'Settings'], ['analytics', 'Analytics'], ['help', 'Help'],
  ['orders', 'Orders'], ['messages', 'Messages'],
] as const;
