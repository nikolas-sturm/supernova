/**
 * @file Cover art finder modal backed by the LizardByte GameDB.
 */

import { Search } from 'lucide-react'
import { type FormEvent, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { uploadCover } from '../api/endpoints'
import { type CoverCandidate, searchCovers } from '../lib/covers'
import styles from './CoverFinder.module.css'
import { Modal } from './Modal'
import { Spinner } from './ui'

export interface CoverFinderModalProps {
  /** Whether the modal is visible. */
  open: boolean
  /** Default search term (usually the app name). */
  defaultQuery: string
  /** Called with the persisted cover path after a successful upload. */
  onCoverSelected: (path: string) => void
  /** Called when the modal should close. */
  onClose: () => void
}

/**
 * @brief Modal that searches GameDB covers and uploads the chosen one.
 *
 * The parent mounts this component only while it should be visible.
 * A search for the app name runs automatically on mount.
 * @param props Modal props.
 * @returns The modal element, or null when closed.
 */
export function CoverFinderModal({
  open,
  defaultQuery,
  onCoverSelected,
  onClose,
}: CoverFinderModalProps) {
  const { t } = useTranslation()
  const [query, setQuery] = useState('')
  const [searching, setSearching] = useState(false)
  const [candidates, setCandidates] = useState<CoverCandidate[]>([])
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    if (!open) {
      return
    }
    let cancelled = false
    setQuery('')
    setSearching(true)
    setCandidates([])
    searchCovers(defaultQuery)
      .then((results) => {
        if (!cancelled) {
          setCandidates(results)
        }
      })
      .catch((error) => console.error('Cover search failed', error))
      .finally(() => {
        if (!cancelled) {
          setSearching(false)
        }
      })
    return () => {
      cancelled = true
    }
  }, [open, defaultQuery])

  if (!open) {
    return null
  }

  /**
   * @brief Runs the cover search using the query or the app name.
   */
  const performSearch = async () => {
    setSearching(true)
    setCandidates([])
    try {
      const results = await searchCovers(query.trim() || defaultQuery)
      setCandidates(results)
    } catch (error) {
      console.error('Cover search failed', error)
    } finally {
      setSearching(false)
    }
  }

  /**
   * @brief Uploads the selected cover and applies its path.
   * @param cover The chosen candidate.
   */
  const applyCover = async (cover: CoverCandidate) => {
    setBusy(true)
    try {
      const path = await uploadCover(cover.key, cover.saveUrl)
      onCoverSelected(path)
      onClose()
    } catch (error) {
      console.error('Failed to download cover', error)
    } finally {
      setBusy(false)
    }
  }

  const onSubmit = (event: FormEvent) => {
    event.preventDefault()
    void performSearch()
  }

  return (
    <Modal
      open={open}
      title={
        searching
          ? t('apps.searching_covers')
          : candidates.length > 0
            ? `${t('apps.covers_found')} (${candidates.length})`
            : t('apps.no_covers_found')
      }
      onClose={onClose}
      size="xl"
    >
      <form className={styles.searchRow} onSubmit={onSubmit}>
        <input
          type="text"
          className={styles.searchInput}
          value={query}
          placeholder={defaultQuery}
          onChange={(event) => setQuery(event.target.value)}
        />
        <button type="submit" className={styles.searchButton}>
          <Search size={18} aria-hidden />
          {t('_common.search')}
        </button>
      </form>
      <div className={styles.hint}>
        <strong>{t('_common.note')}</strong> {t('apps.cover_search_hint')}{' '}
        <a href="https://www.igdb.com/" target="_blank" rel="noopener noreferrer">
          IGDB
        </a>
      </div>

      <div className={`${styles.results} ${busy ? styles.busy : ''}`}>
        {searching && <Spinner />}
        {candidates.map((cover) => (
          <button
            key={cover.key}
            type="button"
            className={styles.cover}
            onClick={() => void applyCover(cover)}
          >
            <img className={styles.coverImage} src={cover.url} alt={cover.name} loading="lazy" />
            <span className={styles.coverName}>{cover.name}</span>
          </button>
        ))}
      </div>
    </Modal>
  )
}
