import { defineConfig } from 'vite'
import { browserTarget, nativeWatchIgnored, reactCompiler } from '../../tooling/frontend/config.ts'

export default defineConfig({
  plugins: [reactCompiler()],
  build: {
    outDir: 'build/web',
    emptyOutDir: true,
    target: browserTarget,
  },
  server: {
    watch: { ignored: nativeWatchIgnored },
    host: '127.0.0.1',
    port: 5174,
    strictPort: true,
  },
})
