import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

export default defineConfig({
  plugins: [
    react({
      compiler: { target: '19' },
    }),
  ],
  build: {
    outDir: 'build/web',
    emptyOutDir: true,
    target: 'es2022',
  },
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
  },
})
