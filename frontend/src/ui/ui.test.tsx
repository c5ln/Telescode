// @vitest-environment jsdom

import { cleanup, fireEvent, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { useState } from 'react'
import { afterEach, describe, expect, it, vi } from 'vitest'

import type { SearchResult } from '../search/search'
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

  it('marks the selected radio item and supports choosing another with the keyboard', async () => {
    const onSelect = vi.fn()
    render(
      <Dropdown
        label="AI agent"
        items={[
          { type: 'radio', id: 'claude', label: 'Claude Code', checked: false, onSelect },
          { type: 'radio', id: 'codex', label: 'Codex', checked: true, onSelect: vi.fn() },
        ]}
        trigger={props => <button {...props} type="button">AI agent</button>}
      />,
    )
    const trigger = screen.getByRole('button', { name: 'AI agent' })
    await userEvent.click(trigger)
    expect(document.activeElement).toBe(screen.getByRole('menuitemradio', { name: 'Codex', checked: true }))
    const claude = screen.getByRole('menuitemradio', { name: 'Claude Code', checked: false })
    await userEvent.keyboard('{ArrowUp}')
    expect(document.activeElement).toBe(claude)
    await userEvent.keyboard('{Enter}')
    expect(onSelect).toHaveBeenCalledOnce()
    expect(screen.queryByRole('menu')).toBeNull()
    expect(document.activeElement).toBe(trigger)
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

  const sampleResults: SearchResult[] = [
    { id: 'class:a.py::Alpha', kind: 'class', label: 'Alpha', subtitle: 'a.py', source: 'name' },
    { id: 'file:b.py', kind: 'file', label: 'b.py', subtitle: 'src/b.py', source: 'content' },
    { id: 'member:a.py::Alpha::run', kind: 'method', label: 'run', subtitle: 'Alpha · a.py', source: 'name' },
  ]

  function ControlledSearch({
    results = sampleResults,
    onSelect = vi.fn(),
    disabled = false,
    emptyMessage = '검색 결과가 없습니다',
    footer,
  }: {
    results?: SearchResult[]
    onSelect?: (r: SearchResult) => void
    disabled?: boolean
    emptyMessage?: string
    footer?: string
  }) {
    const [value, setValue] = useState('')
    return (
      <SearchField
        label="Search"
        value={value}
        onChange={setValue}
        disabled={disabled}
        results={value.trim() ? results : []}
        onSelect={onSelect}
        emptyMessage={emptyMessage}
        footer={footer}
      />
    )
  }

  it('supports combobox keyboard navigation (ArrowDown/ArrowUp/Enter) and 3-stage Escape', async () => {
    const onSelect = vi.fn()
    render(<ControlledSearch onSelect={onSelect} />)
    const input = screen.getByRole('combobox', { name: 'Search' }) as HTMLInputElement
    expect(input.getAttribute('aria-expanded')).toBe('false')

    await userEvent.click(input)
    await userEvent.keyboard('al')

    expect(input.getAttribute('aria-expanded')).toBe('true')
    const listbox = screen.getByRole('listbox')
    expect(input.getAttribute('aria-controls')).toBe(listbox.id)
    const options = within(listbox).getAllByRole('option')
    expect(options).toHaveLength(3)
    expect(options[0].getAttribute('aria-selected')).toBe('true')
    expect(input.getAttribute('aria-activedescendant')).toBe(options[0].id)
    expect(within(options[1]).getByText('내용 일치')).toBeTruthy()

    // ArrowUp at top stays at 0; ArrowDown moves down and stops at the last item.
    await userEvent.keyboard('{ArrowUp}')
    expect(options[0].getAttribute('aria-selected')).toBe('true')
    await userEvent.keyboard('{ArrowDown}{ArrowDown}{ArrowDown}')
    expect(options[2].getAttribute('aria-selected')).toBe('true')
    expect(input.getAttribute('aria-activedescendant')).toBe(options[2].id)

    await userEvent.keyboard('{ArrowUp}{Enter}')
    expect(onSelect).toHaveBeenCalledWith(sampleResults[1])
    expect(screen.queryByRole('listbox')).toBeNull()
    expect(input.value).toBe('al')

    // Reopen by typing, then test 3-stage Escape: close -> clear -> blur.
    await userEvent.keyboard('p')
    expect(screen.getByRole('listbox')).toBeTruthy()
    await userEvent.keyboard('{Escape}')
    expect(screen.queryByRole('listbox')).toBeNull()
    expect(input.value).toBe('alp')
    expect(document.activeElement).toBe(input)

    await userEvent.keyboard('{Escape}')
    expect(input.value).toBe('')
    expect(document.activeElement).toBe(input)

    await userEvent.keyboard('{Escape}')
    expect(document.activeElement).not.toBe(input)
  })

  it('selects an option on mouse click without losing selection to blur', async () => {
    const onSelect = vi.fn()
    render(<ControlledSearch onSelect={onSelect} />)
    const input = screen.getByRole('combobox', { name: 'Search' }) as HTMLInputElement

    await userEvent.click(input)
    await userEvent.keyboard('al')
    const options = screen.getAllByRole('option')

    await userEvent.click(options[2])
    expect(onSelect).toHaveBeenCalledWith(sampleResults[2])
    expect(screen.queryByRole('listbox')).toBeNull()
    expect(input.value).toBe('al')
  })

  it('keeps the dropdown open when pressing mousedown on the listbox scrollbar or footer', async () => {
    const onSelect = vi.fn()
    render(<ControlledSearch onSelect={onSelect} footer="하단 안내" />)
    const input = screen.getByRole('combobox', { name: 'Search' })

    await userEvent.click(input)
    await userEvent.keyboard('al')

    const listbox = screen.getByRole('listbox')
    const footer = screen.getByText('하단 안내')

    // mousedown on listbox/footer must call preventDefault so input does not blur and close the popover.
    expect(fireEvent.mouseDown(listbox)).toBe(false)
    expect(fireEvent.mouseDown(footer)).toBe(false)
    expect(document.activeElement).toBe(input)
    expect(screen.getByRole('listbox')).toBeTruthy()
    expect(onSelect).not.toHaveBeenCalled()
  })

  it('ignores Enter while IME composition is active', async () => {
    const onSelect = vi.fn()
    render(<ControlledSearch onSelect={onSelect} />)
    const input = screen.getByRole('combobox', { name: 'Search' })

    await userEvent.click(input)
    await userEvent.keyboard('al')
    expect(screen.getByRole('listbox')).toBeTruthy()

    fireEvent.keyDown(input, { key: 'Enter', isComposing: true })
    fireEvent.keyDown(input, { key: 'Enter', keyCode: 229 })

    expect(onSelect).not.toHaveBeenCalled()
    expect(screen.getByRole('listbox')).toBeTruthy()
  })

  it('shows emptyMessage outside listbox when results are empty and renders footer when provided', async () => {
    render(
      <ControlledSearch
        results={[]}
        emptyMessage="검색 결과가 없습니다"
        footer="용량 상한으로 2개 파일은 이름·경로만 검색됩니다"
      />,
    )
    const input = screen.getByRole('combobox', { name: 'Search' })
    await userEvent.click(input)
    await userEvent.keyboard('zzz')

    expect(screen.queryByRole('listbox')).toBeNull()
    expect(input.getAttribute('aria-controls')).toBeNull()
    expect(screen.getByRole('status').textContent).toBe('검색 결과가 없습니다')
    expect(screen.getByText('용량 상한으로 2개 파일은 이름·경로만 검색됩니다')).toBeTruthy()
  })

  it('ignores Ctrl+K and hides shortcut hint when disabled', () => {
    render(<SearchField label="Search" disabled results={[]} />)
    const input = screen.getByRole('combobox', { name: 'Search' }) as HTMLInputElement
    expect(input.disabled).toBe(true)
    expect(screen.queryByText('Ctrl K')).toBeNull()

    const event = new KeyboardEvent('keydown', { key: 'k', ctrlKey: true, cancelable: true })
    window.dispatchEvent(event)
    expect(event.defaultPrevented).toBe(false)
    expect(document.activeElement).not.toBe(input)
    // Ensure fireEvent doesn't focus it either
    fireEvent.keyDown(window, { key: 'k', ctrlKey: true })
    expect(document.activeElement).not.toBe(input)
  })
})
