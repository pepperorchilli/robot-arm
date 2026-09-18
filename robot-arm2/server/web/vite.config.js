import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],

  // 部署在 /control/ 路径下（Express 的 /control 路由）
  base: '/control/',

  build: {
    // 直接构建到 Express 的静态目录，省去拷贝步骤
    outDir: '../public/control',
    emptyOutDir: true,
  },

  server: {
    // 开发时把 API 请求代理到后端，避免跨域
    proxy: {
      '/set': 'http://localhost:3000',
      '/api': 'http://localhost:3000',
    },
  },
})
