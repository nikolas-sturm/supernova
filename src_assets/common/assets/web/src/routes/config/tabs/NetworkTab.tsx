/**
 * @file Network settings tab (legacy `configs/tabs/Network.vue`).
 */

import { Info, TriangleAlert } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { CheckboxField, SelectField, TextField } from '../../../components/fields'
import { Alert } from '../../../components/ui'
import type { TabBodyProps } from '../TabContent'
import styles from './NetworkTab.module.css'

const DEFAULT_MOONLIGHT_PORT = 47989

/**
 * @brief Network tab: UPnP, ports, web UI origin, encryption.
 * @param props Tab props.
 * @returns The tab content.
 */
export function NetworkTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  const effectivePort = Number(draft.port ?? DEFAULT_MOONLIGHT_PORT) || DEFAULT_MOONLIGHT_PORT

  return (
    <div id="network" className="configPage">
      <CheckboxField
        id="upnp"
        label={t('config.upnp')}
        description={t('config.upnp_desc')}
        value={draft.upnp ?? 'disabled'}
        onChange={(value) => setDraftValue('upnp', value)}
      />

      <SelectField
        id="address_family"
        label={t('config.address_family')}
        description={t('config.address_family_desc')}
        value={String(draft.address_family ?? 'ipv4')}
        onChange={(value) => setDraftValue('address_family', value)}
        options={[
          { value: 'ipv4', label: t('config.address_family_ipv4') },
          { value: 'both', label: t('config.address_family_both') },
        ]}
      />

      <TextField
        id="bind_address"
        label={t('config.bind_address')}
        description={t('config.bind_address_desc')}
        value={String(draft.bind_address ?? '')}
        onChange={(value) => setDraftValue('bind_address', value)}
      />

      <div className={styles.portBlock} id="port-field">
        <label className="configSectionLabel" htmlFor="port">
          {t('config.port')}
        </label>
        <input
          id="port"
          type="number"
          min={1029}
          max={65514}
          className="inputCell"
          placeholder={String(DEFAULT_MOONLIGHT_PORT)}
          value={String(draft.port ?? DEFAULT_MOONLIGHT_PORT)}
          onChange={(event) => setDraftValue('port', event.target.value)}
        />
        <div className="configSectionDesc">{t('config.port_desc')}</div>

        {effectivePort - 5 < 1024 && (
          <Alert variant="danger">
            <TriangleAlert size={20} aria-hidden /> {t('config.port_alert_1')}
          </Alert>
        )}
        {effectivePort + 21 > 65535 && (
          <Alert variant="danger">
            <TriangleAlert size={20} aria-hidden /> {t('config.port_alert_2')}
          </Alert>
        )}

        <table className="table">
          <thead>
            <tr>
              <th>{t('config.port_protocol')}</th>
              <th>{t('config.port_port')}</th>
              <th>{t('config.port_note')}</th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td>{t('config.port_tcp')}</td>
              <td>{effectivePort - 5}</td>
              <td />
            </tr>
            <tr>
              <td>{t('config.port_tcp')}</td>
              <td>{effectivePort}</td>
              <td>
                {effectivePort !== DEFAULT_MOONLIGHT_PORT && (
                  <div className={styles.noteRow}>
                    <Info size={20} aria-hidden /> {t('config.port_http_port_note')}
                  </div>
                )}
              </td>
            </tr>
            <tr>
              <td>{t('config.port_tcp')}</td>
              <td>{effectivePort + 1}</td>
              <td>{t('config.port_web_ui')}</td>
            </tr>
            <tr>
              <td>{t('config.port_tcp')}</td>
              <td>{effectivePort + 21}</td>
              <td />
            </tr>
            <tr>
              <td>{t('config.port_udp')}</td>
              <td>
                {effectivePort + 9} - {effectivePort + 11}
              </td>
              <td />
            </tr>
          </tbody>
        </table>

        {draft.origin_web_ui_allowed === 'wan' && (
          <Alert variant="warning">
            <TriangleAlert size={20} aria-hidden /> {t('config.port_warning')}
          </Alert>
        )}
      </div>

      <SelectField
        id="origin_web_ui_allowed"
        label={t('config.origin_web_ui_allowed')}
        description={t('config.origin_web_ui_allowed_desc')}
        value={String(draft.origin_web_ui_allowed ?? 'lan')}
        onChange={(value) => setDraftValue('origin_web_ui_allowed', value)}
        options={[
          { value: 'pc', label: t('config.origin_web_ui_allowed_pc') },
          { value: 'lan', label: t('config.origin_web_ui_allowed_lan') },
          { value: 'wan', label: t('config.origin_web_ui_allowed_wan') },
        ]}
      />

      <TextField
        id="csrf_allowed_origins"
        label={t('config.csrf_allowed_origins')}
        description={t('config.csrf_allowed_origins_desc')}
        value={String(draft.csrf_allowed_origins ?? '')}
        onChange={(value) => setDraftValue('csrf_allowed_origins', value)}
      />

      <TextField
        id="external_ip"
        label={t('config.external_ip')}
        description={t('config.external_ip_desc')}
        value={String(draft.external_ip ?? '')}
        onChange={(value) => setDraftValue('external_ip', value)}
        placeholder="123.456.789.12"
      />

      <SelectField
        id="lan_encryption_mode"
        label={t('config.lan_encryption_mode')}
        description={t('config.lan_encryption_mode_desc')}
        value={String(draft.lan_encryption_mode ?? 0)}
        onChange={(value) => setDraftValue('lan_encryption_mode', value)}
        options={[
          { value: '0', label: t('_common.disabled_def') },
          { value: '1', label: t('config.lan_encryption_mode_1') },
          { value: '2', label: t('config.lan_encryption_mode_2') },
        ]}
      />

      <SelectField
        id="wan_encryption_mode"
        label={t('config.wan_encryption_mode')}
        description={t('config.wan_encryption_mode_desc')}
        value={String(draft.wan_encryption_mode ?? 1)}
        onChange={(value) => setDraftValue('wan_encryption_mode', value)}
        options={[
          { value: '0', label: t('_common.disabled') },
          { value: '1', label: t('config.wan_encryption_mode_1') },
          { value: '2', label: t('config.wan_encryption_mode_2') },
        ]}
      />

      <TextField
        id="ping_timeout"
        label={t('config.ping_timeout')}
        description={t('config.ping_timeout_desc')}
        value={String(draft.ping_timeout ?? 10000)}
        onChange={(value) => setDraftValue('ping_timeout', value)}
        placeholder="10000"
      />

      <TextField
        id="packetsize"
        label={t('config.packetsize')}
        description={t('config.packetsize_desc')}
        value={String(draft.packetsize ?? 0)}
        onChange={(value) => setDraftValue('packetsize', value)}
        placeholder="0"
      />
    </div>
  )
}
