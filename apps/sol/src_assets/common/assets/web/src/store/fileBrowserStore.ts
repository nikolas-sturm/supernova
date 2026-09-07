/**
 * @file Global file browser modal state.
 *
 * Any component can open the shared file browser and await the selected path,
 * replacing the legacy callback-based approach with a promise.
 */

import { create } from 'zustand'

/** Filter applied to the server-side directory listing. */
export type BrowseType = 'any' | 'file' | 'directory' | 'executable'

/** Options accepted by `openFileBrowser`. */
export interface FileBrowserOptions {
  /** Entry filter type passed to `/api/browse`. */
  type: BrowseType
  /** Pre-filled starting path. */
  startPath?: string
}

interface FileBrowserState {
  /** True when the modal is visible. */
  open: boolean
  /** Current options. */
  options: FileBrowserOptions | null
  /** Promise resolver invoked with the chosen path, or null on cancel. */
  resolver: ((path: string | null) => void) | null
  /**
   * @brief Opens the file browser.
   * @param options Browser options.
   * @returns Resolves with the selected path, or null when cancelled.
   */
  show: (options: FileBrowserOptions) => Promise<string | null>
  /**
   * @brief Completes the browser interaction.
   * @param path The selected path or null when cancelled.
   */
  finish: (path: string | null) => void
}

export const useFileBrowserStore = create<FileBrowserState>()((set, get) => ({
  open: false,
  options: null,
  resolver: null,
  show: (options) => {
    get().resolver?.(null)
    return new Promise<string | null>((resolve) => {
      set({ open: true, options, resolver: resolve })
    })
  },
  finish: (path) => {
    const { resolver } = get()
    set({ open: false, options: null, resolver: null })
    resolver?.(path)
  },
}))

/**
 * @brief Opens the shared file browser modal.
 * @param options Browser options.
 * @returns Resolves with the selected path, or null when cancelled.
 */
export function openFileBrowser(options: FileBrowserOptions): Promise<string | null> {
  return useFileBrowserStore.getState().show(options)
}
