/**
 * @file Page shells providing the platform context and navigation.
 *
 * `Page` is for authenticated pages: it fetches the Sol config and
 * publishes the platform via context. `SimplePage` is for unauthenticated
 * pages (welcome, logout) and performs no API calls, matching the legacy
 * behavior that avoids triggering an HTTP auth prompt.
 */

import { useQuery } from '@tanstack/react-query'
import type { ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { AppNavbar, SimpleNavbar } from '../components/Navbar'
import { type Platform, PlatformContext } from '../components/Platform'
import { Alert, PageHeader, Spinner } from '../components/ui'
import { configQuery } from '../queries'

export interface PageProps {
  /** Page title (rendered in the standard header). */
  title?: ReactNode
  /** Page description. */
  description?: ReactNode
  /** Page content. */
  children: ReactNode
}

/**
 * @brief Authenticated page shell: navbar, platform context, standard header.
 * @param props Page props.
 * @returns The shell element.
 */
export function Page({ title, description, children }: PageProps) {
  const { t } = useTranslation()
  const { data: config, isPending, isError } = useQuery(configQuery())

  if (isPending) {
    return (
      <>
        <AppNavbar />
        <main className="page">
          <Spinner />
        </main>
      </>
    )
  }

  if (isError || !config) {
    return (
      <>
        <AppNavbar />
        <main className="page">
          <Alert variant="danger">{t('_common.error')}</Alert>
        </main>
      </>
    )
  }

  const platform = (config.platform ?? '') as Platform

  return (
    <PlatformContext.Provider value={platform}>
      <AppNavbar />
      <main className="page">
        {title !== undefined && <PageHeader title={title} description={description} />}
        {children}
      </main>
    </PlatformContext.Provider>
  )
}

export interface SimplePageProps {
  /** Page content. */
  children: ReactNode
}

/**
 * @brief Unauthenticated page shell with no API access.
 * @param props Page props.
 * @returns The shell element.
 */
export function SimplePage({ children }: SimplePageProps) {
  return (
    <>
      <SimpleNavbar />
      <main className="page simplePage">{children}</main>
    </>
  )
}
