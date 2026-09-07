/**
 * @file Application navigation bars.
 *
 * `AppNavbar` is the full navigation for authenticated pages; `SimpleNavbar`
 * is the minimal bar used by the welcome and logout pages.
 */

import { TerraBrand } from '@supernova/design-system'
import { Link, useRouterState } from '@tanstack/react-router'
import {
  CircleUserRound,
  Home,
  Info,
  Layers,
  Lock,
  LogOut,
  Settings,
  Shield,
  Star,
} from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import styles from './Navbar.module.css'
import { ThemeMenu } from './ThemeMenu'

/** Primary navigation entries. */
const navItems = [
  { to: '/', labelKey: 'navbar.home', icon: Home },
  { to: '/pin', labelKey: 'navbar.pin', icon: Lock },
  { to: '/clients', labelKey: 'clients.title', icon: CircleUserRound },
  { to: '/apps', labelKey: 'navbar.applications', icon: Layers },
  { to: '/featured', labelKey: 'navbar.featured', icon: Star },
  { to: '/config', labelKey: 'navbar.configuration', icon: Settings },
  { to: '/troubleshooting', labelKey: 'navbar.troubleshoot', icon: Info },
] as const

/**
 * @brief Logs out by triggering the browser's basic-auth re-prompt.
 *
 * Ported verbatim from the legacy navbar: an XHR with changing fake
 * credentials invalidates the cached basic-auth session, after which the
 * browser navigates to the logout page.
 */
export function logout(): void {
  const cacheBuster = Date.now().toString()
  const logoutPageUrl = new URL('/logout', window.location.href)
  const finish = () => {
    window.location.replace(logoutPageUrl.toString())
  }
  const request = new XMLHttpRequest()
  request.open('GET', '/', true, 'sol-logout', cacheBuster)
  request.setRequestHeader('Cache-Control', 'no-store')
  request.onload = finish
  request.onerror = finish
  request.ontimeout = finish
  request.timeout = 5000
  request.send()
}

/**
 * @brief Full navigation bar with route links, theme menu, and user menu.
 * @returns The navbar element.
 */
export function AppNavbar() {
  const { t } = useTranslation()
  const pathname = useRouterState({ select: (state) => state.location.pathname })
  const [userMenuOpen, setUserMenuOpen] = useState(false)
  const [mobileOpen, setMobileOpen] = useState(false)
  const userMenuRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!userMenuOpen) {
      return
    }
    const onPointerDown = (event: PointerEvent) => {
      if (userMenuRef.current && !userMenuRef.current.contains(event.target as Node)) {
        setUserMenuOpen(false)
      }
    }
    document.addEventListener('pointerdown', onPointerDown)
    return () => document.removeEventListener('pointerdown', onPointerDown)
  }, [userMenuOpen])

  // Close menus when the route changes; setters are stable and intentionally omitted.
  // biome-ignore lint/correctness/useExhaustiveDependencies: route change closes menus once
  useEffect(() => {
    setMobileOpen(false)
    setUserMenuOpen(false)
  }, [pathname])

  return (
    <nav className={styles.navbar} aria-label="Primary navigation">
      <div className={styles.inner}>
        <Link to="/" className={styles.brand} title="Terra / Sol host">
          <TerraBrand subtitle="SOL / HOST ADMIN" />
        </Link>
        <button
          type="button"
          className={styles.mobileToggle}
          aria-expanded={mobileOpen}
          aria-label="Toggle navigation"
          onClick={() => setMobileOpen((value) => !value)}
        >
          <Layers size={20} />
        </button>
        <div className={`${styles.links} ${mobileOpen ? styles.linksOpen : ''}`}>
          <ul className={styles.navList}>
            {navItems.map((item) => {
              const active = item.to === '/' ? pathname === '/' : pathname.startsWith(item.to)
              const Icon = item.icon
              return (
                <li key={item.to}>
                  <Link
                    to={item.to}
                    aria-current={active ? 'page' : undefined}
                    className={`${styles.link} ${active ? styles.active : ''}`}
                  >
                    <Icon size={18} aria-hidden />
                    {t(item.labelKey)}
                  </Link>
                </li>
              )
            })}
          </ul>
          <div className={styles.right}>
            <ThemeMenu />
            <div className={styles.userWrap} ref={userMenuRef}>
              <button
                type="button"
                className={styles.userButton}
                aria-haspopup="menu"
                aria-expanded={userMenuOpen}
                aria-label="User menu"
                title="User menu"
                onClick={() => setUserMenuOpen((value) => !value)}
              >
                <CircleUserRound size={20} aria-hidden />
              </button>
              {userMenuOpen && (
                <div className={styles.userMenu} role="menu">
                  <Link to="/password" className={styles.userItem} role="menuitem">
                    <Shield size={18} aria-hidden />
                    {t('navbar.password')}
                  </Link>
                  <hr className={styles.divider} />
                  <button
                    type="button"
                    className={styles.userItem}
                    role="menuitem"
                    onClick={logout}
                  >
                    <LogOut size={18} aria-hidden />
                    {t('navbar.logout')}
                  </button>
                </div>
              )}
            </div>
          </div>
        </div>
      </div>
    </nav>
  )
}

/**
 * @brief Minimal navbar with logo and theme menu only.
 * @returns The navbar element.
 */
export function SimpleNavbar() {
  return (
    <nav className={`${styles.navbar} ${styles.simple}`} aria-label="Primary navigation">
      <div className={styles.inner}>
        <span className={styles.brand} title="Terra / Sol host">
          <TerraBrand subtitle="SOL / HOST ADMIN" />
        </span>
        <ThemeMenu />
      </div>
    </nav>
  )
}
