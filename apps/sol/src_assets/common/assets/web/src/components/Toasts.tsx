/**
 * @file Global toast host rendering the toast store.
 */

import { X } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useToastStore } from '../store/toastStore'
import styles from './Toasts.module.css'
import { AlertIcon } from './ui'

/**
 * @brief Renders all active toasts; mount once in the app root.
 * @returns The toast container element, or null when empty.
 */
export function Toasts() {
  const { t } = useTranslation()
  const toasts = useToastStore((state) => state.toasts)
  const dismiss = useToastStore((state) => state.dismiss)

  if (toasts.length === 0) {
    return null
  }

  return (
    <div className={styles.container} aria-live="polite">
      {toasts.map((toast) => (
        <div key={toast.id} className={`${styles.toast} ${styles[toast.variant]}`} role="alert">
          <AlertIcon variant={toast.variant} size={18} aria-hidden />
          <div className={styles.body}>
            {toast.title !== undefined && (
              <strong>{toast.title === '' ? toast.title : toast.title}</strong>
            )}
            <span>{toast.message}</span>
          </div>
          <button
            type="button"
            className={styles.close}
            aria-label={t('_common.dismiss')}
            onClick={() => dismiss(toast.id)}
          >
            <X size={16} />
          </button>
        </div>
      ))}
    </div>
  )
}
