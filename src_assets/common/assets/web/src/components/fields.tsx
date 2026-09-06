/**
 * @file Form field components for Sunshine's stringly-typed config values.
 */

import type { ChangeEvent, ReactNode, SelectHTMLAttributes } from 'react'
import styles from './fields.module.css'

export interface TextFieldProps {
  /** Input id. */
  id?: string
  /** Label content. */
  label: ReactNode
  /** Helper text. */
  description?: ReactNode
  /** Error text. */
  error?: string
  /** Current value. */
  value: string
  /** Change handler. */
  onChange: (value: string) => void
  /** Placeholder text. */
  placeholder?: string
  /** autocomplete attribute. */
  autoComplete?: string
  /** Required marker. */
  required?: boolean
  /** HTML input type. */
  type?: 'text' | 'password'
  /** Monospace styling. */
  mono?: boolean
  /** Disable the input. */
  disabled?: boolean
  /** Keydown handler. */
  onKeyDown?: (event: React.KeyboardEvent<HTMLInputElement>) => void
}

/**
 * @brief Controlled text input with label and description.
 * @param props Field props.
 * @returns The field element.
 */
export function TextField({
  id,
  label,
  description,
  error,
  value,
  onChange,
  placeholder,
  autoComplete,
  required,
  type = 'text',
  mono = false,
  disabled,
  onKeyDown,
}: TextFieldProps) {
  return (
    <div className={styles.field}>
      <label className={styles.label} htmlFor={id}>
        {label}
      </label>
      <input
        id={id}
        className={`${styles.input} ${mono ? styles.mono : ''}`}
        type={type}
        value={value}
        placeholder={placeholder}
        autoComplete={autoComplete}
        required={required}
        disabled={disabled}
        onKeyDown={onKeyDown}
        onChange={(event: ChangeEvent<HTMLInputElement>) => onChange(event.target.value)}
      />
      {description !== undefined && <div className={styles.description}>{description}</div>}
      {error !== undefined && <div className={styles.error}>{error}</div>}
    </div>
  )
}

export interface SelectOption {
  /** Option value. */
  value: string
  /** Option label. */
  label: ReactNode
}

export interface SelectFieldProps
  extends Omit<SelectHTMLAttributes<HTMLSelectElement>, 'onChange' | 'value'> {
  /** Input id (doubles as the config search anchor). */
  id?: string
  /** Label content. */
  label: ReactNode
  /** Helper text. */
  description?: ReactNode
  /** Current value. */
  value: string
  /** Change handler receiving the new value. */
  onChange: (value: string) => void
  /** Options to render. */
  options: SelectOption[]
}

/**
 * @brief Controlled select input with label and description.
 * @param props Field props.
 * @returns The field element.
 */
export function SelectField({
  id,
  label,
  description,
  value,
  onChange,
  options,
  ...rest
}: SelectFieldProps) {
  return (
    <div className={styles.field} id={id ? `${id}-field` : undefined}>
      <label className={styles.label} htmlFor={id}>
        {label}
      </label>
      <select
        {...rest}
        id={id}
        className={styles.input}
        value={value}
        onChange={(event) => onChange(event.target.value)}
      >
        {options.map((option) => (
          <option key={option.value} value={option.value}>
            {option.label}
          </option>
        ))}
      </select>
      {description !== undefined && <div className={styles.description}>{description}</div>}
    </div>
  )
}

/**
 * @brief Maps arbitrary config values to a boolean representation.
 *
 * Accepts booleans, 0/1, and the string pairs used by Sunshine config values
 * (`true/false`, `enabled/disabled`, `yes/no`, `on/off`).
 * @param value The raw value.
 * @returns The string pair when recognized, otherwise null.
 */
export function mapToBoolRepresentation(value: unknown): { truthy: string; falsy: string } | null {
  const stringPairs: [string, string][] = [
    ['true', 'false'],
    ['1', '0'],
    ['enabled', 'disabled'],
    ['enable', 'disable'],
    ['yes', 'no'],
    ['on', 'off'],
  ]
  if (typeof value === 'boolean') {
    return { truthy: 'true', falsy: 'false' }
  }
  const normalized = `${value}`.toLowerCase().trim()
  for (const pair of stringPairs) {
    if (normalized === pair[0] || normalized === pair[1]) {
      return { truthy: pair[0], falsy: pair[1] }
    }
  }
  return null
}

export interface CheckboxFieldProps {
  /** Input id (also the config key, used for search anchoring). */
  id: string
  /** Label content. */
  label: ReactNode
  /** Helper text. */
  description?: ReactNode
  /** Raw config value (may be a string like "enabled"). */
  value: unknown
  /**
   * @brief Change handler receiving the value written back to the config.
   * @param value The new raw value in the same string representation.
   */
  onChange: (value: string) => void
  /** Whether the default state is checked (shows the default hint). */
  defaultValue?: boolean
  /** Localized default-checked hint text. */
  checkedHint?: string
  /** Localized default-unchecked hint text. */
  uncheckedHint?: string
}

/**
 * @brief Checkbox bound to Sunshine's stringly-typed boolean config values.
 * @param props Checkbox props.
 * @returns The checkbox element.
 */
export function CheckboxField({
  id,
  label,
  description,
  value,
  onChange,
  defaultValue,
  checkedHint,
  uncheckedHint,
}: CheckboxFieldProps) {
  const mapped = mapToBoolRepresentation(value)
  const checked = mapped ? `${value}`.toLowerCase().trim() === mapped.truthy : Boolean(value)
  const representation = mapped ?? { truthy: 'true', falsy: 'false' }
  return (
    <div className={styles.checkbox}>
      <input
        id={id}
        type="checkbox"
        checked={checked}
        onChange={(event) =>
          onChange(event.target.checked ? representation.truthy : representation.falsy)
        }
      />
      <div>
        <label htmlFor={id} className={styles.label}>
          {label}
          {defaultValue !== undefined && (
            <div className={styles.description}>
              {defaultValue
                ? (checkedHint ?? 'Default: checked')
                : (uncheckedHint ?? 'Default: unchecked')}
            </div>
          )}
        </label>
        {description !== undefined && <div className={styles.description}>{description}</div>}
      </div>
    </div>
  )
}

export interface NumberFieldProps {
  /** Input id (doubles as the config search anchor). */
  id?: string
  /** Label content. */
  label: ReactNode
  /** Helper text. */
  description?: ReactNode
  /** Current value (raw config value; may be number or string). */
  value: string | number
  /** Change handler receiving the raw text. */
  onChange: (value: string) => void
  /** Placeholder text. */
  placeholder?: string
  /** Minimum value. */
  min?: number
  /** Maximum value. */
  max?: number
  /** Monospace styling. */
  mono?: boolean
}

/**
 * @brief Controlled numeric input that keeps the raw string for config fidelity.
 * @param props Field props.
 * @returns The field element.
 */
export function NumberField({
  id,
  label,
  description,
  value,
  onChange,
  placeholder,
  min,
  max,
  mono,
}: NumberFieldProps) {
  return (
    <div className={styles.field} id={id ? `${id}-field` : undefined}>
      <label className={styles.label} htmlFor={id}>
        {label}
      </label>
      <input
        id={id}
        className={`${styles.input} ${mono ? styles.mono : ''}`}
        type="number"
        value={value}
        placeholder={placeholder}
        min={min}
        max={max}
        onChange={(event) => onChange(event.target.value)}
      />
      {description !== undefined && <div className={styles.description}>{description}</div>}
    </div>
  )
}
