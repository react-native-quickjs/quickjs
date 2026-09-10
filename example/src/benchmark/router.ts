import React from 'react';

export type Route = 'dashboard' | 'feed' | 'search' | 'article' | 'profile' | 'settings' | 'editProfile' | 'notifications' | 'saved' | 'analytics' | 'help' | 'orders' | 'orderDetails' | 'messages' | 'messageDetails' | 'security' | 'preferences';
type Screen = React.ComponentType<any>;

// These static factories keep every screen discoverable to Metro while avoiding
// execution of noninitial screen modules during dashboard startup.
export const screenFactories: Record<Route, () => Screen> = {
  dashboard: () => require('./screens/DashboardScreen').default,
  feed: () => require('./screens/FeedScreen').default,
  search: () => require('./screens/SearchScreen').default,
  article: () => require('./screens/ArticleDetailsScreen').default,
  profile: () => require('./screens/ProfileScreen').default,
  settings: () => require('./screens/SettingsScreen').default,
  editProfile: () => require('./screens/EditProfileScreen').default,
  notifications: () => require('./screens/NotificationsScreen').default,
  saved: () => require('./screens/SavedScreen').default,
  analytics: () => require('./screens/AnalyticsScreen').default,
  help: () => require('./screens/HelpScreen').default,
  orders: () => require('./screens/OrdersScreen').default,
  orderDetails: () => require('./screens/OrderDetailsScreen').default,
  messages: () => require('./screens/MessagesScreen').default,
  messageDetails: () => require('./screens/MessageDetailsScreen').default,
  security: () => require('./screens/SecurityScreen').default,
  preferences: () => require('./screens/PreferencesScreen').default,
};
