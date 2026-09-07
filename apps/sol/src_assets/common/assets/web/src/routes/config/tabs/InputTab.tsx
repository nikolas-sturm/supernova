/**
 * @file Input settings tab (legacy `configs/tabs/Inputs.vue`).
 */

import { useTranslation } from 'react-i18next'
import { CheckboxField, SelectField, TextField } from '../../../components/fields'
import type { TabBodyProps } from '../TabContent'

const gamepadTypes = [
  ['generic', 'config.gamepad_generic'],
  ['x360', 'config.gamepad_x360'],
  ['xone', 'config.gamepad_xone'],
  ['xseries', 'config.gamepad_xseries'],
  ['ds4', 'config.gamepad_ds4'],
  ['ds5', 'config.gamepad_ds5'],
  ['switch', 'config.gamepad_switch'],
] as const

/**
 * @brief Input tab: gamepad, keyboard, and mouse options.
 * @param props Tab props.
 * @returns The tab content.
 */
export function InputTab({ draft, setDraftValue, platform }: TabBodyProps) {
  const { t } = useTranslation()
  const controllerEnabled = draft.controller === 'enabled'
  const keyboardEnabled = draft.keyboard === 'enabled'
  const mouseEnabled = draft.mouse === 'enabled'
  const gamepad = String(draft.gamepad ?? 'auto')
  const ds4Selection =
    gamepad === 'ds4' || gamepad === 'ds5' || (gamepad === 'auto' && platform !== 'macos')

  return (
    <div id="input" className="configPage">
      <CheckboxField
        id="controller"
        label={t('config.controller')}
        value={draft.controller ?? 'enabled'}
        onChange={(value) => setDraftValue('controller', value)}
      />

      {controllerEnabled && platform !== 'macos' && (
        <SelectField
          id="gamepad"
          label={t('config.gamepad')}
          description={t('config.gamepad_desc')}
          value={gamepad}
          onChange={(value) => setDraftValue('gamepad', value)}
          options={[
            { value: 'auto', label: t('_common.auto') },
            ...gamepadTypes.map(([value, key]) => ({ value, label: t(key) })),
          ]}
        />
      )}

      {controllerEnabled && ds4Selection && (
        <div className="configSection">
          <div className="configSectionLabel">
            {t(gamepad === 'auto' ? 'config.gamepad_auto' : 'config.gamepad_ds4_manual')}
          </div>
          {gamepad === 'auto' && (platform === 'windows' || platform === 'linux') && (
            <>
              <CheckboxField
                id="motion_as_ds4"
                label={t('config.motion_as_ds4')}
                description={t('config.motion_as_ds4_desc')}
                value={draft.motion_as_ds4 ?? 'enabled'}
                onChange={(value) => setDraftValue('motion_as_ds4', value)}
              />
              <CheckboxField
                id="touchpad_as_ds4"
                label={t('config.touchpad_as_ds4')}
                description={t('config.touchpad_as_ds4_desc')}
                value={draft.touchpad_as_ds4 ?? 'enabled'}
                onChange={(value) => setDraftValue('touchpad_as_ds4', value)}
              />
            </>
          )}
          <CheckboxField
            id="ds4_back_as_touchpad_click"
            label={t('config.ds4_back_as_touchpad_click')}
            description={t('config.ds4_back_as_touchpad_click_desc')}
            value={draft.ds4_back_as_touchpad_click ?? 'enabled'}
            onChange={(value) => setDraftValue('ds4_back_as_touchpad_click', value)}
          />
          <CheckboxField
            id="virtualhid_randomize_mac"
            label={t('config.virtualhid_randomize_mac')}
            description={t('config.virtualhid_randomize_mac_desc')}
            value={draft.virtualhid_randomize_mac ?? 'enabled'}
            onChange={(value) => setDraftValue('virtualhid_randomize_mac', value)}
          />
        </div>
      )}

      {controllerEnabled && (
        <TextField
          id="back_button_timeout"
          label={t('config.back_button_timeout')}
          description={t('config.back_button_timeout_desc')}
          value={String(draft.back_button_timeout ?? -1)}
          onChange={(value) => setDraftValue('back_button_timeout', value)}
          placeholder="-1"
        />
      )}

      <hr className="divider" />

      <CheckboxField
        id="keyboard"
        label={t('config.keyboard')}
        value={draft.keyboard ?? 'enabled'}
        onChange={(value) => setDraftValue('keyboard', value)}
      />

      {keyboardEnabled && platform === 'windows' && (
        <>
          <TextField
            id="key_repeat_delay"
            label={t('config.key_repeat_delay')}
            description={t('config.key_repeat_delay_desc')}
            value={String(draft.key_repeat_delay ?? 500)}
            onChange={(value) => setDraftValue('key_repeat_delay', value)}
            placeholder="500"
          />
          <TextField
            id="key_repeat_frequency"
            label={t('config.key_repeat_frequency')}
            description={t('config.key_repeat_frequency_desc')}
            value={String(draft.key_repeat_frequency ?? 24.9)}
            onChange={(value) => setDraftValue('key_repeat_frequency', value)}
            placeholder="24.9"
          />
          <CheckboxField
            id="always_send_scancodes"
            label={t('config.always_send_scancodes')}
            description={t('config.always_send_scancodes_desc')}
            value={draft.always_send_scancodes ?? 'enabled'}
            onChange={(value) => setDraftValue('always_send_scancodes', value)}
          />
        </>
      )}

      {keyboardEnabled && (
        <CheckboxField
          id="key_rightalt_to_key_win"
          label={t('config.key_rightalt_to_key_win')}
          description={t('config.key_rightalt_to_key_win_desc')}
          value={draft.key_rightalt_to_key_win ?? 'disabled'}
          onChange={(value) => setDraftValue('key_rightalt_to_key_win', value)}
        />
      )}

      <hr className="divider" />

      <CheckboxField
        id="mouse"
        label={t('config.mouse')}
        value={draft.mouse ?? 'enabled'}
        onChange={(value) => setDraftValue('mouse', value)}
      />

      {mouseEnabled && (
        <>
          <CheckboxField
            id="high_resolution_scrolling"
            label={t('config.high_resolution_scrolling')}
            description={t('config.high_resolution_scrolling_desc')}
            value={draft.high_resolution_scrolling ?? 'enabled'}
            onChange={(value) => setDraftValue('high_resolution_scrolling', value)}
          />
          <CheckboxField
            id="native_pen_touch"
            label={t('config.native_pen_touch')}
            description={t('config.native_pen_touch_desc')}
            value={draft.native_pen_touch ?? 'enabled'}
            onChange={(value) => setDraftValue('native_pen_touch', value)}
          />
        </>
      )}
    </div>
  )
}
