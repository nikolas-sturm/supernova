/**
 * @file Applications route (legacy `apps.html`): app grid with search/sort,
 * editor modal, cover finder, file browser, and delete confirmation.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  ArrowDown,
  ArrowUp,
  ArrowUpDown,
  LayersPlus,
  Pencil,
  Search,
  Terminal,
  Trash2,
  X,
} from 'lucide-react'
import { useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { deleteApp } from '../api/endpoints'
import type { AppRecord } from '../api/schemas'
import { ConfirmDialog } from '../components/Modal'
import { Badge, Button, Card, Spinner } from '../components/ui'
import { appsQuery, queryKeys } from '../queries'
import { AppEditorModal } from './apps/AppEditorModal'
import { useConfigValues } from './configValues'
import { Page } from './shell'

/** Sort modes for the app grid. */
type SortMode = 'default' | 'asc' | 'desc'

/**
 * @brief Applications page component.
 * @returns The page element.
 */
export default function AppsRoute() {
  return (
    <Page>
      <AppsContent />
    </Page>
  )
}

/**
 * @brief Applications body once the shell is loaded.
 * @returns The page content.
 */
function AppsContent() {
  const { t } = useTranslation()
  const { platform, version } = useConfigValues()
  const queryClient = useQueryClient()
  const { data: apps, isPending } = useQuery(appsQuery())
  const list = apps ?? []

  const [searchQuery, setSearchQuery] = useState('')
  const [sortMode, setSortMode] = useState<SortMode>('default')
  const [editIndex, setEditIndex] = useState<number | null>(null)
  const [deleteTarget, setDeleteTarget] = useState<{ index: number; name: string } | null>(null)

  const deleteMutation = useMutation({
    mutationFn: (index: number) => deleteApp(index),
    onSuccess: () => {
      setDeleteTarget(null)
      void queryClient.invalidateQueries({ queryKey: queryKeys.apps })
    },
  })

  const displayedApps = useMemo(() => {
    const indexed = list.map((app, index) => ({ app, index }))
    const query = searchQuery.trim().toLowerCase()
    const filtered = query
      ? indexed.filter(({ app }) => (app.name ?? '').toLowerCase().includes(query))
      : indexed
    if (sortMode !== 'default') {
      filtered.sort((a, b) => {
        const result = (a.app.name ?? '').localeCompare(b.app.name ?? '', undefined, {
          sensitivity: 'base',
        })
        return sortMode === 'asc' ? result : -result
      })
    }
    return filtered
  }, [list, searchQuery, sortMode])

  const totalCount = list.length
  const shownCount = displayedApps.length
  const countLabel = shownCount === totalCount ? `${totalCount}` : `${shownCount} / ${totalCount}`

  const toggleSort = () => {
    setSortMode((mode) => (mode === 'default' ? 'asc' : mode === 'asc' ? 'desc' : 'default'))
  }

  /**
   * @brief Builds a fresh app template for creation.
   * @returns The new app record.
   */
  const newApp = (): AppRecord => ({
    name: '',
    output: '',
    cmd: '',
    index: -1,
    'exclude-global-prep-cmd': false,
    elevated: false,
    'auto-detach': true,
    'wait-all': true,
    'exit-timeout': 5,
    'prep-cmd': [],
    detached: [],
    'image-path': '',
  })

  const editApp = (index: number): AppRecord => {
    const source = structuredClone(list[index] ?? {})
    if (source['prep-cmd'] === undefined) {
      source['prep-cmd'] = []
    }
    if (source.detached === undefined) {
      source.detached = []
    }
    if (source['exclude-global-prep-cmd'] === undefined) {
      source['exclude-global-prep-cmd'] = false
    }
    if (source.elevated === undefined && platform === 'windows') {
      source.elevated = false
    }
    if (source['auto-detach'] === undefined) {
      source['auto-detach'] = true
    }
    if (source['wait-all'] === undefined) {
      source['wait-all'] = true
    }
    if (source['exit-timeout'] === undefined) {
      source['exit-timeout'] = 5
    }
    source.index = index
    return source
  }

  if (isPending) {
    return <Spinner />
  }

  return (
    <>
      <div className="appsHeader">
        <h1>
          {t('apps.applications_title')}
          {totalCount > 0 ? ` (${countLabel})` : ''}
        </h1>
        <p>{t('apps.applications_desc')}</p>
      </div>

      <div className="appsToolbar">
        <Button onClick={() => setEditIndex(-1)}>
          <LayersPlus size={18} aria-hidden />
          {t('apps.add_new')}
        </Button>
        {totalCount > 0 && (
          <div className="appsToolbarRight">
            <Button
              variant="secondary"
              outline={sortMode !== 'default'}
              onClick={toggleSort}
              aria-pressed={sortMode !== 'default'}
            >
              {sortMode === 'default' ? (
                <ArrowUpDown size={16} aria-hidden />
              ) : sortMode === 'asc' ? (
                <ArrowUp size={16} aria-hidden />
              ) : (
                <ArrowDown size={16} aria-hidden />
              )}
              {t('apps.sort_by_name')}
            </Button>
            <div className="searchBox">
              <input
                type="text"
                value={searchQuery}
                placeholder={t('apps.search_placeholder')}
                onChange={(event) => setSearchQuery(event.target.value)}
              />
              {searchQuery ? (
                <button
                  type="button"
                  aria-label={t('_common.close')}
                  onClick={() => setSearchQuery('')}
                >
                  <X size={16} aria-hidden />
                </button>
              ) : (
                <span className="searchIcon">
                  <Search size={16} aria-hidden />
                </span>
              )}
            </div>
          </div>
        )}
      </div>

      {displayedApps.length > 0 ? (
        <div className="appGrid">
          {displayedApps.map(({ app, index }) => (
            <div className="appCard" key={index}>
              <div className="appPoster">
                {app['image-path'] ? (
                  <img src={`/api/covers/${index}`} alt={app.name ?? ''} loading="lazy" />
                ) : (
                  <div className="appPosterPlaceholder">
                    <span>{(app.name ?? ' ').charAt(0).toUpperCase()}</span>
                  </div>
                )}
                <div className="appPosterOverlay">
                  {app.cmd && (
                    <div className="appOverlayRow" title={app.cmd}>
                      <Terminal size={14} aria-hidden />
                      <span>{app.cmd}</span>
                    </div>
                  )}
                  <div className="appOverlayBadges">
                    {app.elevated === true && (
                      <Badge variant="secondary">{t('apps.badge_admin')}</Badge>
                    )}
                    {app.detached?.length ? (
                      <Badge variant="secondary">{t('apps.badge_detached')}</Badge>
                    ) : null}
                    {app['prep-cmd']?.length ? (
                      <Badge variant="secondary">{t('apps.badge_app_prep')}</Badge>
                    ) : null}
                    {app['exclude-global-prep-cmd'] === true && (
                      <Badge variant="secondary">{t('apps.badge_no_global_prep')}</Badge>
                    )}
                    {app['auto-detach'] === true && (
                      <Badge variant="secondary">{t('apps.badge_auto_detach')}</Badge>
                    )}
                  </div>
                </div>
              </div>
              <div className="appCardMeta">
                <h3>{app.name}</h3>
                <div className="appCardActions">
                  <Button small onClick={() => setEditIndex(index)}>
                    <Pencil size={16} aria-hidden />
                    {t('apps.edit')}
                  </Button>
                  <Button
                    small
                    variant="danger"
                    onClick={() => setDeleteTarget({ index, name: app.name ?? '' })}
                  >
                    <Trash2 size={16} aria-hidden />
                  </Button>
                </div>
              </div>
            </div>
          ))}
        </div>
      ) : totalCount > 0 ? (
        <Card>
          <p className="mutedCenter">{t('apps.no_search_results')}</p>
        </Card>
      ) : (
        <Card>
          <p className="mutedCenter">{t('apps.no_applications')}</p>
        </Card>
      )}

      {editIndex !== null && (
        <AppEditorModal
          key={`${editIndex}`}
          app={editIndex === -1 ? newApp() : editApp(editIndex)}
          platform={platform}
          version={version ?? ''}
          onClose={() => setEditIndex(null)}
        />
      )}

      <ConfirmDialog
        open={deleteTarget !== null}
        title={t('apps.delete_title')}
        message={t('apps.delete_confirm', { name: deleteTarget?.name ?? '' })}
        confirmLabel={t('apps.delete')}
        onConfirm={() => {
          if (deleteTarget) {
            deleteMutation.mutate(deleteTarget.index)
          }
        }}
        onCancel={() => setDeleteTarget(null)}
      />
    </>
  )
}
