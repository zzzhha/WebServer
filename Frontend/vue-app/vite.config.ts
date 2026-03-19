import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import path from 'path'
import Inspector from 'unplugin-vue-dev-locator/vite'

// https://vite.dev/config/
export default defineConfig({
  build: {
    sourcemap: 'hidden',
    outDir: path.resolve(__dirname, '../html'),
    emptyOutDir: false,
    rollupOptions: {
      input: {
        index: path.resolve(__dirname, 'index.html'),
        welcome: path.resolve(__dirname, 'welcome.html'),
        login: path.resolve(__dirname, 'login.html'),
        register: path.resolve(__dirname, 'register.html'),
        picture: path.resolve(__dirname, 'picture.html'),
        video: path.resolve(__dirname, 'video.html'),
      },
    },
  },
  plugins: [
    vue(),
    Inspector(),
  ],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, './src'), // ✅ 定义 @ = src
    },
  },
  server: {
    proxy: {
      '^/(login|register)$': {
        target: 'http://127.0.0.1:18080',
        changeOrigin: true,
      },
      '^/(images|video|download)/': {
        target: 'http://127.0.0.1:18080',
        changeOrigin: true,
      },
      '^/api/': {
        target: 'http://127.0.0.1:18080',
        changeOrigin: true,
      },
    },
  },
})
