/** @file Localized shared Eclipse theme selector. */
import { ThemePicker } from '@supernova/design-system'
import { useTranslation } from 'react-i18next'

/** @brief Keeps the admin translations while sharing selection behavior with Terra. */
export function ThemeMenu() {
  const { t } = useTranslation()
  return (
    <ThemePicker
      label={t('navbar.toggle_theme')}
      randomLabel={t('navbar.theme_random')}
      darkLabel={t('navbar.theme_group_dark')}
      lightLabel={t('navbar.theme_group_light')}
      themeLabel={(theme) => t(`navbar.theme_${theme.replaceAll('-', '_')}`)}
    />
  )
}
