// @vitest-environment jsdom

import { cleanup, render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { Breadcrumbs, type Crumb } from './Breadcrumbs'
import { Dropdown } from './Dropdown'
import { SearchField } from './SearchField'

afterEach(cleanup)

const crumbs = (...labels: string[]): Crumb[] => labels.map((label) => ({ id: label, label }))

describe('Breadcrumbs', () => {
  it('shows every level, with ancestors as buttons and the last as the current location', async () => {
    const onNavigate = vi.fn()
    render(<Breadcrumbs items={crumbs('src', 'core', 'dependency.cpp')} onNavigate={onNavigate} />)

    const nav = screen.getByRole('navigation', { name: 'Breadcrumb' })
    expect(nav.textContent).toBe('srccoredependency.cpp')
    expect(screen.getByText('dependency.cpp').getAttribute('aria-current')).toBe('location')
    expect(screen.queryByRole('button', { name: 'dependency.cpp' })).toBeNull()

    await userEvent.click(screen.getByRole('button', { name: 'core' }))
    expect(onNavigate).toHaveBeenCalledWith({ id: 'core', label: 'core' }, 1)
  })

  it('collapses the middle of a deep path into a menu', async () => {
    const onNavigate = vi.fn()
    render(<Breadcrumbs items={crumbs('repo', 'a', 'b', 'c', 'd', 'e', 'leaf')} maxVisible={4} onNavigate={onNavigate} />)

    // repo › … › e › leaf
    expect(screen.getAllByRole('listitem')).toHaveLength(4)
    expect(screen.queryByText('b')).toBeNull()

    await userEvent.click(screen.getByRole('button', { name: 'Show 4 hidden levels' }))
    const items = screen.getAllByRole('menuitem')
    expect(items.map((i) => i.textContent)).toEqual(['a', 'b', 'c', 'd'])

    await userEvent.click(items[1])
    expect(onNavigate).toHaveBeenCalledWith({ id: 'b', label: 'b' }, 2)
    expect(screen.queryByRole('menu')).toBeNull()
  })

  it('keeps short paths intact', () => {
    render(<Breadcrumbs items={crumbs('a', 'b', 'c')} maxVisible={3} />)
    expect(screen.getAllByRole('listitem')).toHaveLength(3)
  })
})

describe('Dropdown', () => {
  const setup = () => {
    const onA = vi.fn()
    const onC = vi.fn()
    render(
      <>
        <Dropdown
          items={[
            { id: 'a', label: 'Alpha', onSelect: onA },
            { id: 'b', label: 'Beta', onSelect: vi.fn(), disabled: true },
            { type: 'separator', id: 's' },
            { id: 'c', label: 'Gamma', onSelect: onC },
          ]}
          trigger={(props) => (
            <button {...props} type="button">
              More
            </button>
          )}
        />
        <p>outside</p>
      </>,
    )
    return { onA, onC, trigger: screen.getByRole('button', { name: 'More' }) }
  }

  it('opens on click, focuses the first item, and skips disabled items with the arrows', async () => {
    const { onC, trigger } = setup()
    await userEvent.click(trigger)

    expect(trigger.getAttribute('aria-expanded')).toBe('true')
    expect(document.activeElement).toBe(screen.getByRole('menuitem', { name: 'Alpha' }))

    await userEvent.keyboard('{ArrowDown}')
    expect(document.activeElement).toBe(screen.getByRole('menuitem', { name: 'Gamma' }))
    await userEvent.keyboard('{ArrowDown}')
    expect(document.activeElement).toBe(screen.getByRole('menuitem', { name: 'Alpha' }))
    await userEvent.keyboard('{End}{Enter}')

    expect(onC).toHaveBeenCalledOnce()
    expect(screen.queryByRole('menu')).toBeNull()
    expect(document.activeElement).toBe(trigger)
  })

  it('closes on Escape and returns focus to the trigger', async () => {
    const { onA, trigger } = setup()
    trigger.focus()
    await userEvent.keyboard('{ArrowDown}')
    expect(screen.getByRole('menu')).toBeTruthy()

    await userEvent.keyboard('{Escape}')
    expect(screen.queryByRole('menu')).toBeNull()
    expect(document.activeElement).toBe(trigger)
    expect(onA).not.toHaveBeenCalled()
  })

  it('closes on an outside click', async () => {
    const { trigger } = setup()
    await userEvent.click(trigger)
    await userEvent.click(screen.getByText('outside'))
    expect(screen.queryByRole('menu')).toBeNull()
  })

  it('flips a toggle item in place and keeps the menu open', async () => {
    const onChange = vi.fn()
    const { rerender } = render(
      <Dropdown
        items={[{ type: 'toggle', id: 't', label: 'Complexity', checked: false, onChange }]}
        trigger={(props) => (
          <button {...props} type="button">
            View
          </button>
        )}
      />,
    )
    await userEvent.click(screen.getByRole('button', { name: 'View' }))
    const toggle = screen.getByRole('menuitemcheckbox', { name: 'Complexity' })
    expect(toggle.getAttribute('aria-checked')).toBe('false')
    expect(document.activeElement).toBe(toggle)

    await userEvent.keyboard('{Enter}')
    expect(onChange).toHaveBeenCalledWith(true)
    expect(screen.getByRole('menu')).toBeTruthy()

    rerender(
      <Dropdown
        items={[{ type: 'toggle', id: 't', label: 'Complexity', checked: true, onChange }]}
        trigger={(props) => (
          <button {...props} type="button">
            View
          </button>
        )}
      />,
    )
    expect(screen.getByRole('menuitemcheckbox', { name: 'Complexity' }).getAttribute('aria-checked')).toBe('true')
  })
})

describe('SearchField', () => {
  it('focuses on Ctrl K and hides the shortcut hint while there is a query', async () => {
    render(<SearchField placeholder="Search files, classes, functions…" />)
    const input = screen.getByRole('textbox', { name: 'Search files, classes, functions…' })
    expect(screen.getByText('Ctrl K')).toBeTruthy()

    await userEvent.keyboard('{Control>}k{/Control}')
    expect(document.activeElement).toBe(input)

    await userEvent.keyboard('dep')
    expect(screen.queryByText('Ctrl K')).toBeNull()
  })

  it('clears on the first Escape and leaves the field on the second', async () => {
    const onChange = vi.fn()
    render(<SearchField onChange={onChange} />)
    const input = screen.getByRole('textbox') as HTMLInputElement

    await userEvent.click(input)
    await userEvent.keyboard('core')
    await userEvent.keyboard('{Escape}')
    expect(input.value).toBe('')
    expect(onChange).toHaveBeenLastCalledWith('')
    expect(document.activeElement).toBe(input)

    await userEvent.keyboard('{Escape}')
    expect(document.activeElement).not.toBe(input)
  })
})
