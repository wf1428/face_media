/**
 * @file portal.js
 * @brief 从服务器取得固定地址，通过用户点击请求完整浏览器并提供复制后备。
 * @author Du lin
 * @date 2026-10-16
 */

const openButton = document.getElementById('open-browser')
const copyButton = document.getElementById('copy-address')
const address = document.getElementById('server-address')
const status = document.getElementById('status')

/**
 * @brief 生成安卓浏览器请求；不指定厂商包名，失败时回到引导页。
 * @param {string} origin 设备 Web 同源入口。
 * @param {string} loginUrl 普通浏览器登录入口。
 * @param {string} guideUrl 引导页面地址。
 * @returns {string} Android 的浏览器 Intent URL；其他平台直接返回登录地址。
 */
function browserLink(origin, loginUrl, guideUrl) {
  if (!/Android/i.test(navigator.userAgent)) return loginUrl
  const target = new URL(origin)
  return (
    `intent://${target.host}/#Intent;scheme=${target.protocol.slice(0, -1)};` +
    'action=android.intent.action.VIEW;category=android.intent.category.BROWSABLE;' +
    `S.browser_fallback_url=${encodeURIComponent(guideUrl)};end`
  )
}

/**
 * @brief 请求复制地址；HTTP 门户没有 Clipboard API 时使用选中复制。
 */
async function copyAddress() {
  try {
    if (window.isSecureContext && navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(address.value)
    } else {
      address.focus()
      address.select()
      address.setSelectionRange(0, address.value.length)
      if (!document.execCommand('copy')) throw new Error('copy unavailable')
    }
    status.textContent = '地址已复制，请打开手机浏览器并粘贴访问。'
  } catch {
    address.focus()
    address.select()
    address.setSelectionRange(0, address.value.length)
    status.textContent = '请长按上方地址复制，再到手机浏览器中打开。'
  }
}

/**
 * @brief 读取固定部署地址；不自动发起应用跳转，保留真实用户点击。
 */
async function initialize() {
  try {
    const response = await fetch('/portal-config.json', { cache: 'no-store' })
    if (!response.ok) throw new Error('config unavailable')
    const config = await response.json()
    const origin = new URL(config.origin)
    if (!['http:', 'https:'].includes(origin.protocol) || origin.origin !== config.origin)
      throw new Error('invalid origin')
    if (
      config.loginUrl !== `${config.origin}/#/login` ||
      config.guideUrl !== `${config.origin}/portal`
    )
      throw new Error('invalid destination')
    address.value = config.loginUrl
    openButton.href = browserLink(config.origin, config.loginUrl, config.guideUrl)
    openButton.hidden = false
    copyButton.disabled = false
    status.textContent = '点击按钮打开浏览器；若未能跳出此窗口，请复制地址访问。'
  } catch {
    status.textContent = '设备地址读取失败，请重新连接热点后重试。'
  }
}

/** @brief 只显示跳转请求提示，不把发出链接误报为浏览器已经打开。 */
openButton.addEventListener('click', () => {
  status.textContent = '已请求打开浏览器。若手机拦截，请复制地址到浏览器中访问。'
})
copyButton.addEventListener('click', copyAddress)
initialize()
