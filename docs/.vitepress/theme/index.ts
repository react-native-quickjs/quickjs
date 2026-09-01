import {h} from 'vue';
import type {Theme} from 'vitepress';
import DefaultTheme from 'vitepress/theme';
import HeroLogo from './components/HeroLogo.vue';
import './custom.css';

// The default home layout only renders the hero image column when either
// `hero.image` is set in frontmatter or this slot has content, so filling the
// slot is what puts the mark on the page.
export default {
  extends: DefaultTheme,
  Layout: () =>
    h(DefaultTheme.Layout, null, {
      'home-hero-image': () => h(HeroLogo),
    }),
} satisfies Theme;
