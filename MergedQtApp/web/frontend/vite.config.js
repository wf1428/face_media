/**
 * @file vite.config.js
 * @brief 静态网页构建配置，真实业务由独立 C++ 服务同源提供。
 * @author Du lin
 * @date 2026-10-09
 */

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// Vant already embeds WOFF2. Remove only its external font fallback so the
// offline portal never requests a CDN, in both development and production CSS.
function offlineIconFont() {
  return {
    name: 'offline-vant-icon-font',
    enforce: 'pre',
    transform(code, id) {
      if (!id.replaceAll('\\', '/').includes('/vant/') || !id.endsWith('.css')) return
      return code.replace(/,\s*url\([^)]*at\.alicdn\.com[^)]*\)\s*format\(["']woff["']\)/g, '')
    },
  }
}

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [offlineIconFont(), vue()],
  // 服务器的 CSP 禁止动态执行代码；国际化文案使用无需 new Function 的解释式编译。
  define: {
    __INTLIFY_JIT_COMPILATION__: true,
    __INTLIFY_DROP_MESSAGE_COMPILER__: false,
  },
  server: {
    host: '0.0.0.0',
    port: 8081,
    // Vite 仅用于样式开发；真实业务使用 device-webd 提供 dist，保持 Cookie/Origin 同源。
  },
  build: {
    outDir: 'dist',
  },
})
