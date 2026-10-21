/**
 * @file loginCredentials.js
 * @brief 记住密码仅使用浏览器密码管理器，本地存储只记录用户勾选偏好。
 * @author Du lin
 * @date 2026-10-16
 */

const preferenceKey = 'device-remember-password'

/** @brief 读取勾选偏好，浏览器禁用存储时保持未勾选。 */
export function remembersPassword() {
  try {
    return localStorage.getItem(preferenceKey) === '1'
  } catch {
    return false
  }
}

/** @brief 保存偏好；不保存账号口令或会话令牌。 */
export function setRememberPassword(remember) {
  try {
    if (remember) localStorage.setItem(preferenceKey, '1')
    else localStorage.removeItem(preferenceKey)
  } catch {
    // 存储不可用不影响本次登录。
  }
}

/** @brief 请求浏览器返回已保存的网页登录凭据，不自动提交登录。 */
export async function recalledPassword() {
  if (!remembersPassword() || !window.PasswordCredential || !navigator.credentials?.get) return null
  try {
    const credential = await navigator.credentials.get({ password: true, mediation: 'optional' })
    return credential?.type === 'password' ? credential : null
  } catch {
    return null
  }
}

/** @brief 认证成功后请求浏览器保存口令；不支持时由原生表单自动填充接管。 */
export function saveBrowserPassword(username, password) {
  if (!window.PasswordCredential || !navigator.credentials?.store) return
  try {
    const credential = new window.PasswordCredential({ id: username, password, name: username })
    // 浏览器可能请求用户确认，不能等待确认结果而阻塞已成功的设备登录。
    navigator.credentials.store(credential).catch(() => {})
  } catch {
    // 不支持或拒绝保存时，当前登录继续正常完成。
  }
}
