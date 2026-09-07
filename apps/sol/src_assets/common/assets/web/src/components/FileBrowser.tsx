/**
 * @file Shared file browser modal host.
 *
 * Mount once in the app root; other components call `openFileBrowser()`
 * and await the resulting path.
 */

import { ArrowRight, Check, FileText, Folder, FolderUp, HardDrive } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { browseDirectory } from '../api/endpoints'
import { useFileBrowserStore } from '../store/fileBrowserStore'
import styles from './FileBrowser.module.css'
import { Modal } from './Modal'
import { Alert, Spinner } from './ui'

interface BrowseEntry {
  name: string
  path: string
  type: string
}

/**
 * @brief Renders the shared file browser when open.
 * @returns The modal element, or null when closed.
 */
export function FileBrowserModal() {
  const { t } = useTranslation()
  const open = useFileBrowserStore((state) => state.open)
  const options = useFileBrowserStore((state) => state.options)
  const finish = useFileBrowserStore((state) => state.finish)

  const [currentPath, setCurrentPath] = useState('')
  const [parentPath, setParentPath] = useState('')
  const [entries, setEntries] = useState<BrowseEntry[]>([])
  const [typedPath, setTypedPath] = useState('')
  const [selectedPath, setSelectedPath] = useState('')
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')

  const navigate = useCallback(
    async (path: string) => {
      if (!options) {
        return
      }
      setLoading(true)
      setError('')
      try {
        const data = await browseDirectory(options.type, path || undefined)
        setCurrentPath(data.path ?? '')
        setParentPath(data.parent ?? '')
        setEntries(data.entries ?? [])
        setTypedPath(data.path ?? '')
        setSelectedPath(options.type === 'directory' ? (data.path ?? '') : '')
      } catch (err) {
        setError(err instanceof Error ? err.message : 'Browse failed')
      } finally {
        setLoading(false)
      }
    },
    [options],
  )

  useEffect(() => {
    if (open && options) {
      setCurrentPath('')
      setParentPath('')
      setEntries([])
      setSelectedPath(options.startPath ?? '')
      setTypedPath(options.startPath ?? '')
      setError('')
      void navigate(options.startPath ?? '')
    }
  }, [open, options, navigate])

  if (!open || !options) {
    return null
  }

  const titleKey =
    options.type === 'directory'
      ? 'file_browser.select_directory'
      : options.type === 'executable'
        ? 'file_browser.select_executable'
        : 'file_browser.select_file'

  const selectEntry = (entry: BrowseEntry) => {
    if (entry.type === 'directory') {
      void navigate(entry.path)
    } else {
      setSelectedPath(entry.path)
      setTypedPath(entry.path)
    }
  }

  const activateEntry = (entry: BrowseEntry) => {
    if (entry.type === 'directory') {
      void navigate(entry.path)
    } else {
      setSelectedPath(entry.path)
      setTypedPath(entry.path)
      finish(entry.path)
    }
  }

  const confirm = () => {
    const path = selectedPath || typedPath
    if (path) {
      finish(path)
    }
  }

  return (
    <Modal
      open={open}
      title={t(titleKey)}
      onClose={() => finish(null)}
      footer={
        <>
          <div className={styles.selectedPath}>{selectedPath && <code>{selectedPath}</code>}</div>
          <button type="button" className={styles.secondaryButton} onClick={() => finish(null)}>
            {t('_common.cancel')}
          </button>
          <button
            type="button"
            className={styles.primaryButton}
            onClick={confirm}
            disabled={!selectedPath && !typedPath}
          >
            <Check size={16} aria-hidden />
            {t('file_browser.select')}
          </button>
        </>
      }
    >
      <div className={styles.pathRow}>
        <input
          type="text"
          className={styles.pathInput}
          value={typedPath}
          onChange={(event) => {
            setTypedPath(event.target.value)
            setSelectedPath(event.target.value)
          }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') {
              void navigate(typedPath)
            }
          }}
        />
        <button
          type="button"
          className={styles.secondaryButton}
          onClick={() => void navigate(typedPath)}
        >
          <ArrowRight size={16} aria-hidden />
        </button>
      </div>

      <div className={styles.upRow}>
        <button
          type="button"
          className={styles.secondaryButton}
          disabled={loading || parentPath === currentPath}
          onClick={() => void navigate(parentPath)}
        >
          <FolderUp size={16} aria-hidden />
          {t('file_browser.up')}
        </button>
      </div>

      {error && <Alert variant="danger">{error}</Alert>}

      {loading ? (
        <Spinner />
      ) : (
        <div className={styles.entries}>
          {entries.length === 0 && <div className={styles.empty}>{t('file_browser.empty')}</div>}
          {entries.map((entry) => (
            <button
              key={entry.path}
              type="button"
              className={`${styles.entry} ${selectedPath === entry.path ? styles.entryActive : ''}`}
              onClick={() => selectEntry(entry)}
              onDoubleClick={() => activateEntry(entry)}
            >
              {!currentPath && entry.type === 'directory' ? (
                <HardDrive size={16} aria-hidden />
              ) : entry.type === 'directory' ? (
                <Folder size={16} aria-hidden />
              ) : (
                <FileText size={16} aria-hidden />
              )}
              <span className={styles.entryName}>{entry.name}</span>
            </button>
          ))}
        </div>
      )}
    </Modal>
  )
}
