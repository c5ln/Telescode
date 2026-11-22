// The few glyphs the shell needs, drawn on a 16px grid with a 1.5px stroke.
// Inline SVG keeps them crisp, themeable via currentColor, and dependency-free.

import type { ReactNode, SVGProps } from 'react'

type IconProps = Omit<SVGProps<SVGSVGElement>, 'children'>

function Icon({ children, ...props }: IconProps & { children: ReactNode }) {
  return (
    <svg
      width="16"
      height="16"
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.5"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
      {...props}
    >
      {children}
    </svg>
  )
}

export const SearchIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="7" cy="7" r="4.25" />
    <path d="m10.25 10.25 3 3" />
  </Icon>
)

export const MoreIcon = (p: IconProps) => (
  <Icon {...p} stroke="none" fill="currentColor">
    <circle cx="3.5" cy="8" r="1.25" />
    <circle cx="8" cy="8" r="1.25" />
    <circle cx="12.5" cy="8" r="1.25" />
  </Icon>
)

export const PlusIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M8 3.5v9M3.5 8h9" />
  </Icon>
)

export const MinusIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M3.5 8h9" />
  </Icon>
)

export const FitIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M2.75 6V2.75H6M10 2.75h3.25V6M13.25 10v3.25H10M6 13.25H2.75V10" />
  </Icon>
)

export const ChevronRightIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m6.25 4 4 4-4 4" />
  </Icon>
)

export const ChevronLeftIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m9.75 4-4 4 4 4" />
  </Icon>
)

export const ChevronDownIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m4 6 4 4 4-4" />
  </Icon>
)

export const CheckIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m3.5 8 3 3 6-6" />
  </Icon>
)

export const PlayIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M5 3.25v9.5L12.5 8 5 3.25Z" />
  </Icon>
)

export const PauseIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M5.5 3.5v9M10.5 3.5v9" />
  </Icon>
)

export const CloseIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m4 4 8 8M12 4l-8 8" />
  </Icon>
)

export const QuizIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="8" cy="8" r="5.75" />
    <path d="M6.25 6.5a1.75 1.75 0 1 1 2.5 1.6c-.5.23-.75.6-.75 1.15v.25" />
    <path d="M8 11.5h.01" />
  </Icon>
)

export const FileIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M4 2.5h5.25L12.5 5.75V13.5H4z" />
    <path d="M9.25 2.5v3.25h3.25" />
  </Icon>
)

export const ClassIcon = (p: IconProps) => (
  <Icon {...p}>
    <rect x="2.75" y="2.75" width="10.5" height="10.5" rx="1.5" />
    <path d="M2.75 6.5h10.5" />
  </Icon>
)

export const MethodIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M5.25 4.25 2.5 8l2.75 3.75M10.75 4.25 13.5 8l-2.75 3.75M8.75 3.75l-1.5 8.5" />
  </Icon>
)

export const FieldIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="4.75" cy="8" r="1.75" />
    <path d="M8.25 8h5" />
  </Icon>
)
