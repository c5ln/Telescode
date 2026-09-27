import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'

import 'pretendard/dist/web/variable/pretendardvariable-dynamic-subset.css'
import './styles/tokens.css'
import './styles/globals.css'

import { AppShell } from './app/AppShell'
import { readPreview } from './app/preview'

const preview = import.meta.env.DEV ? readPreview(window.location.search) : null

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <AppShell allowLocalDatabase={import.meta.env.DEV} {...preview} />
  </StrictMode>,
)
