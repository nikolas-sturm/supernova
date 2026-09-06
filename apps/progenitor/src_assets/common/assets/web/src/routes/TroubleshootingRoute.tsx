/**
 * @file Troubleshooting route (legacy `troubleshooting.html`): virtual input
 * drivers, license management, force close, restart, display reset, client
 * unpairing, and the live log viewer.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  Check,
  ChevronDown,
  ChevronsDown,
  ChevronsUp,
  ChevronUp,
  Copy,
  Download,
  ExternalLink,
  KeyRound,
  RefreshCw,
  RotateCcw,
  Search,
  Trash2,
  XCircle,
} from 'lucide-react'
import { useEffect, useMemo, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  closeApp,
  resetDisplayDevicePersistence,
  restart,
  unpairAll,
  unpairClient,
  updateClient,
  updateVirtualInputLicense,
} from '../api/endpoints'
import type { DriverStatus } from '../api/schemas'
import { TextField } from '../components/fields'
import { Alert, Badge, Button, Card, LinkButton, Spinner } from '../components/ui'
import { filterLogs, parseLogEntries } from '../lib/logs'
import {
  clientsQuery,
  logsQuery,
  queryKeys,
  releaseQuery,
  virtualInputLicenseQuery,
  virtualInputStatusQuery,
} from '../queries'
import { useConfigValues } from './configValues'
import { Page } from './shell'
import styles from './TroubleshootingRoute.module.css'

/**
 * @brief Troubleshooting page component.
 * @returns The page element.
 */
export default function TroubleshootingRoute() {
  return (
    <Page>
      <TroubleshootingContent />
    </Page>
  )
}

/**
 * @brief Troubleshooting body once the shell is loaded.
 * @returns The page content.
 */
function TroubleshootingContent() {
  const { t } = useTranslation()
  const { platform } = useConfigValues()
  const queryClient = useQueryClient()

  const [closeAppStatus, setCloseAppStatus] = useState<boolean | null>(null)
  const [restartStatus, setRestartStatus] = useState(false)
  const [ddResetStatus, setDdResetStatus] = useState<boolean | null>(null)
  const [unpairAllStatus, setUnpairAllStatus] = useState<boolean | null>(null)
  const [unpairMessage, setUnpairMessage] = useState(false)

  const { data: clients } = useQuery(clientsQuery())

  const closeAppMutation = useMutation({
    mutationFn: closeApp,
    onSuccess: (body) => {
      setCloseAppStatus(body.status === true)
      window.setTimeout(() => setCloseAppStatus(null), 5000)
    },
  })

  const restartMutation = useMutation({
    mutationFn: restart,
    onSuccess: () => {
      setRestartStatus(true)
      window.setTimeout(() => setRestartStatus(false), 5000)
    },
  })

  const ddResetMutation = useMutation({
    mutationFn: resetDisplayDevicePersistence,
    onSuccess: (body) => {
      setDdResetStatus(body.status === true)
      window.setTimeout(() => setDdResetStatus(null), 5000)
    },
  })

  const unpairAllMutation = useMutation({
    mutationFn: unpairAll,
    onSuccess: (body) => {
      setUnpairAllStatus(body.status === true)
      window.setTimeout(() => setUnpairAllStatus(null), 5000)
      void queryClient.invalidateQueries({ queryKey: queryKeys.clients })
    },
  })

  const unpairMutation = useMutation({
    mutationFn: (uuid: string) => unpairClient(uuid),
    onSuccess: () => {
      setUnpairMessage(true)
      void queryClient.invalidateQueries({ queryKey: queryKeys.clients })
    },
  })

  const toggleMutation = useMutation({
    mutationFn: ({ uuid, enabled }: { uuid: string; enabled: boolean }) =>
      updateClient(uuid, enabled),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: queryKeys.clients })
    },
  })

  return (
    <>
      <h1>{t('troubleshooting.troubleshooting')}</h1>

      {platform === 'windows' && <VirtualInputCard />}

      <Card title={t('troubleshooting.force_close')}>
        <p>{t('troubleshooting.force_close_desc')}</p>
        {closeAppStatus === true && (
          <Alert variant="success">{t('troubleshooting.force_close_success')}</Alert>
        )}
        {closeAppStatus === false && (
          <Alert variant="danger">{t('troubleshooting.force_close_error')}</Alert>
        )}
        <Button
          variant="warning"
          disabled={closeAppMutation.isPending}
          onClick={() => closeAppMutation.mutate()}
        >
          <XCircle size={18} aria-hidden />
          {t('troubleshooting.force_close')}
        </Button>
      </Card>

      <Card title={t('troubleshooting.restart_sunshine')}>
        <p>{t('troubleshooting.restart_sunshine_desc')}</p>
        {restartStatus && (
          <Alert variant="success">{t('troubleshooting.restart_sunshine_success')}</Alert>
        )}
        <Button
          variant="warning"
          disabled={restartMutation.isPending}
          onClick={() => restartMutation.mutate()}
        >
          <RefreshCw size={18} aria-hidden />
          {t('troubleshooting.restart_sunshine')}
        </Button>
      </Card>

      {platform === 'windows' && (
        <Card title={t('troubleshooting.dd_reset')}>
          <p style={{ whiteSpace: 'pre-line' }}>{t('troubleshooting.dd_reset_desc')}</p>
          {ddResetStatus === true && (
            <Alert variant="success">{t('troubleshooting.dd_reset_success')}</Alert>
          )}
          {ddResetStatus === false && (
            <Alert variant="danger">{t('troubleshooting.dd_reset_error')}</Alert>
          )}
          <Button
            variant="warning"
            disabled={ddResetMutation.isPending}
            onClick={() => ddResetMutation.mutate()}
          >
            <RotateCcw size={18} aria-hidden />
            {t('troubleshooting.dd_reset')}
          </Button>
        </Card>
      )}

      <Card title={t('troubleshooting.unpair_title')}>
        <div className={styles.cardHeadRow}>
          <p>{t('troubleshooting.unpair_desc')}</p>
          <Button
            variant="danger"
            disabled={unpairAllMutation.isPending}
            onClick={() => unpairAllMutation.mutate()}
          >
            <Trash2 size={18} aria-hidden />
            {t('troubleshooting.unpair_all')}
          </Button>
        </div>
        {unpairMessage && (
          <Alert variant="success" title={t('_common.success')}>
            <div className={styles.cardHeadRow}>
              <span>{t('troubleshooting.unpair_single_success')}</span>
              <Button small variant="success" onClick={() => setUnpairMessage(false)}>
                {t('_common.dismiss')}
              </Button>
            </div>
          </Alert>
        )}
        {unpairAllStatus === true && (
          <Alert variant="success">{t('troubleshooting.unpair_all_success')}</Alert>
        )}
        {unpairAllStatus === false && (
          <Alert variant="danger">{t('troubleshooting.unpair_all_error')}</Alert>
        )}
      </Card>

      <Card>
        {clients !== undefined && clients.length > 0 ? (
          <ul className={styles.clientList}>
            {clients.map((client) => (
              <li key={client.uuid} className={styles.clientRow}>
                <div className={styles.clientName}>
                  {client.name !== '' ? client.name : t('troubleshooting.unpair_single_unknown')}
                </div>
                <label className={styles.switchLabel}>
                  <input
                    type="checkbox"
                    role="switch"
                    checked={client.enabled}
                    aria-checked={client.enabled}
                    onChange={(event) =>
                      toggleMutation.mutate({ uuid: client.uuid, enabled: event.target.checked })
                    }
                  />
                </label>
                <Button
                  small
                  variant="danger"
                  aria-label={t('troubleshooting.unpair_title')}
                  onClick={() => unpairMutation.mutate(client.uuid)}
                >
                  <Trash2 size={16} aria-hidden />
                </Button>
              </li>
            ))}
          </ul>
        ) : (
          <p className="mutedCenter">
            <em>{t('troubleshooting.unpair_single_no_devices')}</em>
          </p>
        )}
      </Card>

      <LogViewer />
    </>
  )
}

/**
 * @brief Windows virtual input driver status card with license management.
 * @returns The card element.
 */
function VirtualInputCard() {
  const { t } = useTranslation()
  const statusQuery = useQuery(virtualInputStatusQuery())
  const licenseQuery = useQuery(virtualInputLicenseQuery())
  const virtualhidRelease = useQuery(releaseQuery('LizardByte/libvirtualhid'))
  const vigembusRelease = useQuery(releaseQuery('nefarius/ViGEmBus'))
  const queryClient = useQueryClient()

  const [licenseKey, setLicenseKey] = useState('')
  const [licenseError, setLicenseError] = useState('')
  const [licenseBusy, setLicenseBusy] = useState(false)

  const license = licenseQuery.data
  const virtualhid: DriverStatus = statusQuery.data?.virtualhid ?? {
    installed: false,
    version: '',
    version_compatible: false,
    minimum_version: '',
    supported_versions: '',
  }
  const vigembus: DriverStatus = statusQuery.data?.vigembus ?? {
    installed: false,
    version: '',
    version_compatible: false,
    minimum_version: '',
    supported_versions: '',
  }
  const controllerEnabled = true

  /**
   * @brief Runs a license operation and refreshes the license query.
   * @param action The operation name.
   */
  const runLicenseAction = async (action: string) => {
    if (licenseBusy || (action === 'activate' && !licenseKey.trim())) {
      return
    }
    setLicenseBusy(true)
    setLicenseError('')
    try {
      await updateVirtualInputLicense(action, action === 'activate' ? licenseKey.trim() : undefined)
      await queryClient.invalidateQueries({ queryKey: queryKeys.virtualInputLicense })
      if (action === 'activate') {
        setLicenseKey('')
      }
    } catch (error) {
      setLicenseError(
        error instanceof Error
          ? error.message
          : t('troubleshooting.virtualhid_license_request_failed'),
      )
    } finally {
      setLicenseBusy(false)
    }
  }

  if (statusQuery.isPending || licenseQuery.isPending) {
    return <Spinner />
  }

  const licenseBadgeVariant =
    license?.state === 'licensed'
      ? 'success'
      : ['expired', 'disabled', 'invalid'].includes(license?.state ?? '')
        ? 'danger'
        : 'neutral'

  return (
    <Card title={t('troubleshooting.virtual_gamepad')}>
      <p>{t('troubleshooting.virtual_gamepad_desc')}</p>

      <section className={styles.section}>
        <div className={styles.sectionHead}>
          <h3>{t('troubleshooting.virtual_gamepad_drivers')}</h3>
          <p>{t('troubleshooting.virtual_gamepad_drivers_desc')}</p>
        </div>

        <div className={styles.tableWrap}>
          <table className="table">
            <thead>
              <tr>
                <th>{t('troubleshooting.driver_name')}</th>
                <th>{t('troubleshooting.driver_installed_version')}</th>
                <th>{t('troubleshooting.driver_latest_version')}</th>
                <th>{t('troubleshooting.driver_supported_versions')}</th>
                <th>{t('troubleshooting.driver_status')}</th>
                <th>
                  <span className="visuallyHidden">{t('troubleshooting.driver_download')}</span>
                </th>
              </tr>
            </thead>
            <tbody>
              <tr id="virtualhid">
                <th scope="row">{t('troubleshooting.virtualhid_driver')}</th>
                <td>{driverVersionText(virtualhid, t)}</td>
                <td>{releaseCell(virtualhidRelease, t)}</td>
                <td>{virtualhid.supported_versions}</td>
                <td>{driverStatusBadge(virtualhid, t)}</td>
                <td>
                  <LinkButton
                    href={
                      virtualhidRelease.data?.html_url ??
                      'https://github.com/LizardByte/libvirtualhid/releases/latest'
                    }
                    small
                  >
                    <Download size={16} aria-hidden />
                    {t('troubleshooting.driver_download')}
                  </LinkButton>
                </td>
              </tr>
              {controllerEnabled && (!virtualhid.installed || vigembus.installed) && (
                <tr id="vigembus">
                  <th scope="row">{t('troubleshooting.vigembus_driver')}</th>
                  <td>{driverVersionText(vigembus, t)}</td>
                  <td>{releaseCell(vigembusRelease, t)}</td>
                  <td>{vigembus.supported_versions}</td>
                  <td>{driverStatusBadge(vigembus, t)}</td>
                  <td>
                    <LinkButton
                      href={
                        vigembusRelease.data?.html_url ??
                        'https://github.com/nefarius/ViGEmBus/releases/latest'
                      }
                      small
                    >
                      <Download size={16} aria-hidden />
                      {t('troubleshooting.driver_download')}
                    </LinkButton>
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </section>

      <section className={styles.section} id="virtualhid-license">
        <div className={styles.sectionHead}>
          <h3>{t('troubleshooting.virtualhid_license')}</h3>
          <p>{t('troubleshooting.virtualhid_license_desc')}</p>
          <Badge variant={licenseBadgeVariant}>
            {t(`troubleshooting.virtualhid_license_state_${license?.state ?? 'unavailable'}`)}
          </Badge>
        </div>

        {licenseError && <Alert variant="danger">{licenseError}</Alert>}
        {license && !license.service_available && (
          <Alert variant="warning">
            {license.message || t('troubleshooting.virtualhid_license_unavailable')}
          </Alert>
        )}

        {license && (
          <>
            <dl className={styles.licenseStats}>
              {license.plan_name && (
                <>
                  <dt>{t('troubleshooting.virtualhid_license_plan')}</dt>
                  <dd>{license.plan_name}</dd>
                </>
              )}
              <dt>{t('troubleshooting.virtualhid_license_activations')}</dt>
              <dd>
                {license.activation_limit
                  ? `${license.activation_usage} / ${license.activation_limit}`
                  : t('troubleshooting.virtualhid_license_not_reported')}
              </dd>
              <dt>{t('troubleshooting.virtualhid_license_active_devices')}</dt>
              <dd>{license.active_devices}</dd>
              {license.customer_email && (
                <>
                  <dt>{t('troubleshooting.virtualhid_license_customer')}</dt>
                  <dd>{license.customer_email}</dd>
                </>
              )}
            </dl>

            <div className="buttonRow">
              <Button
                variant="secondary"
                disabled={licenseBusy || !license.service_available}
                onClick={() => void runLicenseAction('validate')}
              >
                <RefreshCw size={18} className={licenseBusy ? styles.spinning : ''} aria-hidden />
                {t('troubleshooting.virtualhid_license_refresh')}
              </Button>
              {license.manage_account_url && (
                <LinkButton href={license.manage_account_url} variant="secondary" outline>
                  <ExternalLink size={18} aria-hidden />
                  {t('troubleshooting.virtualhid_license_manage')}
                </LinkButton>
              )}
              {license.purchase_url && !license.licensed && (
                <LinkButton href={license.purchase_url} variant="success">
                  {t('troubleshooting.virtualhid_license_buy')}
                </LinkButton>
              )}
              {license.licensed && (
                <Button
                  variant="danger"
                  outline
                  disabled={licenseBusy || !license.service_available}
                  onClick={() => void runLicenseAction('deactivate')}
                >
                  {t('troubleshooting.virtualhid_license_deactivate')}
                </Button>
              )}
            </div>

            {!license.licensed ? (
              <div className={styles.licenseInput}>
                <TextField
                  id="virtualhid-license-key"
                  label={t('troubleshooting.virtualhid_license_key')}
                  value={licenseKey}
                  onChange={setLicenseKey}
                  type="password"
                  placeholder={t('troubleshooting.virtualhid_license_key_placeholder')}
                  description={t('troubleshooting.virtualhid_license_key_desc')}
                  disabled={licenseBusy || !license.service_available}
                  onKeyDown={(event) => {
                    if (event.key === 'Enter') {
                      void runLicenseAction('activate')
                    }
                  }}
                />
                <Button
                  disabled={licenseBusy || !license.service_available || !licenseKey.trim()}
                  onClick={() => void runLicenseAction('activate')}
                >
                  <KeyRound size={18} aria-hidden />
                  {t('troubleshooting.virtualhid_license_activate')}
                </Button>
              </div>
            ) : (
              <Alert
                variant="success"
                title={t('troubleshooting.virtualhid_license_machine_activated')}
              >
                {t('troubleshooting.virtualhid_license_machine_activated_desc')}
              </Alert>
            )}
          </>
        )}
      </section>
    </Card>
  )
}

/**
 * @brief Installed-version cell text for a driver row.
 * @param driver The driver status.
 * @param t Translation function.
 * @returns The localized version text.
 */
function driverVersionText(driver: DriverStatus, t: (key: string) => string): string {
  if (!driver.installed) {
    return t('troubleshooting.driver_not_installed')
  }
  return driver.version || t('troubleshooting.driver_version_unknown')
}

/**
 * @brief Latest-release cell for a driver row.
 * @param release The release query.
 * @param t Translation function.
 * @returns The rendered cell content.
 */
function releaseCell(
  release: { isPending: boolean; isError: boolean; data?: { tag_name: string } | undefined },
  t: (key: string) => string,
): React.JSX.Element {
  if (release.isPending) {
    return <span>{t('troubleshooting.driver_release_checking')}</span>
  }
  if (release.isError || release.data === undefined) {
    return <span>{t('troubleshooting.driver_release_unavailable')}</span>
  }
  return <span>{release.data.tag_name}</span>
}

/**
 * @brief Status badge for a driver row.
 * @param driver The driver status.
 * @param t Translation function.
 * @returns The badge element.
 */
function driverStatusBadge(driver: DriverStatus, t: (key: string) => string): React.JSX.Element {
  if (!driver.installed) {
    return <Badge variant="neutral">{t('troubleshooting.driver_status_not_installed')}</Badge>
  }
  return driver.version_compatible ? (
    <Badge variant="success">{t('troubleshooting.driver_status_compatible')}</Badge>
  ) : (
    <Badge variant="danger">{t('troubleshooting.driver_status_unsupported')}</Badge>
  )
}

/**
 * @brief Live log viewer with level highlighting, filter, and navigation.
 * @returns The logs card element.
 */
function LogViewer() {
  const { t } = useTranslation()
  const { data: logs } = useQuery(logsQuery())

  const [logFilter, setLogFilter] = useState('')
  const [copied, setCopied] = useState(false)
  const [currentIndex, setCurrentIndex] = useState(-1)
  const containerRef = useRef<HTMLDivElement>(null)

  const actualLogs = useMemo(() => filterLogs(logs ?? '', logFilter), [logs, logFilter])
  const entries = useMemo(() => parseLogEntries(actualLogs), [actualLogs])
  const warningIndices = useMemo(
    () =>
      entries
        .filter((entry) => ['Warning', 'Error', 'Fatal'].includes(entry.level))
        .map((entry) => entry.index),
    [entries],
  )

  // Reset the selection when the filter changes; setter is stable.
  // biome-ignore lint/correctness/useExhaustiveDependencies: only the filter change should reset selection
  useEffect(() => {
    setCurrentIndex(-1)
  }, [logFilter])

  /**
   * @brief Scrolls the log container to the selected entry.
   * @param index The entry index to reveal.
   */
  const revealEntry = (index: number) => {
    const container = containerRef.current
    if (!container) {
      return
    }
    const element = container.querySelector(`[data-entry-index="${index}"]`)
    if (!element) {
      return
    }
    const containerRect = container.getBoundingClientRect()
    const elementRect = element.getBoundingClientRect()
    container.scrollTop =
      container.scrollTop + (elementRect.top - containerRect.top) - containerRect.height * 0.15
  }

  /**
   * @brief Moves the selection to the previous/next warning or error.
   * @param direction The navigation direction.
   */
  const navigateLog = (direction: 'prev' | 'next') => {
    if (warningIndices.length === 0) {
      return
    }
    if (direction === 'next') {
      const next =
        currentIndex === -1
          ? warningIndices[0]
          : warningIndices.find((index) => index > currentIndex)
      if (next !== undefined) {
        setCurrentIndex(next)
        revealEntry(next)
      }
    } else {
      if (currentIndex === -1) {
        return
      }
      const previous = [...warningIndices].reverse().find((index) => index < currentIndex)
      if (previous !== undefined) {
        setCurrentIndex(previous)
        revealEntry(previous)
      }
    }
  }

  /**
   * @brief Copies the (filtered) logs to the clipboard.
   */
  const copyLogs = () => {
    void navigator.clipboard.writeText(actualLogs).then(() => {
      setCopied(true)
      window.setTimeout(() => setCopied(false), 2000)
    })
  }

  const hasPrev = warningIndices.some((index) => index < currentIndex)
  const hasNext =
    currentIndex === -1
      ? warningIndices.length > 0
      : warningIndices.some((index) => index > currentIndex)

  return (
    <Card title={t('troubleshooting.logs')}>
      <p>{t('troubleshooting.logs_desc')}</p>
      <div className={styles.logFilter}>
        <Search size={18} aria-hidden />
        <input
          type="text"
          value={logFilter}
          placeholder={t('troubleshooting.logs_find')}
          onChange={(event) => setLogFilter(event.target.value)}
        />
      </div>
      <div className={styles.logContainer} ref={containerRef}>
        <div className={styles.logNav}>
          <button
            type="button"
            title="Jump to Top"
            onClick={() => containerRef.current?.scrollTo({ top: 0, behavior: 'smooth' })}
          >
            <ChevronsUp size={18} aria-hidden />
          </button>
          <button
            type="button"
            title="Previous Warning/Error"
            disabled={!hasPrev}
            onClick={() => navigateLog('prev')}
          >
            <ChevronUp size={18} aria-hidden />
          </button>
          <button
            type="button"
            title="Next Warning/Error"
            disabled={!hasNext}
            onClick={() => navigateLog('next')}
          >
            <ChevronDown size={18} aria-hidden />
          </button>
          <button
            type="button"
            title="Jump to Bottom"
            onClick={() =>
              containerRef.current?.scrollTo({
                top: containerRef.current.scrollHeight,
                behavior: 'smooth',
              })
            }
          >
            <ChevronsDown size={18} aria-hidden />
          </button>
          <button type="button" title="Copy Logs" onClick={copyLogs}>
            {copied ? <Check size={18} aria-hidden /> : <Copy size={18} aria-hidden />}
          </button>
        </div>
        <pre className={styles.logPre}>
          {entries.map((entry) => (
            <span
              key={entry.index}
              data-entry-index={entry.index}
              className={`${styles[`log_${entry.level.toLowerCase()}`] ?? ''} ${
                entry.index === currentIndex ? styles.logSelected : ''
              }`}
            >
              {entry.raw}
              {'\n'}
            </span>
          ))}
        </pre>
      </div>
    </Card>
  )
}
