/**
 * @file First-run setup route (legacy `welcome.html`): create credentials.
 * This page is served without authentication and performs no API reads.
 */

import { type FormEvent, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { savePassword } from '../api/endpoints'
import { TextField } from '../components/fields'
import { ResourceCard } from '../components/ResourceCard'
import { Alert, Button, Card } from '../components/ui'
import { SimplePage } from './shell'

/**
 * @brief Welcome/setup page component.
 * @returns The page element.
 */
export default function WelcomeRoute() {
  const { t } = useTranslation()
  const [username, setUsername] = useState('sunshine')
  const [password, setPassword] = useState('')
  const [confirmPassword, setConfirmPassword] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [success, setSuccess] = useState(false)
  const [loading, setLoading] = useState(false)

  /**
   * @brief Submits the new credentials and reloads on success.
   * @param event The submit event.
   */
  const onSubmit = async (event: FormEvent) => {
    event.preventDefault()
    setError(null)
    setLoading(true)
    try {
      const body = await savePassword({
        newUsername: username,
        newPassword: password,
        confirmNewPassword: confirmPassword,
      })
      if (body.status === true) {
        setSuccess(true)
        window.setTimeout(() => {
          window.location.reload()
        }, 5000)
      } else {
        setError(body.error ?? 'Unknown error')
      }
    } catch {
      setError('Internal Server Error')
    } finally {
      setLoading(false)
    }
  }

  return (
    <SimplePage>
      <h1>{t('welcome.greeting')}</h1>
      <p>{t('welcome.create_creds')}</p>
      <div className="welcomeColumns">
        <Card title={t('welcome.create_creds_alert')}>
          <form onSubmit={(event) => void onSubmit(event)}>
            <TextField
              id="usernameInput"
              label={t('_common.username')}
              value={username}
              onChange={setUsername}
              autoComplete="username"
            />
            <TextField
              id="passwordInput"
              label={t('_common.password')}
              value={password}
              onChange={setPassword}
              type="password"
              autoComplete="new-password"
              required
            />
            <TextField
              id="confirmPasswordInput"
              label={t('welcome.confirm_password')}
              value={confirmPassword}
              onChange={setConfirmPassword}
              type="password"
              autoComplete="new-password"
              required
            />
            <Button type="submit" disabled={loading}>
              {t('welcome.login')}
            </Button>
            {error && (
              <div className="formMargin">
                <Alert variant="danger" title={t('_common.error')}>
                  {error}
                </Alert>
              </div>
            )}
            {success && (
              <div className="formMargin">
                <Alert variant="success" title={t('_common.success')}>
                  {t('welcome.welcome_success')}
                </Alert>
              </div>
            )}
          </form>
        </Card>
        <div>
          <ResourceCard />
        </div>
      </div>
    </SimplePage>
  )
}
