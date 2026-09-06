/**
 * @file Password change route (legacy `password.html`).
 */

import { type FormEvent, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { savePassword } from '../api/endpoints'
import { TextField } from '../components/fields'
import { Alert, Button, Card } from '../components/ui'
import { Page } from './shell'

/**
 * @brief Password change page component.
 * @returns The page element.
 */
export default function PasswordRoute() {
  const { t } = useTranslation()
  const [currentUsername, setCurrentUsername] = useState('')
  const [currentPassword, setCurrentPassword] = useState('')
  const [newUsername, setNewUsername] = useState('')
  const [newPassword, setNewPassword] = useState('')
  const [confirmPassword, setConfirmPassword] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [success, setSuccess] = useState(false)

  /**
   * @brief Submits the credential change.
   * @param event The submit event.
   */
  const onSubmit = async (event: FormEvent) => {
    event.preventDefault()
    setError(null)
    try {
      const body = await savePassword({
        currentUsername,
        currentPassword,
        newUsername,
        newPassword,
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
    }
  }

  return (
    <Page title={t('password.password_change')} description={t('password.password_change_desc')}>
      <form onSubmit={(event) => void onSubmit(event)}>
        <Card>
          <div className="twoColumn">
            <section>
              <h3>{t('password.current_creds')}</h3>
              <TextField
                id="currentUsername"
                label={t('_common.username')}
                value={currentUsername}
                onChange={setCurrentUsername}
                required
              />
              <TextField
                id="currentPassword"
                label={t('_common.password')}
                value={currentPassword}
                onChange={setCurrentPassword}
                type="password"
                autoComplete="current-password"
              />
            </section>
            <section>
              <h3>{t('password.new_creds')}</h3>
              <TextField
                id="newUsername"
                label={t('_common.username')}
                value={newUsername}
                onChange={setNewUsername}
                description={t('password.new_username_desc')}
              />
              <TextField
                id="newPassword"
                label={t('_common.password')}
                value={newPassword}
                onChange={setNewPassword}
                type="password"
                autoComplete="new-password"
                required
              />
              <TextField
                id="confirmNewPassword"
                label={t('password.confirm_password')}
                value={confirmPassword}
                onChange={setConfirmPassword}
                type="password"
                autoComplete="new-password"
                required
              />
            </section>
          </div>
        </Card>
        {error && (
          <Alert variant="danger" title="Error:">
            {error}
          </Alert>
        )}
        {success && (
          <Alert variant="success" title={t('_common.success')}>
            {t('password.success_msg')}
          </Alert>
        )}
        <Button type="submit">{t('_common.save')}</Button>
      </form>
    </Page>
  )
}
