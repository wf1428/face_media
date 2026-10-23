/**
 * @file request.js
 * @brief Cookie 会话请求与 CSRF；上传传 File 原始内容，不缓存视频字节。
 * @author Du lin
 * @date 2026-10-09
 */

import axios from 'axios'

let csrf = '' // 当前会话 CSRF；仅内存保存。

/**
 * @brief 登录/刷新会话后更新状态修改请求 token。
 * @param {string} value 会话 CSRF 令牌，空值清除当前令牌。
 */
export function setCsrf(value) {
  csrf = value || ''
}

const client = axios.create({ baseURL: '/api/v1', timeout: 30000, withCredentials: true })

/**
 * @brief 发送同源 API 请求，解包响应并统一传播认证及业务错误。
 * @param {string} method HTTP 方法，小写 get、post、put 或 delete。
 * @param {string} url 相对于 API 根的请求地址。
 * @param {object|File|undefined} data 查询参数、业务请求体或上传原始文件，按接口约定传入。
 * @param {object} options Axios 请求选项；envelope=true 返回完整响应包，timeout 的单位为毫秒。
 * @returns {Promise<*>} 默认返回响应 data，envelope=true 返回完整包；失败抛出附带 status 的错误。
 */
export async function request(method, url, data, options = {}) {
  try {
    const { envelope = false, ...httpOptions } = options
    // 状态修改请求附带当前会话的 CSRF 令牌，上传仍发送 File 本体。
    const headers = method === 'get' ? {} : { 'X-CSRF-Token': csrf }
    const response = await client.request({
      method,
      url,
      ...(method === 'get' ? { params: data } : { data }),
      ...httpOptions,
      headers: { ...headers, ...httpOptions.headers },
    })
    if (response.data?.code !== 200) throw new Error(response.data?.message || '设备操作失败')
    return envelope ? response.data : response.data.data
  } catch (error) {
    // 会话失效统一通知根页面返回登录入口。
    if (error.response?.status === 401) {
      setCsrf('')
      window.dispatchEvent(new Event('device-session-expired'))
    }

    const failure = new Error(error.response?.data?.message || error.message || '设备连接失败')
    failure.status = error.response?.status
    throw failure
  }
}
