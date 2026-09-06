/**
 * @file Core UI primitives: buttons, cards, alerts, badges, spinner.
 */

import { buttonClassName, type Variant } from '@supernova/design-system'
import styles from '@supernova/design-system/primitives.module.css'
import { Link } from '@tanstack/react-router'
import { AlertCircle, AlertTriangle, CheckCircle, Info, type LucideProps } from 'lucide-react'
import type { HTMLAttributes, ReactNode } from 'react'
import { useTranslation } from 'react-i18next'

export type { ButtonProps, Variant } from '@supernova/design-system'
/** Visual variant shared by buttons and alerts. */
export { Button } from '@supernova/design-system'

export interface LinkButtonProps {
  /** External URL; takes precedence over `to`. */
  href?: string
  /** Internal route path for router navigation. */
  to?: string
  /** Visual variant. */
  variant?: Variant
  /** Outlined style instead of filled. */
  outline?: boolean
  /** Compact size. */
  small?: boolean
  /** Button content. */
  children: ReactNode
  /** Additional class names. */
  className?: string
  /** Title attribute. */
  title?: string
}

/**
 * @brief Anchor styled like {@link Button} for internal and external links.
 * @param props Link props with variant modifiers.
 * @returns The anchor element.
 */
export function LinkButton({
  href,
  to,
  variant = 'primary',
  outline = false,
  small = false,
  children,
  className,
  title,
}: LinkButtonProps) {
  const resolvedClassName = buttonClassName({ variant, outline, small, className })
  if (href !== undefined) {
    return (
      <a
        href={href}
        target="_blank"
        rel="noopener noreferrer"
        className={resolvedClassName}
        title={title}
      >
        {children}
      </a>
    )
  }
  return (
    <Link to={to ?? '/'} className={resolvedClassName} title={title}>
      {children}
    </Link>
  )
}

export interface CardProps extends Omit<HTMLAttributes<HTMLDivElement>, 'title'> {
  /** Optional heading rendered inside the card. */
  title?: ReactNode
}

/**
 * @brief Surface card used for page sections.
 * @param props Card props with optional title.
 * @returns The card element.
 */
export function Card({ title, className, children, ...rest }: CardProps) {
  return (
    <div className={`${styles.card} ${className ?? ''}`} {...rest}>
      {title !== undefined && <h2 className={styles.cardTitle}>{title}</h2>}
      {children}
    </div>
  )
}

export interface AlertProps extends Omit<HTMLAttributes<HTMLDivElement>, 'title'> {
  /** Visual variant. */
  variant: Variant
  /** Optional bold title. */
  title?: ReactNode
}

/**
 * @brief Inline alert banner with a severity icon.
 * @param props Alert props.
 * @returns The alert element.
 */
export function Alert({ variant, title, className, children, ...rest }: AlertProps) {
  const { t } = useTranslation()
  return (
    <div
      role="alert"
      className={`${styles.alert} ${styles[`alert_${variant}`]} ${className ?? ''}`}
      {...rest}
    >
      <AlertIcon variant={variant} size={18} aria-hidden />
      <div className={styles.alertBody}>
        {title !== undefined && <strong>{title === '' ? t('_common.success') : title}</strong>}
        <div>{children}</div>
      </div>
    </div>
  )
}

/**
 * @brief Maps an alert variant to its severity icon.
 * @param variant The variant.
 * @param props Icon props.
 * @returns The matching Lucide icon.
 */
export function AlertIcon({ variant, ...props }: { variant: Variant } & LucideProps) {
  switch (variant) {
    case 'danger':
      return <AlertCircle {...props} />
    case 'warning':
      return <AlertTriangle {...props} />
    case 'success':
      return <CheckCircle {...props} />
    default:
      return <Info {...props} />
  }
}

export interface BadgeProps extends HTMLAttributes<HTMLSpanElement> {
  /** Visual variant. */
  variant?: Variant | 'neutral'
}

/**
 * @brief Small status pill.
 * @param props Badge props.
 * @returns The badge element.
 */
export function Badge({ variant = 'neutral', className, ...rest }: BadgeProps) {
  return (
    <span
      className={`${styles.badge} ${styles[`badge_${variant}`]} ${className ?? ''}`}
      {...rest}
    />
  )
}

/**
 * @brief Loading spinner.
 * @returns The spinner element.
 */
export function Spinner() {
  return (
    <span className={styles.spinnerWrap} role="status">
      <span className={styles.spinner} />
      <span className={styles.visuallyHidden}>Loading</span>
    </span>
  )
}

export interface PageHeaderProps {
  /** Page title. */
  title: ReactNode
  /** Page description. */
  description?: ReactNode
}

/**
 * @brief Standard page heading block.
 * @param props Title and description.
 * @returns The header element.
 */
export function PageHeader({ title, description }: PageHeaderProps) {
  return (
    <div className={styles.pageHeader}>
      <h1>{title}</h1>
      {description !== undefined && <p>{description}</p>}
    </div>
  )
}
