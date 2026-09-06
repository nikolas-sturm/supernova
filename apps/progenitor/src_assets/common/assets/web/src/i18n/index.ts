/**
 * @file i18next setup.
 *
 * The bundled `en` locale is the Crowdin source file and must stay at
 * `public/assets/locale/en.json`. Additional locales are lazily fetched from
 * the same directory at runtime. Interpolation uses single braces (`{name}`)
 * to stay compatible with the existing vue-i18n message syntax.
 */

import i18next from 'i18next'
import { initReactI18next } from 'react-i18next'
import en from '../../public/assets/locale/en.json'

export const i18n = i18next.createInstance()

i18n.use(initReactI18next).init({
  lng: 'en',
  fallbackLng: 'en',
  resources: {
    en: { translation: en as Record<string, unknown> },
  },
  interpolation: {
    // React already escapes output.
    escapeValue: false,
    prefix: '{',
    suffix: '}',
  },
  returnNull: false,
})

/**
 * @brief Loads the configured locale from the backend and fetches its messages.
 *
 * Falls back to English when the locale request or translation download
 * fails. The `configLocale` endpoint works without authentication.
 */
export async function initI18n(): Promise<void> {
  try {
    const response = await fetch('./api/configLocale')
    if (!response.ok) {
      return
    }
    const body = (await response.json()) as { locale?: string }
    const locale = body.locale ?? 'en'
    document.documentElement.lang = locale
    if (locale === 'en') {
      return
    }
    const messagesResponse = await fetch(`./assets/locale/${locale}.json`)
    if (!messagesResponse.ok) {
      console.error('Failed to download translations')
      return
    }
    const messages = (await messagesResponse.json()) as Record<string, unknown>
    i18n.addResourceBundle(locale, 'translation', messages, true, true)
    await i18n.changeLanguage(locale)
  } catch (error) {
    console.error('Failed to initialize locale', error)
  }
}
