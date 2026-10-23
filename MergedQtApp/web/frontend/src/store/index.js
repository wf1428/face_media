/**
 * @file index.js
 * @brief 仅内存登录状态；刷新后由 Cookie 恢复，不保存凭据。
 * @author Du lin
 * @date 2026-10-09
 */

import { defineStore } from 'pinia'
import { api } from '../api'

export const useAppStore = defineStore('app', {
  /**
   * @brief 创建仅在内存中保存的登录与设备展示状态。
   * @returns {object} 前端状态的初始对象。
   */
  state: () => ({ authenticated: false, username: '', deviceInfo: null }),
  actions: {
    /**
     * @brief 接收登录成功信息，仅更新内存状态。
     * @param {object} info 已认证会话资料，username 为展示账号。
     */
    setSession(info) {
      this.authenticated = true
      this.username = info.username || ''
    },

    /**
     * @brief 保存公开设备信息。
     * @param {object} info 服务端公开设备资料，不包含登录凭据。
     */
    setDeviceInfo(info) {
      this.deviceInfo = info
    },

    /**
     * @brief 刷新网页后通过现有 Cookie 校验并恢复内存身份。
     * @returns {Promise<boolean>} 会话恢复成功时为 true，失败时清除认证状态并返回 false。
     */
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
