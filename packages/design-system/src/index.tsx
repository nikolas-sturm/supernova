import type { ButtonHTMLAttributes } from 'react'
import identity from './identity.module.css'
import styles from './primitives.module.css'
import { type ThemePreference, themeOptions, useThemeStore } from './theme'

export type Variant = 'primary' | 'secondary' | 'success' | 'danger' | 'warning' | 'info'
export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: Variant | undefined
  outline?: boolean | undefined
  small?: boolean | undefined
}

/** @brief Shared action styling for native buttons and router-owned links. */
export function buttonClassName({
  variant = 'primary',
  outline = false,
  small = false,
  className = '',
}: ButtonProps = {}) {
  return [
    styles.button,
    styles[variant],
    outline ? styles.outline : '',
    small ? styles.small : '',
    className,
  ]
    .filter(Boolean)
    .join(' ')
}

/** @brief Accessible native action button; callers retain all event and form props. */
export function Button({ variant, outline, small, className, ...props }: ButtonProps) {
  return (
    <button
      type="button"
      data-terra-button=""
      {...props}
      className={buttonClassName({ variant, outline, small, className })}
    />
  )
}

/** @brief Terra identity shared by host and client; subtitle identifies the surface. */
export function TerraBrand({ subtitle }: { subtitle: string }) {
  return (
    <span className={identity.brand}>
      <span className={identity.mark} aria-hidden="true" />
      <span>
        <strong>TERRA</strong>
        <small>{subtitle}</small>
      </span>
    </span>
  )
}

const defaultThemeLabel = (theme: ThemePreference) =>
  theme === 'dark'
    ? 'Terra / Dark'
    : theme === 'auto'
      ? 'System'
      : theme
          .split('-')
          .map((word) => word.charAt(0).toUpperCase() + word.slice(1))
          .join(' ')

/** @brief Native select supplies keyboard, touch, and screen-reader theme navigation. */
export function ThemePicker({
  label = 'Theme',
  randomLabel = 'Random theme',
  darkLabel = 'Dark themes',
  lightLabel = 'Light themes',
  themeLabel = defaultThemeLabel,
}: {
  label?: string
  randomLabel?: string
  darkLabel?: string
  lightLabel?: string
  themeLabel?: (theme: ThemePreference) => string
}) {
  const preference = useThemeStore((state) => state.preference)
  const setTheme = useThemeStore((state) => state.setTheme)
  const randomTheme = useThemeStore((state) => state.randomTheme)
  return (
    <div className={identity.themePicker}>
      <label>
        <span>{label}</span>
        <select
          value={preference}
          onChange={(event) => setTheme(event.currentTarget.value as ThemePreference)}
        >
          <option value="auto">{themeLabel('auto')}</option>
          <optgroup label={darkLabel}>
            {themeOptions.dark.map((theme) => (
              <option key={theme} value={theme}>
                {themeLabel(theme)}
              </option>
            ))}
          </optgroup>
          <optgroup label={lightLabel}>
            {themeOptions.light.map((theme) => (
              <option key={theme} value={theme}>
                {themeLabel(theme)}
              </option>
            ))}
          </optgroup>
        </select>
      </label>
      <Button
        variant="secondary"
        small
        onClick={randomTheme}
        aria-label={randomLabel}
        title={randomLabel}
      >
        <svg
          width="16"
          height="16"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          strokeWidth="1.8"
          aria-hidden="true"
        >
          <path d="M20 7v5h-5M20 12a8 8 0 1 0-2 5" />
        </svg>
      </Button>
    </div>
  )
}
