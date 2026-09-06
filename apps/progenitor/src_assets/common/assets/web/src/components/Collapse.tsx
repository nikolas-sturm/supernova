/**
 * @file Collapsible section (replaces Bootstrap accordion).
 */

import { ChevronDown } from 'lucide-react'
import { type ReactNode, useId } from 'react'
import styles from './Collapse.module.css'

export interface CollapseProps {
  /** Section header content. */
  title: ReactNode
  /** Section body content. */
  children: ReactNode
  /** Whether the section starts expanded. */
  defaultOpen?: boolean
}

/**
 * @brief Toggleable disclosure section.
 * @param props Collapse props.
 * @returns The collapsible section element.
 */
export function Collapse({ title, children, defaultOpen = true }: CollapseProps) {
  const id = useId()
  return (
    <div className={styles.collapse}>
      <button
        type="button"
        className={styles.toggle}
        aria-expanded={defaultOpen}
        aria-controls={id}
      >
        <span>{title}</span>
        <ChevronDown size={18} className={styles.chevron} aria-hidden />
      </button>
      <div id={id} className={styles.body}>
        {children}
      </div>
    </div>
  )
}
