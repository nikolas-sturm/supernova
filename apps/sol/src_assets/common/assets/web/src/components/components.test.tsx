/**
 * @file Component tests for form fields and modals.
 */

import { cleanup, fireEvent, render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { CheckboxField, mapToBoolRepresentation } from './fields'
import { ConfirmDialog } from './Modal'

afterEach(() => {
  cleanup()
})

describe('mapToBoolRepresentation', () => {
  it('recognizes the Sol string pairs', () => {
    expect(mapToBoolRepresentation('enabled')).toEqual({ truthy: 'enabled', falsy: 'disabled' })
    expect(mapToBoolRepresentation('disabled')).toEqual({ truthy: 'enabled', falsy: 'disabled' })
    expect(mapToBoolRepresentation('true')).toEqual({ truthy: 'true', falsy: 'false' })
    expect(mapToBoolRepresentation(1)).toEqual({ truthy: '1', falsy: '0' })
  })

  it('returns null for unknown values', () => {
    expect(mapToBoolRepresentation('banana')).toBeNull()
  })
})

describe('CheckboxField', () => {
  it('renders checked state for "enabled" values', () => {
    render(<CheckboxField id="controller" label="Gamepad" value="enabled" onChange={() => {}} />)
    const checkbox = screen.getByRole('checkbox')
    expect(checkbox).toBeChecked()
  })

  it('writes back the same string representation on toggle', async () => {
    const onChange = vi.fn()
    render(<CheckboxField id="upnp" label="UPnP" value="enabled" onChange={onChange} />)
    await userEvent.click(screen.getByRole('checkbox'))
    expect(onChange).toHaveBeenCalledWith('disabled')
  })

  it('shows the default hint when provided', () => {
    render(
      <CheckboxField
        id="autoDetach"
        label="Auto detach"
        value="enabled"
        onChange={() => {}}
        defaultValue
        checkedHint="Default: checked"
      />,
    )
    expect(screen.getByText('Default: checked')).toBeInTheDocument()
  })
})

describe('ConfirmDialog', () => {
  it('does not render when closed', () => {
    render(
      <ConfirmDialog
        open={false}
        title="Delete"
        message="Sure?"
        confirmLabel="Delete"
        onConfirm={() => {}}
        onCancel={() => {}}
      />,
    )
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument()
  })

  it('calls onConfirm and onCancel', async () => {
    const onConfirm = vi.fn()
    const onCancel = vi.fn()
    render(
      <ConfirmDialog
        open
        title="Delete Application"
        message="Sure?"
        confirmLabel="Delete"
        onConfirm={onConfirm}
        onCancel={onCancel}
      />,
    )
    expect(screen.getByRole('dialog')).toBeInTheDocument()
    fireEvent.click(screen.getByRole('button', { name: 'Delete' }))
    expect(onConfirm).toHaveBeenCalledOnce()
    fireEvent.click(screen.getByRole('button', { name: /cancel/i }))
    expect(onCancel).toHaveBeenCalledOnce()
  })
})
