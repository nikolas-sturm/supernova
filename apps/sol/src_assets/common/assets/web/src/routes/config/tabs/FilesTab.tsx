/**
 * @file Config Files settings tab (legacy `configs/tabs/Files.vue`).
 */

import { useTranslation } from 'react-i18next'
import { TextField } from '../../../components/fields'
import type { TabBodyProps } from '../TabContent'

/**
 * @brief Files tab: file path settings.
 * @param props Tab props.
 * @returns The tab content.
 */
export function FilesTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="files" className="configPage">
      <TextField
        id="file_apps"
        label={t('config.file_apps')}
        description={t('config.file_apps_desc')}
        value={String(draft.file_apps ?? '')}
        onChange={(value) => setDraftValue('file_apps', value)}
        placeholder="apps.json"
        mono
      />
      <TextField
        id="credentials_file"
        label={t('config.credentials_file')}
        description={t('config.credentials_file_desc')}
        value={String(draft.credentials_file ?? '')}
        onChange={(value) => setDraftValue('credentials_file', value)}
        placeholder="sunshine_state.json"
        mono
      />
      <TextField
        id="log_path"
        label={t('config.log_path')}
        description={t('config.log_path_desc')}
        value={String(draft.log_path ?? '')}
        onChange={(value) => setDraftValue('log_path', value)}
        placeholder="sol.log"
        mono
      />
      <TextField
        id="pkey"
        label={t('config.pkey')}
        description={t('config.pkey_desc')}
        value={String(draft.pkey ?? '')}
        onChange={(value) => setDraftValue('pkey', value)}
        placeholder="/dir/pkey.pem"
        mono
      />
      <TextField
        id="cert"
        label={t('config.cert')}
        description={t('config.cert_desc')}
        value={String(draft.cert ?? '')}
        onChange={(value) => setDraftValue('cert', value)}
        placeholder="/dir/cert.pem"
        mono
      />
      <TextField
        id="file_state"
        label={t('config.file_state')}
        description={t('config.file_state_desc')}
        value={String(draft.file_state ?? '')}
        onChange={(value) => setDraftValue('file_state', value)}
        placeholder="sunshine_state.json"
        mono
      />
    </div>
  )
}
