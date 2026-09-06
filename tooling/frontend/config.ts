import react from '@vitejs/plugin-react'

export const browserTarget = 'es2022'
export const reactCompiler = () => react({ compiler: { target: '19' } })
export const browserTests = {
  environment: 'jsdom' as const,
  execArgv: ['--no-experimental-webstorage'],
  css: true,
  coverage: { reporter: ['text', 'html'] as ('text' | 'html')[] },
}
