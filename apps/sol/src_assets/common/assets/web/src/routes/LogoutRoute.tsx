/**
 * @file Logout confirmation route (legacy `logout.html`).
 */

import { LogIn } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Card, LinkButton } from '../components/ui'
import { SimplePage } from './shell'

/**
 * @brief Logged-out page component.
 * @returns The page element.
 */
export default function LogoutRoute() {
  const { t } = useTranslation()
  return (
    <SimplePage>
      <div className="centeredNarrow">
        <Card>
          <div className="centered">
            <h1>{t('logout.logged_out')}</h1>
            <p>{t('logout.logged_out_desc')}</p>
            <LinkButton href="./">
              <LogIn size={18} aria-hidden />
              {t('logout.login')}
            </LinkButton>
          </div>
        </Card>
      </div>
    </SimplePage>
  )
}
