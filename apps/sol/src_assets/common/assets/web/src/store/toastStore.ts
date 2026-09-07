/**
 * @file Global toast notification state.
 * Ported from the legacy `Notification.vue` singleton.
 */

import { create } from 'zustand'

/** Toast variants mapped to theme alert colors. */
export type ToastVariant = 'danger' | 'warning' | 'success' | 'info'

/** A single toast notification. */
export interface Toast {
  /** Unique identifier. */
  id: number
  /** Visual variant. */
  variant: ToastVariant
  /** Bold title (already localized). */
  title?: string | undefined
  /** Body message (already localized). */
  message: string
}

interface ToastState {
  /** Currently visible toasts. */
  toasts: Toast[]
  /**
   * @brief Pushes a toast.
   * @param toast Toast without id.
   * @returns The generated id.
   */
  push: (toast: Omit<Toast, 'id'>) => number
  /**
   * @brief Removes a toast by id.
   * @param id The id to dismiss.
   */
  dismiss: (id: number) => void
}

let nextId = 1

export const useToastStore = create<ToastState>()((set) => ({
  toasts: [],
  push: (toast) => {
    const id = nextId++
    set((state) => ({ toasts: [...state.toasts, { ...toast, id }] }))
    window.setTimeout(() => {
      useToastStore.getState().dismiss(id)
    }, 8000)
    return id
  },
  dismiss: (id) => {
    set((state) => ({ toasts: state.toasts.filter((toast) => toast.id !== id) }))
  },
}))

/**
 * @brief Convenience helpers mirroring the legacy `notify` API.
 */
export const notify = {
  /**
   * @param message Body message.
   * @param title Optional bold title.
   */
  error: (message: string, title?: string) =>
    useToastStore.getState().push({ variant: 'danger', message, title }),
  /**
   * @param message Body message.
   * @param title Optional bold title.
   */
  warning: (message: string, title?: string) =>
    useToastStore.getState().push({ variant: 'warning', message, title }),
  /**
   * @param message Body message.
   * @param title Optional bold title.
   */
  success: (message: string, title?: string) =>
    useToastStore.getState().push({ variant: 'success', message, title }),
  /**
   * @param message Body message.
   * @param title Optional bold title.
   */
  info: (message: string, title?: string) =>
    useToastStore.getState().push({ variant: 'info', message, title }),
}
