/** @file Dashboard account greeting and page decoration regressions. */
import { cleanup, render, screen } from '@testing-library/react'
import type { ReactNode } from 'react'
import { afterEach, describe, expect, it, vi } from 'vitest'
import en from '../../public/assets/locale/en.json'
import css from '../styles/global.css?raw'
import DashboardRoute from './DashboardRoute'

const account = vi.hoisted(() => ({ username: 'actual-account' as string | undefined }))
vi.mock('./configValues', () => ({
  useConfigValues: () => ({
    platform: 'linux',
    version: '0.0.0',
    config: { username: account.username, sunshine_name: 'Not the account' },
  }),
}))
vi.mock('./shell', () => ({ Page: ({ children }: { children: ReactNode }) => <>{children}</> }))
vi.mock('../components/ResourceCard', () => ({ ResourceCard: () => null }))
vi.mock('@tanstack/react-query', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@tanstack/react-query')>()),
  useQuery: () => ({ data: undefined, isPending: false }),
}))
vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, options?: { username?: string }) => {
      const text = en.index[key.replace('index.', '') as keyof typeof en.index] ?? key
      return text.replace('{{username}}', options?.username ?? '')
    },
  }),
}))

afterEach(() => {
  cleanup()
  account.username = 'actual-account'
})

describe('dashboard heading', () => {
  it('greets the authenticated account, not the product or host name', () => {
    render(<DashboardRoute />)
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Hello, actual-account!')
    expect(screen.queryByText('Hello, Sol!')).not.toBeInTheDocument()
  })

  it('does not invent an account name before updated backend metadata is available', () => {
    account.username = undefined
    render(<DashboardRoute />)
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent(/^Hello!$/)
  })

  it('has no generated host-administration label above page content', () => {
    expect(css).not.toMatch(/\.page::before/)
    expect(css).not.toContain('HOST ADMINISTRATION')
  })
})
