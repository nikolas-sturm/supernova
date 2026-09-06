/** @file Eclipse shell tests: retain admin destinations and mobile navigation. */
import { cleanup, fireEvent, render, screen } from '@testing-library/react'
import type { ReactNode } from 'react'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { AppNavbar, SimpleNavbar } from './Navbar'

vi.mock('@tanstack/react-router', () => ({
  Link: ({ to, children, ...props }: { to: string; children: ReactNode }) => (
    <a href={to} {...props}>
      {children}
    </a>
  ),
  useRouterState: ({ select }: { select: (state: { location: { pathname: string } }) => string }) =>
    select({ location: { pathname: '/clients' } }),
}))

afterEach(cleanup)

describe('Eclipse admin navigation', () => {
  it('retains admin destinations and marks the current client-management page', () => {
    render(<AppNavbar />)
    const destinations = screen.getAllByRole('link').map((link) => link.getAttribute('href'))
    for (const path of [
      '/',
      '/pin',
      '/clients',
      '/apps',
      '/featured',
      '/config',
      '/troubleshooting',
    ]) {
      expect(destinations).toContain(path)
    }
    expect(screen.getByRole('link', { name: 'clients.title' })).toHaveAttribute(
      'aria-current',
      'page',
    )
    expect(screen.getByText('ECLIPSE')).toBeInTheDocument()
    fireEvent.click(screen.getByRole('button', { name: 'User menu' }))
    expect(screen.getByRole('menuitem', { name: /navbar.password/ })).toHaveAttribute(
      'href',
      '/password',
    )
    expect(screen.getByRole('menuitem', { name: /navbar.logout/ })).toBeEnabled()
  })

  it('updates mobile navigation state and retains the shared theme control', () => {
    render(<AppNavbar />)
    // jsdom applies desktop CSS but does not evaluate mobile media queries.
    const toggle = screen.getByLabelText('Toggle navigation')
    expect(toggle).toHaveAttribute('aria-expanded', 'false')
    fireEvent.click(toggle)
    expect(toggle).toHaveAttribute('aria-expanded', 'true')
    expect(screen.getByRole('combobox')).toBeInTheDocument()
  })

  it('keeps unauthenticated chrome free of admin links', () => {
    render(<SimpleNavbar />)
    expect(screen.getByText('ECLIPSE')).toBeInTheDocument()
    expect(screen.queryByRole('link')).not.toBeInTheDocument()
    expect(screen.getByRole('combobox')).toBeInTheDocument()
  })
})
