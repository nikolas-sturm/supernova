# Terra Design System

Shared frontend source for Sol host administration and Terra streaming client.
Consumers compile TypeScript/TSX and CSS Modules through their existing Vite pipelines.
No separate package build or runtime stylesheet injection is required.

## Consumption

Both app manifests need `"@supernova/design-system": "0.1.0"`; the root workspace
must include `packages/*`. Existing React 19.2 and Zustand 5 satisfy peer dependencies.

```tsx
import '@supernova/design-system/styles.css'
import { Button, TerraBrand, ThemePicker } from '@supernova/design-system'
import { useThemeStore } from '@supernova/design-system/theme'

const dispose = useThemeStore.getState().initialize()
// Call dispose when tearing down an embedded application.
```

## Exports

- `.`: `Button`, `buttonClassName`, `ButtonProps`, `Variant`, `TerraBrand`, `ThemePicker`.
- `./theme`: `themeOptions`, `ThemePreference`, `isThemePreference`, `getStoredThemePreference`, `resolveTheme`, `useThemeStore`.
- `./styles.css`: tokens, palettes, document reset/base, reduced-motion defaults.
- `./primitives.module.css`: shared button, card, alert, badge, spinner, and heading styles. Admin-specific router and translation adapters remain in Sol.

## Theme Contract

`--color-*` semantic tokens are the common palette contract. Both main UIs use them
directly; there is no old-token alias layer. Spacing, angular radii, typography, and
layout measures live in `tokens.css`. All original named Sol palettes live
in `themes.css`; their `data-theme` selectors are retained. Selected text contrast
was improved where necessary. `dark` is now the acid/violet Terra default.

Shared `Button` elements expose `data-terra-button`; app-local button selectors
must exclude that attribute to avoid overriding shared primitives.

The existing `localStorage.theme` string remains compatible. Invalid or missing
preferences fall back to `dark`; explicit `auto` follows the OS. Storage denial
does not prevent in-memory selection. Same-origin windows track storage events.
Different app origins retain independent preferences, not cross-origin sync.

The shared picker uses a native select for keyboard and touch navigation, grouped
options, localized labels supplied by the host, and a non-submitting random action.

## Overlay Boundary

Terra's stream overlay intentionally remains a fixed dark capture surface over
arbitrary streamed content. It does not initialize the application theme store.
Transparent `html`, `body`, and `#root` backgrounds remain app-local, as do overlay
opacity, telemetry chart colors, and native session behavior.

## Verification

Theme-store and picker integration coverage lives in Sol's existing Vitest
suite; Terra's App suite covers theme switching without changing profiles or modes.
Run each app's existing `test`, `typecheck`, and frontend build scripts after the
workspace dependencies are installed. Browser review should cover both main UIs
at 1440px and 390px, a light palette, a dark extra palette, admin modals/config,
and the translucent stream overlay.
