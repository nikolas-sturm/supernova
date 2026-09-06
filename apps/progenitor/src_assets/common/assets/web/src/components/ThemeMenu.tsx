/**
 * @file Theme selection dropdown with all themes plus auto and random.
 */

import {
  CloudMoon,
  CloudRain,
  Coffee,
  Contrast,
  Droplet,
  Flame,
  Flower,
  Flower2,
  Ghost,
  Layers,
  type LucideIcon,
  Milk,
  MonitorSmartphone,
  Moon,
  Mountain,
  Shuffle,
  Sparkles,
  Sprout,
  Sun,
  Sunrise,
  Sunset,
  TreePine,
  Trees,
  Waves,
} from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { type ThemePreference, themeOptions, useThemeStore } from '../store/themeStore'
import styles from './ThemeMenu.module.css'

/** Icon per theme id used in the menu. */
const themeIcons: Record<string, LucideIcon> = {
  auto: MonitorSmartphone,
  dark: Moon,
  dracula: Ghost,
  mocha: Coffee,
  ember: Flame,
  'rose-pine': TreePine,
  moonlight: CloudMoon,
  slate: Layers,
  midnight: CloudRain,
  nord: Mountain,
  light: Sun,
  alucard: Droplet,
  latte: Milk,
  'ember-light': Sunset,
  'rose-pine-dawn': Sprout,
  sunshine: Sunrise,
  indigo: Sparkles,
  ocean: Waves,
  forest: Trees,
  rose: Flower,
  lavender: Flower2,
  monochrome: Contrast,
}

interface ThemeItemProps {
  /** The theme id this item selects. */
  theme: ThemePreference
  /** Whether this theme is currently active. */
  active: boolean
  /** Selection callback. */
  onSelect: (theme: ThemePreference) => void
}

/**
 * @brief One selectable theme entry in the menu.
 * @param props Item props.
 * @returns The item element.
 */
function ThemeItem({ theme, active, onSelect }: ThemeItemProps) {
  const { t } = useTranslation()
  const Icon = themeIcons[theme] ?? Moon
  return (
    <button
      type="button"
      role="menuitemradio"
      aria-checked={active}
      className={`${styles.item} ${active ? styles.active : ''}`}
      onClick={() => onSelect(theme)}
    >
      <Icon size={18} aria-hidden />
      {t(`navbar.theme_${theme.replaceAll('-', '_')}`)}
    </button>
  )
}

/**
 * @brief Dropdown for switching the UI theme.
 * @returns The theme menu element.
 */
export function ThemeMenu() {
  const { t } = useTranslation()
  const preference = useThemeStore((state) => state.preference)
  const setTheme = useThemeStore((state) => state.setTheme)
  const randomTheme = useThemeStore((state) => state.randomTheme)
  const [open, setOpen] = useState(false)
  const containerRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) {
      return
    }
    const onPointerDown = (event: PointerEvent) => {
      if (containerRef.current && !containerRef.current.contains(event.target as Node)) {
        setOpen(false)
      }
    }
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        setOpen(false)
      }
    }
    document.addEventListener('pointerdown', onPointerDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('pointerdown', onPointerDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [open])

  const select = (theme: ThemePreference) => {
    setTheme(theme)
    setOpen(false)
  }

  const ActiveIcon = themeIcons[preference] ?? MonitorSmartphone

  return (
    <div className={styles.wrap} ref={containerRef}>
      <button
        type="button"
        className={styles.trigger}
        aria-haspopup="menu"
        aria-expanded={open}
        aria-label={t('navbar.toggle_theme')}
        onClick={() => setOpen((value) => !value)}
      >
        <ActiveIcon size={18} aria-hidden />
        <span className={styles.triggerLabel}>{t('navbar.toggle_theme')}</span>
      </button>
      {open && (
        <div className={styles.menu} role="menu">
          <button
            type="button"
            role="menuitemradio"
            aria-checked={preference === 'auto'}
            className={`${styles.item} ${preference === 'auto' ? styles.active : ''}`}
            onClick={() => select('auto')}
          >
            <MonitorSmartphone size={18} aria-hidden />
            {t('navbar.theme_auto')}
          </button>
          <button type="button" role="menuitem" className={styles.item} onClick={randomTheme}>
            <Shuffle size={18} aria-hidden />
            {t('navbar.theme_random')}
          </button>

          <div className={styles.group}>{t('navbar.theme_group_dark')}</div>
          {themeOptions.dark.map((theme) => (
            <ThemeItem key={theme} theme={theme} active={preference === theme} onSelect={select} />
          ))}

          <div className={styles.group}>{t('navbar.theme_group_light')}</div>
          {themeOptions.light.map((theme) => (
            <ThemeItem key={theme} theme={theme} active={preference === theme} onSelect={select} />
          ))}
        </div>
      )}
    </div>
  )
}
