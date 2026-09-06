/** @file Admin integration tests for the shared native theme selector. */
import { cleanup, render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { themeOptions, useThemeStore } from '../store/themeStore'
import { ThemeMenu } from './ThemeMenu'

afterEach(cleanup)
beforeEach(() => {
  localStorage.clear()
  useThemeStore.getState().setTheme('dark')
})

describe('ThemeMenu', () => {
  it('offers every original palette and applies light and dark selections', async () => {
    render(<ThemeMenu />)
    const select = screen.getByRole('combobox')
    expect(screen.getAllByRole('option')).toHaveLength(
      themeOptions.dark.length + themeOptions.light.length + 1,
    )
    await userEvent.selectOptions(select, 'latte')
    expect(select).toHaveValue('latte')
    expect(document.documentElement.dataset.theme).toBe('latte')
    expect(localStorage.getItem('theme')).toBe('latte')
    await userEvent.selectOptions(select, 'dark')
    expect(document.documentElement.dataset.theme).toBe('dark')
  })

  it('randomizes without submitting an enclosing admin form', async () => {
    let submitted = false
    render(
      <form
        onSubmit={(event) => {
          event.preventDefault()
          submitted = true
        }}
      >
        <ThemeMenu />
      </form>,
    )
    await userEvent.click(screen.getByRole('button'))
    expect(useThemeStore.getState().preference).not.toBe('dark')
    expect(screen.getByRole('combobox')).toHaveValue(useThemeStore.getState().preference)
    expect(submitted).toBe(false)
  })
})
