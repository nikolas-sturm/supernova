/**
 * @file Configuration route (legacy `config.html`): tabbed settings editor
 * with search, default-stripping save, and apply/restart flow.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useNavigate, useSearch } from '@tanstack/react-router'
import {
  Check,
  Cpu,
  FileCog,
  Gamepad2,
  Gpu,
  type LucideIcon,
  Network as NetworkIcon,
  Save,
  Search,
  Settings,
  Sliders,
  Volume2,
} from 'lucide-react'
import { useEffect, useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { saveConfig } from '../api/endpoints'
import { configSchema } from '../api/schemas'
import { Alert, Button, Card, Spinner } from '../components/ui'
import { configQuery, queryKeys } from '../queries'
import styles from './ConfigRoute.module.css'
import {
  buildOptionIndex,
  CONFIG_TABS,
  type ConfigTab,
  ENCODER_TAB_IDS,
  populateConfigDraft,
  stripDefaultValues,
  tabsForPlatform,
} from './config/defaults'
import { TabContent } from './config/TabContent'
import { Page } from './shell'

/** The visible tab list is computed once the platform is known. */
export interface ConfigTabsContextValue {
  tabs: ConfigTab[]
  draft: Record<string, unknown>
  setDraftValue: (key: string, value: unknown) => void
  platform: string
}

/**
 * @brief Configuration page component.
 * @returns The page element.
 */
export default function ConfigRoute() {
  return (
    <Page>
      <ConfigContent />
    </Page>
  )
}

/**
 * @brief Configuration body once the config query has data.
 * @returns The page content.
 */
function ConfigContent() {
  const { t } = useTranslation()
  const queryClient = useQueryClient()
  const navigate = useNavigate()
  const search = useSearch({ strict: false }) as { tab?: string }
  const { data: rawConfig, isPending } = useQuery({
    ...configQuery(),
    select: (data) => configSchema.parse(data),
  })

  const [draft, setDraft] = useState<Record<string, unknown> | null>(null)
  const [saved, setSaved] = useState(false)
  const [restarted, setRestarted] = useState(false)
  const [searchQuery, setSearchQuery] = useState('')

  const tabs = useMemo(
    () => (rawConfig ? tabsForPlatform(CONFIG_TABS, rawConfig.platform) : []),
    [rawConfig],
  )
  const currentTabId =
    search.tab && tabs.some((tab) => tab.id === search.tab)
      ? search.tab
      : (tabs[0]?.id ?? 'general')

  useEffect(() => {
    if (rawConfig && draft === null) {
      setDraft(populateConfigDraft(rawConfig as unknown as Record<string, unknown>, tabs))
    }
  }, [rawConfig, draft, tabs])

  const saveMutation = useMutation({
    mutationFn: (payload: Record<string, unknown>) => saveConfig(payload),
    onSuccess: (body) => {
      if (body.status === true) {
        setSaved(true)
        void queryClient.invalidateQueries({ queryKey: queryKeys.config })
      }
    },
  })

  /**
   * @brief Saves only the values that differ from defaults.
   * @returns True when the save succeeded.
   */
  const save = async (): Promise<boolean> => {
    setSaved(false)
    setRestarted(false)
    if (!draft) {
      return false
    }
    const payload = stripDefaultValues(draft, tabs)
    const body = await saveMutation.mutateAsync(payload)
    return body.status === true
  }

  /**
   * @brief Saves and restarts Sol to apply the changes.
   */
  const apply = async () => {
    const success = await save()
    if (success) {
      setRestarted(true)
      window.setTimeout(() => {
        setSaved(false)
        setRestarted(false)
      }, 5000)
      await fetch('./api/restart', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
      })
    }
  }

  const optionIndex = useMemo(() => buildOptionIndex(tabs), [tabs])
  const searchResults = useMemo(() => {
    const query = searchQuery.trim().toLowerCase()
    if (!query) {
      return []
    }
    return optionIndex.filter(
      (option) =>
        option.key.toLowerCase().includes(query) || option.label.toLowerCase().includes(query),
    )
  }, [optionIndex, searchQuery])

  /**
   * @brief Jumps to the first search result: switches tab and scrolls to the field.
   */
  const jumpToFirstResult = () => {
    const first = searchResults[0]
    if (!first) {
      return
    }
    if (first.tabId !== currentTabId) {
      void navigate({ to: '/config', search: { tab: first.tabId } })
    }
    window.setTimeout(() => {
      const element = document.getElementById(first.key)
      if (element) {
        element.scrollIntoView({ behavior: 'smooth', block: 'center' })
        element.style.background = 'var(--color-primary-light)'
        window.setTimeout(() => {
          element.style.background = ''
        }, 3000)
      }
    }, 150)
  }

  if (isPending || !draft) {
    return <Spinner />
  }

  const generalTabs = tabs.filter((tab) => !ENCODER_TAB_IDS.has(tab.id))
  const encoderTabs = tabs.filter((tab) => ENCODER_TAB_IDS.has(tab.id))

  const tabIcon = (tabId: string): LucideIcon => {
    const map: Record<string, LucideIcon> = {
      general: Settings,
      input: Gamepad2,
      av: Volume2,
      network: NetworkIcon,
      files: FileCog,
      advanced: Sliders,
      nv: Gpu,
      amd: Gpu,
      qsv: Gpu,
      vaapi: Gpu,
      vt: Gpu,
      vulkan: Gpu,
      sw: Cpu,
    }
    return map[tabId] ?? Settings
  }

  const renderTabButton = (tab: ConfigTab) => {
    const Icon = tabIcon(tab.id)
    const active = tab.id === currentTabId
    return (
      <button
        key={tab.id}
        type="button"
        className={`${styles.tabButton} ${active ? styles.tabActive : ''}`}
        onClick={() => void navigate({ to: '/config', search: { tab: tab.id } })}
      >
        <Icon size={18} aria-hidden />
        {tab.name}
      </button>
    )
  }

  return (
    <>
      <div className="appsHeader">
        <h1>{t('config.configuration')}</h1>
        <p>{t('config.configuration_desc')}</p>
      </div>

      <div className={styles.searchRow}>
        <div className={styles.searchBox}>
          <Search size={18} aria-hidden />
          <input
            type="text"
            value={searchQuery}
            placeholder={t('config.search_options')}
            onChange={(event) => setSearchQuery(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Enter') {
                jumpToFirstResult()
              }
            }}
          />
        </div>
        {searchQuery &&
          (searchResults.length === 0 ? (
            <span className={styles.searchStatus}>No results for "{searchQuery}"</span>
          ) : (
            <span className={styles.searchStatus}>Found {searchResults.length} result(s)</span>
          ))}
      </div>

      <div className={styles.layout}>
        <nav className={styles.sidebar}>
          <div className={styles.navGroup}>{generalTabs.map(renderTabButton)}</div>
          {encoderTabs.length > 0 && (
            <>
              <div className={styles.navHeading}>{t('config.encoders')}</div>
              <div className={styles.navGroup}>{encoderTabs.map(renderTabButton)}</div>
            </>
          )}
          <div className={styles.actions}>
            <Button onClick={() => void save()}>
              <Save size={18} aria-hidden />
              {t('_common.save')}
            </Button>
            {saved && !restarted && (
              <Button variant="success" onClick={() => void apply()}>
                <Check size={18} aria-hidden />
                {t('_common.apply')}
              </Button>
            )}
          </div>
        </nav>

        <div className={styles.content}>
          {saved && !restarted && (
            <Alert variant="success" title={t('_common.success')}>
              {t('config.apply_note')}
            </Alert>
          )}
          {restarted && (
            <Alert variant="success" title={t('_common.success')}>
              {t('config.restart_note')}
            </Alert>
          )}
          <Card>
            <TabContent
              tabId={currentTabId}
              draft={draft}
              setDraftValue={(key, value) => setDraft((current) => ({ ...current, [key]: value }))}
              platform={rawConfig?.platform ?? ''}
            />
          </Card>
        </div>
      </div>
    </>
  )
}
