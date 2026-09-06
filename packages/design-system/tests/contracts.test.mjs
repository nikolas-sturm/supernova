import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { test } from 'node:test'

const read = (relative) => readFileSync(new URL(relative, import.meta.url), 'utf8')
const terra = '../../../apps/terra/src/'
const progenitor = '../../../apps/progenitor/src_assets/common/assets/web/src/'

test('every original palette remains available under its persisted selector', () => {
  const themes = read('../src/themes.css')
  const catalog = read('../src/theme.ts').split('} as const')[0]
  const names = [...catalog.matchAll(/^\s+'([a-z-]+)',/gm)].map((match) => match[1])
  assert.equal(names.length, 21)
  assert.equal(new Set(names).size, 21)
  for (const name of names) assert.ok(themes.includes(`[data-theme="${name}"]`), name)
  assert.match(themes, /:root:not\(\[data-theme\]\),\s*\[data-theme="dark"\]/)
})

test('all static Terra component classes survive the stylesheet replacement', () => {
  const css = read(`${terra}App.module.css`)
  const classes = new Set([...css.matchAll(/\.([a-zA-Z_][\w-]*)/g)].map((match) => match[1]))
  for (const file of ['App.tsx', 'SettingsView.tsx']) {
    for (const match of read(`${terra}${file}`).matchAll(/styles\.([a-zA-Z_][\w]*)/g)) {
      assert.ok(classes.has(match[1]), `${file}: missing .${match[1]}`)
    }
  }
})

test('main client CSS uses defined shared semantic tokens rather than old aliases', () => {
  const css = read(`${terra}App.module.css`)
  const shared = read('../src/tokens.css') + read('../src/themes.css')
  const defined = new Set([...shared.matchAll(/(--[\w-]+)\s*:/g)].map((match) => match[1]))
  for (const match of css.matchAll(/var\((--[\w-]+)/g)) assert.ok(defined.has(match[1]), match[1])
  assert.doesNotMatch(css, /--(?:acid|violet|ink|canvas|accent|secondary|muted|line)(?:[):;]|-rgb)/)
})

test('both app entrypoints load the shared stylesheet and theme initialization', () => {
  for (const app of [terra, progenitor]) {
    const main = read(`${app}main.tsx`)
    assert.match(main, /@supernova\/design-system\/styles\.css/)
    assert.match(main, /useThemeStore\.getState\(\)\.initialize\(\)/)
  }
})

test('capture-window transparency stays outside the shared reset', () => {
  const global = read(`${terra}styles/global.css`)
  assert.match(global, /html\.stream-overlay-window,/)
  assert.match(global, /html\.stream-overlay-window body,/)
  assert.match(global, /html\.stream-overlay-window #root\s*\{[^}]*background: transparent;/)
  assert.match(read(`${terra}main.tsx`), /if \(!overlayWindow\) useThemeStore/)
  assert.doesNotMatch(read('../src/base.css'), /body\s*\{[^}]*background:/)
})
