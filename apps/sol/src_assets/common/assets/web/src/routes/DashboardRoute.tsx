/**
 * @file Dashboard route (legacy `index.html`): fatal error banner, Windows
 * driver warnings, version comparison with GitHub releases, resource links.
 */

import { useQuery } from '@tanstack/react-query'
import { useMemo } from 'react'
import { useTranslation } from 'react-i18next'
import { type GithubRelease, githubReleaseSchema } from '../api/schemas'
import { Markdown } from '../components/Markdown'
import { ResourceCard } from '../components/ResourceCard'
import { Alert, Card, LinkButton, Spinner } from '../components/ui'
import { parseLogEntries } from '../lib/logs'
import { SolVersion } from '../lib/version'
import {
  logsQuery,
  releaseQuery,
  virtualInputLicenseQuery,
  virtualInputStatusQuery,
} from '../queries'
import { useConfigValues } from './configValues'
import { Page } from './shell'

/**
 * @brief Dashboard page component.
 * @returns The page element.
 */
export default function DashboardRoute() {
  return (
    <Page>
      <DashboardContent />
    </Page>
  )
}

/**
 * @brief Dashboard body once the config is loaded.
 * @returns The dashboard content.
 */
function DashboardContent() {
  const { t } = useTranslation()
  const { platform, version, config } = useConfigValues()

  return (
    <>
      <h1>{t('index.welcome')}</h1>
      <p>{t('index.description')}</p>

      <FatalLogsAlert />
      {platform === 'windows' && <VirtualInputWarnings />}

      <VersionCard
        installedVersion={version ?? ''}
        notifyPreReleases={config.notify_pre_releases !== 'disabled'}
      />

      <ResourceCard />
    </>
  )
}

/**
 * @brief Banner listing Fatal log entries with a link to the logs.
 * @returns The alert element, or null when there are no fatal entries.
 */
function FatalLogsAlert() {
  const { t } = useTranslation()
  const { data: logs } = useQuery(logsQuery())
  const fatalEntries = useMemo(
    () => parseLogEntries(logs ?? '').filter((entry) => entry.level === 'Fatal'),
    [logs],
  )

  if (fatalEntries.length === 0) {
    return null
  }

  return (
    <Alert variant="danger" title={t('index.startup_errors')}>
      <ul>
        {fatalEntries.map((entry) => (
          <li key={entry.index}>{entry.raw}</li>
        ))}
      </ul>
      <LinkButton href="./troubleshooting#logs" variant="danger">
        View Logs
      </LinkButton>
    </Alert>
  )
}

/**
 * @brief Windows-only warnings for the virtual input drivers and license.
 * @returns The warning alerts.
 */
function VirtualInputWarnings() {
  const { t } = useTranslation()
  const statusQuery = useQuery(virtualInputStatusQuery())
  const licenseQuery = useQuery(virtualInputLicenseQuery())

  if (statusQuery.isPending || licenseQuery.isPending) {
    return <Spinner />
  }

  const virtualhid = statusQuery.data?.virtualhid
  const vigembus = statusQuery.data?.vigembus
  const license = licenseQuery.data

  const warnings: React.JSX.Element[] = []

  if (virtualhid && (!virtualhid.installed || !virtualhid.version_compatible)) {
    warnings.push(
      !virtualhid.installed ? (
        <Alert key="virtualhid" variant="warning" title={t('index.virtualhid_not_installed_title')}>
          {t('index.virtualhid_not_installed_desc')}
          <div>
            <LinkButton href="./troubleshooting#virtualhid" variant="warning">
              {t('index.fix_now')}
            </LinkButton>
          </div>
        </Alert>
      ) : (
        <Alert key="virtualhid" variant="warning" title={t('index.virtualhid_outdated_title')}>
          {t('index.virtualhid_outdated_desc', {
            version: virtualhid.version,
            supported_versions: virtualhid.supported_versions,
          })}
          <div>
            <LinkButton href="./troubleshooting#virtualhid" variant="warning">
              {t('index.fix_now')}
            </LinkButton>
          </div>
        </Alert>
      ),
    )
  }

  if (vigembus?.installed && virtualhid && !virtualhid.installed) {
    warnings.push(
      <Alert
        key="vigembus"
        variant="warning"
        title={t('index.virtualhid_missing_vigembus_installed_title')}
      >
        {t('index.virtualhid_missing_vigembus_installed_desc')}
      </Alert>,
    )
  }

  if (license && !license.licensed) {
    warnings.push(
      <Alert key="license" variant="warning" title={t('index.virtualhid_license_required_title')}>
        {t('index.virtualhid_license_required_desc')}
        <div>
          <LinkButton href="./troubleshooting#virtualhid-license" variant="warning">
            {t('index.fix_now')}
          </LinkButton>
        </div>
      </Alert>,
    )
  }

  return <>{warnings}</>
}

export interface VersionCardProps {
  /** The running Sol version. */
  installedVersion: string
  /** Whether pre-release notifications are enabled. */
  notifyPreReleases: boolean
}

/**
 * @brief Version card comparing the running build with GitHub releases.
 * @param props Card props.
 * @returns The card element.
 */
function VersionCard({ installedVersion, notifyPreReleases }: VersionCardProps) {
  const { t } = useTranslation()
  const installed = useMemo(() => new SolVersion(installedVersion), [installedVersion])
  const latest = useQuery(releaseQuery('LizardByte/Sunshine'))
  const allReleases = useQuery({
    queryKey: ['releases', 'all'],
    queryFn: async (): Promise<GithubRelease[]> => {
      const response = await fetch('https://api.github.com/repos/LizardByte/Sunshine/releases')
      if (!response.ok) {
        throw new Error(`GitHub returned ${response.status}`)
      }
      return githubReleaseSchema.array().parse(await response.json())
    },
  })

  const githubVersion = latest.data ? new SolVersion(latest.data.tag_name) : null
  const preReleaseVersion = useMemo<GithubRelease | null>(() => {
    return allReleases.data?.find((release) => release.prerelease) ?? null
  }, [allReleases.data])

  const installedNotStable =
    githubVersion !== null && githubVersion !== undefined && installed.isGreater(githubVersion)
  const stableAvailable = githubVersion?.isGreater(installed) ?? false
  const preReleaseAvailable =
    notifyPreReleases &&
    preReleaseVersion !== null &&
    installed.isGreater(preReleaseVersion.tag_name) &&
    githubVersion !== null &&
    preReleaseVersion !== undefined &&
    new SolVersion(preReleaseVersion.tag_name).isGreater(githubVersion)

  return (
    <Card>
      <h2>Version {installed.version}</h2>

      {(latest.isPending || allReleases.isPending) && <div>{t('index.loading_latest')}</div>}

      {installed.isDirty() && <Alert variant="success">{t('index.version_dirty')} 🌇</Alert>}

      {installedNotStable && (
        <Alert variant="info">{t('index.installed_version_not_stable')}</Alert>
      )}

      {!preReleaseAvailable && !stableAvailable && !installed.isDirty() && !installedNotStable && (
        <Alert variant="success">{t('index.version_latest')}</Alert>
      )}

      {preReleaseAvailable && preReleaseVersion && (
        <Alert variant="warning">
          <div className="updateHeader">
            <span>{t('index.new_pre_release')}</span>
            <strong>{preReleaseVersion.name ?? preReleaseVersion.tag_name}</strong>
            <LinkButton href={preReleaseVersion.html_url ?? '#'} variant="success">
              {t('index.download')}
            </LinkButton>
          </div>
          <div className="releaseNotes">
            <Markdown>{preReleaseVersion.body ?? ''}</Markdown>
          </div>
        </Alert>
      )}

      {stableAvailable && latest.data && (
        <Alert variant="warning">
          <div className="updateHeader">
            <span>{t('index.new_stable')}</span>
            <strong>{latest.data.name ?? latest.data.tag_name}</strong>
            <LinkButton href={latest.data.html_url ?? '#'} variant="success">
              {t('index.download')}
            </LinkButton>
          </div>
          <div className="releaseNotes">
            <Markdown>{latest.data.body ?? ''}</Markdown>
          </div>
        </Alert>
      )}
    </Card>
  )
}
