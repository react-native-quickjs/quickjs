import { FeedItem, accounts } from './data';

export const formatMinutes = (minutes: number) => `${minutes} min read`;
export const formatScore = (score: number) => `${score}% match`;
export const formatCount = (count: number) => String(count);

export function selectRecommended(items: FeedItem[]) {
  return items.filter(item => item.score >= 80).sort((a, b) => b.score - a.score);
}

export function selectSaved(items: FeedItem[]) {
  return items.filter(item => item.saved);
}

export function searchItems(items: FeedItem[], query: string) {
  const needle = query.trim().toLowerCase();
  if (!needle) return items;
  return items.filter(item => `${item.title} ${item.category} ${item.author}`.toLowerCase().includes(needle));
}

export function accountFor(name: string) {
  return accounts.find(account => account.name === name) ?? accounts[0];
}

export function validateProfile(name: string, email: string) {
  return name.trim().length >= 2 && /^\S+@\S+\.\S+$/.test(email);
}
