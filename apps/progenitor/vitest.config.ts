import react from '@vitejs/plugin-react'
import { defineConfig } from 'vitest/config'
import { browserTests } from '../../tooling/frontend/config.ts'

export default defineConfig({
  root: 'src_assets/common/assets/web',
  plugins: [react()],
  test: {
    ...browserTests,
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.{test,spec}.{ts,tsx}'],
  },
})
