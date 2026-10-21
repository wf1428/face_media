/**
 * @file index.js
 * @brief 仅内存登录状态；刷新后由 Cookie 恢复，不保存凭据。
 * @author Du lin
 * @date 2026-10-09
 */

import { defineStore } from 'pinia'
import { api } from '../api'

export const useAppStore = defineStore('app', {
  /** @brief 创建仅在内存中保存的登录与设备展示状态。 */
  state: () => ({ authenticated: false, username: '', deviceInfo: null }),
  actions: {
    /** @brief 接收登录成功信息。 */
    setSession(info) {
      this.authenticated = true
      this.username = info.username || ''
    },

    /** @brief 保存公开设备信息。 */
    setDeviceInfo(info) {
      this.deviceInfo = info
    },

    /** @return 刷新后恢复会话结果。 */
    async restore() {
      try {
        this.setSession(await api.session())
        return true
      } catch {
        this.authenticated = false
        return false
      }
    },

    /** @brief 清理内存状态。 */
    clear() {
      this.authenticated = false
      this.username = ''
      this.deviceInfo = null
    },

    /** @brief 登出真实服务器会话。 */
    async logout() {
      await api.logout()
      this.clear()
    },
  },
})
