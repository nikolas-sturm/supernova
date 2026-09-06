/**
 * @brief Application create/edit modal (legacy apps.html editor) with
 * prep-command tables, detached commands, file browsing, and cover finder.
 */

import { useMutation, useQueryClient } from '@tanstack/react-query'
import { FolderOpen, Play, Plus, Save, Search, Shield, Trash2, Undo, X } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { saveApp } from '../../api/endpoints'
import type { AppRecord } from '../../api/schemas'
import { CoverFinderModal } from '../../components/CoverFinder'
import { CheckboxField, NumberField, TextField } from '../../components/fields'
import { Modal } from '../../components/Modal'
import type { Platform } from '../../components/Platform'
import { Alert, Button } from '../../components/ui'
import { queryKeys } from '../../queries'
import { openFileBrowser } from '../../store/fileBrowserStore'
import styles from './AppEditorModal.module.css'

export interface AppEditorModalProps {
  /** The app record being edited (index -1 means create). */
  app: AppRecord
  /** Host platform. */
  platform: Platform
  /** Running Sunshine version (unused for now, kept for parity). */
  version: string
  /** Close callback. */
  onClose: () => void
}

/** A prep command row. */
type PrepCmd = NonNullable<AppRecord['prep-cmd']>[number]

/**
 * @brief Modal for editing all fields of one application.
 * @param props Editor props.
 * @returns The modal element.
 */
export function AppEditorModal({ app, platform, onClose }: AppEditorModalProps) {
  const { t } = useTranslation()
  const queryClient = useQueryClient()
  const [draft, setDraft] = useState<AppRecord>(app)
  const [coverFinderOpen, setCoverFinderOpen] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const saveMutation = useMutation({
    mutationFn: (record: AppRecord) => saveApp(record as unknown as Record<string, unknown>),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: queryKeys.apps })
      onClose()
    },
    onError: (err) => {
      setError(err instanceof Error ? err.message : 'Failed to save')
    },
  })

  /**
   * @brief Updates one field of the draft.
   * @param key The field name.
   * @param value The new value.
   */
  const update = <K extends keyof AppRecord>(key: K, value: AppRecord[K]) => {
    setDraft((current) => ({ ...current, [key]: value }))
  }

  const prepCommands = (draft['prep-cmd'] ?? []) as PrepCmd[]
  const detached = draft.detached ?? []

  /**
   * @brief Adds an empty prep command row.
   */
  const addPrepCmd = () => {
    const row: PrepCmd =
      platform === 'windows' ? { do: '', undo: '', elevated: false } : { do: '', undo: '' }
    update('prep-cmd', [...prepCommands, row])
  }

  /**
   * @brief Opens the file browser and writes the chosen path back to a field.
   * @param type Browse filter type.
   * @param apply Setter receiving the selected path.
   */
  const browse = async (
    type: 'any' | 'file' | 'directory' | 'executable',
    apply: (path: string) => void,
  ) => {
    const path = await openFileBrowser({ type })
    if (path !== null) {
      apply(path)
    }
  }

  return (
    <Modal
      open
      size="xl"
      title={
        draft.index === -1
          ? t('apps.add_new')
          : draft.name
            ? `${t('apps.edit')}: ${draft.name}`
            : t('apps.edit')
      }
      onClose={onClose}
      footer={
        <>
          <Button variant="secondary" onClick={onClose}>
            <X size={18} aria-hidden />
            {t('_common.cancel')}
          </Button>
          <Button
            onClick={() => {
              const record = {
                ...draft,
                'image-path': String(draft['image-path'] ?? '').replaceAll('"', ''),
              }
              saveMutation.mutate(record)
            }}
          >
            <Save size={18} aria-hidden />
            {t('_common.save')}
          </Button>
        </>
      }
    >
      {error && <Alert variant="danger">{error}</Alert>}

      <TextField
        id="appName"
        label={t('apps.app_name')}
        description={t('apps.app_name_desc')}
        value={String(draft.name ?? '')}
        onChange={(value) => update('name', value)}
      />

      <div className={styles.withBrowse}>
        <TextField
          id="appOutput"
          label={t('apps.output_name')}
          description={t('apps.output_desc')}
          value={String(draft.output ?? '')}
          onChange={(value) => update('output', value)}
          mono
        />
        <Button
          variant="secondary"
          onClick={() => void browse('any', (path) => update('output', path))}
          aria-label={t('file_browser.select_file')}
        >
          <FolderOpen size={18} aria-hidden />
        </Button>
      </div>

      <CheckboxField
        id="excludeGlobalPrep"
        label={t('apps.global_prep_name')}
        description={t('apps.global_prep_desc')}
        value={draft['exclude-global-prep-cmd'] ?? false}
        onChange={(value) => update('exclude-global-prep-cmd', value === 'true')}
        defaultValue
      />

      <div className={styles.section}>
        <div className={styles.sectionHead}>
          <div className={styles.sectionLabel}>{t('apps.cmd_prep_name')}</div>
          <div className={styles.sectionDesc}>{t('apps.cmd_prep_desc')}</div>
        </div>
        {prepCommands.length === 0 && (
          <Button variant="success" onClick={addPrepCmd}>
            <Plus size={18} aria-hidden />
            {t('apps.add_cmds')}
          </Button>
        )}
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
                    <div className={styles.withBrowse}>
                      <input
                        type="text"
                        className={`${styles.cellInput} mono`}
                        value={command.do ?? ''}
                        onChange={(event) => {
                          const next = [...prepCommands]
                          next[index] = { ...command, do: event.target.value }
                          update('prep-cmd', next)
                        }}
                      />
                      <Button
                        small
                        variant="secondary"
                        onClick={() =>
                          void browse('executable', (path) => {
                            const next = [...prepCommands]
                            next[index] = { ...command, do: path }
                            update('prep-cmd', next)
                          })
                        }
                        aria-label={t('file_browser.select_executable')}
                      >
                        <FolderOpen size={14} aria-hidden />
                      </Button>
                    </div>
                  </td>
                  <td>
                    <div className={styles.withBrowse}>
                      <input
                        type="text"
                        className={`${styles.cellInput} mono`}
                        value={command.undo ?? ''}
                        onChange={(event) => {
                          const next = [...prepCommands]
                          next[index] = { ...command, undo: event.target.value }
                          update('prep-cmd', next)
                        }}
                      />
                      <Button
                        small
                        variant="secondary"
                        onClick={() =>
                          void browse('executable', (path) => {
                            const next = [...prepCommands]
                            next[index] = { ...command, undo: path }
                            update('prep-cmd', next)
                          })
                        }
                        aria-label={t('file_browser.select_executable')}
                      >
                        <FolderOpen size={14} aria-hidden />
                      </Button>
                    </div>
                  </td>
                  {platform === 'windows' && (
                    <td>
                      <CheckboxField
                        id={`prep-cmd-admin-${index}`}
                        label={t('_common.elevated')}
                        value={command.elevated ?? false}
                        onChange={(value) => {
                          const next = [...prepCommands]
                          next[index] = { ...command, elevated: value === 'true' }
                          update('prep-cmd', next)
                        }}
                      />
                    </td>
                  )}
                  <td>
                    <div className={styles.rowActions}>
                      <Button
                        small
                        variant="danger"
                        onClick={() =>
                          update(
                            'prep-cmd',
                            prepCommands.filter((_, i) => i !== index),
                          )
                        }
                        aria-label={t('apps.delete')}
                      >
                        <Trash2 size={14} aria-hidden />
                      </Button>
                      <Button
                        small
                        variant="success"
                        onClick={addPrepCmd}
                        aria-label={t('apps.add_cmds')}
                      >
                        <Plus size={14} aria-hidden />
                      </Button>
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      <div className={styles.section}>
        <div className={styles.sectionHead}>
          <div className={styles.sectionLabel}>{t('apps.detached_cmds')}</div>
          <div className={styles.sectionDesc}>
            {t('apps.detached_cmds_desc')}
            <br />
            <strong>{t('_common.note')}</strong> {t('apps.detached_cmds_note')}
          </div>
        </div>
        {detached.map((command, index) => (
          // biome-ignore lint/suspicious/noArrayIndexKey: rows are positional editing entries
          <div key={index} className={styles.detachedRow}>
            <input
              type="text"
              className={`${styles.cellInput} mono`}
              value={command}
              onChange={(event) => {
                const next = [...detached]
                next[index] = event.target.value
                update('detached', next)
              }}
            />
            <Button
              small
              variant="secondary"
              onClick={() =>
                void browse('executable', (path) => {
                  const next = [...detached]
                  next[index] = path
                  update('detached', next)
                })
              }
              aria-label={t('file_browser.select_executable')}
            >
              <FolderOpen size={14} aria-hidden />
            </Button>
            <Button
              small
              variant="danger"
              onClick={() =>
                update(
                  'detached',
                  detached.filter((_, i) => i !== index),
                )
              }
              aria-label={t('apps.delete')}
            >
              <Trash2 size={14} aria-hidden />
            </Button>
            <Button
              small
              variant="success"
              onClick={() => update('detached', [...detached, ''])}
              aria-label={t('apps.detached_cmds_add')}
            >
              <Plus size={14} aria-hidden />
            </Button>
          </div>
        ))}
        {detached.length === 0 && (
          <Button variant="success" onClick={() => update('detached', [...detached, ''])}>
            <Plus size={18} aria-hidden />
            {t('apps.detached_cmds_add')}
          </Button>
        )}
      </div>

      <div className={styles.withBrowse}>
        <TextField
          id="appCmd"
          label={t('apps.cmd')}
          description={
            <>
              {t('apps.cmd_desc')}
              <br />
              <strong>{t('_common.note')}</strong> {t('apps.cmd_note')}
            </>
          }
          value={String(draft.cmd ?? '')}
          onChange={(value) => update('cmd', value)}
          mono
        />
        <Button
          variant="secondary"
          onClick={() => void browse('executable', (path) => update('cmd', path))}
          aria-label={t('file_browser.select_executable')}
        >
          <FolderOpen size={18} aria-hidden />
        </Button>
      </div>

      <div className={styles.withBrowse}>
        <TextField
          id="appWorkingDir"
          label={t('apps.working_dir')}
          description={t('apps.working_dir_desc')}
          value={String(draft['working-dir'] ?? '')}
          onChange={(value) => update('working-dir', value)}
          mono
        />
        <Button
          variant="secondary"
          onClick={() => void browse('directory', (path) => update('working-dir', path))}
          aria-label={t('file_browser.select_directory')}
        >
          <FolderOpen size={18} aria-hidden />
        </Button>
      </div>

      {platform === 'windows' && (
        <CheckboxField
          id="appElevation"
          label={t('_common.run_as')}
          description={t('apps.run_as_desc')}
          value={draft.elevated ?? false}
          onChange={(value) => update('elevated', value === 'true')}
          defaultValue={false}
        />
      )}

      <CheckboxField
        id="autoDetach"
        label={t('apps.auto_detach')}
        description={t('apps.auto_detach_desc')}
        value={draft['auto-detach'] ?? true}
        onChange={(value) => update('auto-detach', value === 'true')}
        defaultValue
      />

      <CheckboxField
        id="waitAll"
        label={t('apps.wait_all')}
        description={t('apps.wait_all_desc')}
        value={draft['wait-all'] ?? true}
        onChange={(value) => update('wait-all', value === 'true')}
        defaultValue
      />

      <NumberField
        id="exitTimeout"
        label={t('apps.exit_timeout')}
        description={t('apps.exit_timeout_desc')}
        value={String(draft['exit-timeout'] ?? 5)}
        onChange={(value) => update('exit-timeout', value)}
        min={0}
        placeholder="5"
      />

      <div className={styles.withBrowse}>
        <TextField
          id="appImagePath"
          label={t('apps.image')}
          description={t('apps.image_desc')}
          value={String(draft['image-path'] ?? '')}
          onChange={(value) => update('image-path', value)}
          mono
        />
        <Button
          variant="secondary"
          onClick={() => void browse('file', (path) => update('image-path', path))}
          aria-label={t('file_browser.select_file')}
        >
          <FolderOpen size={18} aria-hidden />
        </Button>
        <Button variant="secondary" onClick={() => setCoverFinderOpen(true)}>
          <Search size={18} aria-hidden />
          {t('apps.find_cover')}
        </Button>
      </div>

      <Alert variant="info" title={t('apps.env_vars_about')}>
        {t('apps.env_vars_desc')}
        <EnvVarsTable platform={platform} />
      </Alert>

      <CoverFinderModal
        open={coverFinderOpen}
        defaultQuery={String(draft.name ?? '')}
        onCoverSelected={(path) => update('image-path', path)}
        onClose={() => setCoverFinderOpen(false)}
      />
    </Modal>
  )
}

/**
 * @brief Environment variable reference table.
 * @param props Host platform.
 * @returns The table element.
 */
function EnvVarsTable({ platform }: { platform: Platform }) {
  const { t } = useTranslation()
  const rows: [string, string][] = [
    ['SUNSHINE_APP_ID', t('apps.env_app_id')],
    ['SUNSHINE_APP_NAME', t('apps.env_app_name')],
    ['SUNSHINE_CLIENT_NAME', t('apps.env_client_name')],
    ['SUNSHINE_CLIENT_WIDTH', t('apps.env_client_width')],
    ['SUNSHINE_CLIENT_HEIGHT', t('apps.env_client_height')],
    ['SUNSHINE_CLIENT_FPS', t('apps.env_client_fps')],
    ['SUNSHINE_CLIENT_HDR', t('apps.env_client_hdr')],
    ['SUNSHINE_CLIENT_GCMAP', t('apps.env_client_gcmap')],
    ['SUNSHINE_CLIENT_HOST_AUDIO', t('apps.env_client_host_audio')],
    ['SUNSHINE_CLIENT_ENABLE_SOPS', t('apps.env_client_enable_sops')],
    ['SUNSHINE_CLIENT_AUDIO_CONFIGURATION', t('apps.env_client_audio_config')],
  ]
  return (
    <div className={styles.envBlock}>
      <table className="table">
        <tbody>
          {rows.map(([name, description]) => (
            <tr key={name}>
              <td className="mono">{name}</td>
              <td>{description}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {platform === 'windows' && (
        <div className={styles.envExample}>
          <strong>{t('apps.env_qres_example')}</strong>
          <pre>
            cmd /C &lt;{t('apps.env_qres_path')}&gt;\QRes.exe /X:%SUNSHINE_CLIENT_WIDTH%
            /Y:%SUNSHINE_CLIENT_HEIGHT% /R:%SUNSHINE_CLIENT_FPS%
          </pre>
        </div>
      )}
      {(platform === 'linux' || platform === 'freebsd') && (
        <div className={styles.envExample}>
          <strong>{t('apps.env_xrandr_example')}</strong>
          <pre>{`sh -c "xrandr --output HDMI-1 --mode \\"\${SUNSHINE_CLIENT_WIDTH}x\${SUNSHINE_CLIENT_HEIGHT}\\" --rate \${SUNSHINE_CLIENT_FPS}"`}</pre>
        </div>
      )}
      {platform === 'macos' && (
        <div className={styles.envExample}>
          <strong>{t('apps.env_displayplacer_example')}</strong>
          <pre>{`sh -c "displayplacer \\"id:<screenId> res:\${SUNSHINE_CLIENT_WIDTH}x\${SUNSHINE_CLIENT_HEIGHT} hz:\${SUNSHINE_CLIENT_FPS} scaling:on origin:(0,0) degree:0\\""`}</pre>
        </div>
      )}
    </div>
  )
}
