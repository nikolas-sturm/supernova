import { defineConfig } from 'vite'
import { browserTarget, reactCompiler } from '../../tooling/frontend/config.ts'

export default defineConfig({
  plugins: [reactCompiler()],
  build: {
    outDir: 'build/web',
    emptyOutDir: true,
    target: browserTarget,
  },
  server: {
    host: '127.0.0.1',
    port: 5174,
    strictPort: true,
  },
})
