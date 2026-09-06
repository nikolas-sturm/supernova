/**
 * @file General settings tab (legacy `configs/tabs/General.vue`).
 */

import { Play, Plus, Shield, Trash2, Undo } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { CheckboxField, SelectField, TextField } from '../../../components/fields'
import { Button } from '../../../components/ui'
import type { TabBodyProps } from '../TabContent'

/** Locale options matching the legacy select. */
const localeOptions = [
  'bg',
  'cs',
  'de',
  'en',
  'en_GB',
  'en_US',
  'es',
  'fr',
  'hu',
  'it',
  'ja',
  'ko',
  'pl',
  'pt',
  'pt_BR',
  'ru',
  'sv',
  'tr',
  'uk',
  'vi',
  'zh',
  'zh_TW',
] as const

const localeNames: Record<string, string> = {
  bg: 'Bulgarian',
  cs: 'Czech',
  de: 'German',
  en: 'English',
  en_GB: 'English, United Kingdom',
  en_US: 'English, United States',
  es: 'Spanish',
  fr: 'French',
  hu: 'Hungarian',
  it: 'Italian',
  ja: 'Japanese',
  ko: 'Korean',
  pl: 'Polish',
  pt: 'Portuguese',
  pt_BR: 'Português, Brasileiro',
  ru: 'Russian',
  sv: 'Swedish',
  tr: 'Türkçe',
  uk: 'Ukranian',
  vi: 'Vietnamese',
  zh: 'Chinese Simplified',
  zh_TW: 'Chinese Traditional',
}

const logLevels = [
  ['0', 'config.min_log_level_0'],
  ['1', 'config.min_log_level_1'],
  ['2', 'config.min_log_level_2'],
  ['3', 'config.min_log_level_3'],
  ['4', 'config.min_log_level_4'],
  ['5', 'config.min_log_level_5'],
  ['6', 'config.min_log_level_6'],
] as const

/**
 * @brief General tab: locale, name, log level, global prep commands.
 * @param props Tab props.
 * @returns The tab content.
 */
export function GeneralTab({ draft, setDraftValue, platform }: TabBodyProps) {
  const { t } = useTranslation()
  const prepCommands = Array.isArray(draft.global_prep_cmd)
    ? (draft.global_prep_cmd as { do: string; undo: string; elevated?: boolean }[])
    : []

  return (
    <div id="general" className="configPage">
      <SelectField
        id="locale"
        label={t('config.locale')}
        description={t('config.locale_desc')}
        value={String(draft.locale ?? 'en')}
        onChange={(value) => setDraftValue('locale', value)}
        options={localeOptions.map((value) => ({
          value,
          label: `${localeNames[value.split('_')[0] ?? '']} (${value})`,
        }))}
      />

      <TextField
        id="sunshine_name"
        label={t('config.sunshine_name')}
        description={t('config.sunshine_name_desc')}
        value={String(draft.sunshine_name ?? '')}
        onChange={(value) => setDraftValue('sunshine_name', value)}
        placeholder="Sunshine"
      />

      <SelectField
        id="min_log_level"
        label={t('config.min_log_level')}
        description={t('config.min_log_level_desc')}
        value={String(draft.min_log_level ?? 2)}
        onChange={(value) => setDraftValue('min_log_level', value)}
        options={logLevels.map(([value, key]) => ({ value, label: t(key) }))}
      />

      <div className="configSection">
        <div className="configSectionLabel">{t('config.global_prep_cmd')}</div>
        <div className="configSectionDesc">{t('config.global_prep_cmd_desc')}</div>

        {prepCommands.length > 0 && (
          <table className="table">
            <thead>
              <tr>
                <th>
                  <Play size={16} aria-hidden /> {t('_common.do_cmd')}
                </th>
                <th>
                  <Undo size={16} aria-hidden /> {t('_common.undo_cmd')}
                </th>
                {platform === 'windows' && (
                  <th>
                    <Shield size={16} aria-hidden /> {t('_common.run_as')}
                  </th>
                )}
                <th />
              </tr>
            </thead>
            <tbody>
              {prepCommands.map((command, index) => (
                // biome-ignore lint/suspicious/noArrayIndexKey: rows are positional editing entries
                <tr key={index}>
                  <td>
                    <input
                      type="text"
                      className="mono inputCell"
                      value={command.do ?? ''}
                      onChange={(event) => {
                        const next = [...prepCommands]
                        next[index] = { ...command, do: event.target.value }
                        setDraftValue('global_prep_cmd', next)
                      }}
                    />
                  </td>
                  <td>
                    <input
                      type="text"
                      className="mono inputCell"
                      value={command.undo ?? ''}
                      onChange={(event) => {
                        const next = [...prepCommands]
                        next[index] = { ...command, undo: event.target.value }
                        setDraftValue('global_prep_cmd', next)
                      }}
                    />
                  </td>
                  {platform === 'windows' && (
                    <td>
                      <CheckboxField
                        id={`global-prep-admin-${index}`}
                        label={t('_common.elevated')}
                        value={command.elevated ?? false}
                        onChange={(value) => {
                          const next = [...prepCommands]
                          next[index] = { ...command, elevated: value === 'true' }
                          setDraftValue('global_prep_cmd', next)
                        }}
                      />
                    </td>
                  )}
                  <td>
                    <Button
                      small
                      variant="danger"
                      aria-label={t('_common.close')}
                      onClick={() =>
                        setDraftValue(
                          'global_prep_cmd',
                          prepCommands.filter((_, i) => i !== index),
                        )
                      }
                    >
                      <Trash2 size={14} aria-hidden />
                    </Button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}

        <Button
          variant="success"
          onClick={() =>
            setDraftValue('global_prep_cmd', [
              ...prepCommands,
              platform === 'windows' ? { do: '', undo: '', elevated: false } : { do: '', undo: '' },
            ])
          }
        >
          <Plus size={18} aria-hidden />
          {t('config.add')}
        </Button>
      </div>

      <CheckboxField
        id="system_tray"
        label={t('config.system_tray')}
        description={t('config.system_tray_desc')}
        value={draft.system_tray ?? 'enabled'}
        onChange={(value) => setDraftValue('system_tray', value)}
      />

      <CheckboxField
        id="notify_pre_releases"
        label={t('config.notify_pre_releases')}
        description={t('config.notify_pre_releases_desc')}
        value={draft.notify_pre_releases ?? 'disabled'}
        onChange={(value) => setDraftValue('notify_pre_releases', value)}
      />
    </div>
  )
}
