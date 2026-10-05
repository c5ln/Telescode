import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'

import 'pretendard/dist/web/variable/pretendardvariable-dynamic-subset.css'
import './styles/tokens.css'
import './styles/globals.css'

import { AppShell } from './app/AppShell'
import { readPreview } from './app/preview'
import { RenderApp } from './tour/RenderApp'

const isTour = window.location.pathname === '/tour-render'
const preview = !isTour && import.meta.env.DEV ? await readPreview(window.location.search) : null

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    {isTour ? <RenderApp /> : <AppShell allowLocalFolder={import.meta.env.DEV} {...preview} />}
  </StrictMode>,
)
