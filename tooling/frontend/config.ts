import { fileURLToPath } from 'node:url'
import react from '@vitejs/plugin-react'

export const browserTarget = 'es2022'
export const reactCompiler = () => react({ compiler: { target: '19' } })
export const designSystemSource = fileURLToPath(
  new URL('../../packages/design-system/src', import.meta.url),
)
// Windows directory watchers can prevent CMake from renaming extracted dependencies.
export const nativeWatchIgnored =
  /(?:^|[/\\])apps[/\\][^/\\]+[/\\](?:cmake-build-[^/\\]+|third-party|refs|extensions)(?:[/\\]|$)/
export const browserTests = {
  environment: 'jsdom' as const,
  execArgv: ['--no-experimental-webstorage'],
  css: true,
  coverage: { reporter: ['text', 'html'] as ('text' | 'html')[] },
}
