/**
 * @file Accessible modal dialog with promise-friendly API.
 */

import { Button } from '@supernova/design-system'
import { X } from 'lucide-react'
import { type ReactNode, useEffect, useRef } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import styles from './Modal.module.css'

export interface ModalProps {
  /** Whether the modal is visible. */
  open: boolean
  /** Title rendered in the header. */
  title: ReactNode
  /** Called when the modal should close (backdrop click, escape, close button). */
  onClose: () => void
  /** Modal width class. */
  size?: 'md' | 'lg' | 'xl'
  /** Footer content (action buttons). */
  footer?: ReactNode
  /** Modal body content. */
  children: ReactNode
}

/** Increasing z-index so stacked modals (e.g. file browser over app editor) layer correctly. */
let zIndexCounter = 1050

/**
 * @brief Modal dialog rendered in a portal with escape handling.
 * @param props Modal props.
 * @returns The modal portal, or null when closed.
 */
export function Modal({ open, title, onClose, size = 'lg', footer, children }: ModalProps) {
  const { t } = useTranslation()
  const dialogRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) {
      return
    }
    zIndexCounter += 10
    const z = zIndexCounter
    if (dialogRef.current) {
      dialogRef.current.style.zIndex = String(z)
    }
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.stopPropagation()
        onClose()
      }
    }
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('keydown', onKeyDown)
      zIndexCounter = Math.max(1050, z - 10)
    }
  }, [open, onClose])

  if (!open) {
    return null
  }

  return createPortal(
    <div className={styles.backdrop} onClick={onClose} role="presentation">
      <div
        ref={dialogRef}
        className={`${styles.dialog} ${styles[size]}`}
        role="dialog"
        aria-modal="true"
        onClick={(event) => event.stopPropagation()}
      >
        <header className={styles.header}>
          <h2 className={styles.title}>{title}</h2>
          <button
            type="button"
            className={styles.close}
            aria-label={t('_common.close')}
            onClick={onClose}
          >
            <X size={20} />
          </button>
        </header>
        <div className={styles.body}>{children}</div>
        {footer !== undefined && <footer className={styles.footer}>{footer}</footer>}
      </div>
    </div>,
    document.body,
  )
}

export interface ConfirmDialogProps {
  /** Whether the dialog is visible. */
  open: boolean
  /** Dialog title. */
  title: ReactNode
  /** Dialog body message. */
  message: ReactNode
  /** Confirm button label. */
  confirmLabel: ReactNode
  /** Called on confirm. */
  onConfirm: () => void
  /** Called on cancel/close. */
  onCancel: () => void
  /** Confirm button variant. */
  confirmVariant?: 'primary' | 'danger'
}

/**
 * @brief Simple confirmation dialog.
 * @param props Dialog props.
 * @returns The dialog element.
 */
export function ConfirmDialog({
  open,
  title,
  message,
  confirmLabel,
  onConfirm,
  onCancel,
  confirmVariant = 'danger',
}: ConfirmDialogProps) {
  const { t } = useTranslation()
  return (
    <Modal
      open={open}
      title={title}
      onClose={onCancel}
      size="md"
      footer={
        <>
          <Button variant="secondary" onClick={onCancel}>
            {t('_common.cancel')}
          </Button>
          <Button variant={confirmVariant} onClick={onConfirm}>
            {confirmLabel}
          </Button>
        </>
      }
    >
      <p>{message}</p>
    </Modal>
  )
}
