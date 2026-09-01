import { defineConfig } from 'vitepress'

const GITHUB = 'https://github.com/react-native-quickjs/quickjs'

// Served from a project GitHub Pages path, so every internal link carries
// `/quickjs/`.
export default defineConfig({
  title: 'react-native-quickjs',
  description: 'A QuickJS JSI runtime for React Native',
  lang: 'en-US',

  base: '/quickjs/',
  cleanUrls: true,
  // A dangling cross-reference fails the build rather than shipping. There is
  // one page for now, so every link here goes to GitHub.
  ignoreDeadLinks: false,

  // `base` is not applied to `head`, so these carry it themselves. Browsers
  // that understand an SVG icon take it and ignore the .ico; the rest fall
  // back.
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/quickjs/logo.svg' }],
    ['link', { rel: 'icon', href: '/quickjs/favicon.ico', sizes: '32x32' }],
    ['link', { rel: 'apple-touch-icon', href: '/quickjs/logo-180.png' }],
    [
      'meta',
      {
        property: 'og:image',
        content: 'https://react-native-quickjs.github.io/quickjs/logo-512.png',
      },
    ],
  ],

  themeConfig: {
    // Unlike `head`, theme paths are resolved against `base` for us.
    logo: '/logo.svg',
    siteTitle: 'react-native-quickjs',
    socialLinks: [{ icon: 'github', link: GITHUB }],
    footer: {
      message: 'MIT licensed. QuickJS-ng is MIT, © Fabrice Bellard and Charlie Gordon.',
      copyright: `<a href="${GITHUB}">react-native-quickjs/quickjs</a>`,
    },
  },
})
