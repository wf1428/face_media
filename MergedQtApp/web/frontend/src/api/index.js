/**
 * @file index.js
 * @brief 标准本地业务 API，保留原会话、视频流上传和 Excel/图片导入接口。
 * @author Du lin
 * @date 2026-10-09
 */

import { request, setCsrf } from './request'
import { localApi } from './local'

/** @return 字节数展示文本。 */
export function formatSize(bytes) {
  return Number.isFinite(Number(bytes))
    ? bytes >= 1048576
      ? (bytes / 1048576).toFixed(1) + ' MiB'
      : Math.max(1, Math.ceil(bytes / 1024)) + ' KiB'
    : '—'
}

/** @return 刷新页时由 Cookie 恢复的真实会话。 */
async function session() {
  const data = await request('get', '/auth/session')
  setCsrf(data.csrf)
  return data
}

/** @return 上传并发布 File 的结果；失败查询同一任务，禁止盲目重建。 */
async function uploadFile(file, kind, batchId, onProgress = () => {}) {
  // 先取得服务端任务编号；文件名只作为业务元数据，文件内容单独传输。
  const task = await request('post', '/uploads', {
    name: file.name,
    size: file.size,
    kind,
    ...(batchId ? { batch_id: batchId } : {}),
  })
  if (kind === 'resource_package') sessionStorage.setItem('ycest-resource-task', task.id)
  try {
    // 直接上传浏览器 File 原始字节，不在前端改名或压缩。
    await request('put', `/uploads/${task.id}/content`, file, {
      timeout: 0,
      headers: { 'Content-Type': 'application/octet-stream' },

      /** @brief 将上传字节数转换为当前任务的进度事件。 */
      onUploadProgress: (event) =>
        onProgress({
          id: task.id,
          percent: Math.min(99, Math.floor((event.loaded / file.size) * 100)),
          state: 'RECEIVING',
        }),
    })
    onProgress({ id: task.id, percent: 99, state: 'VALIDATING' })

    // 传输完成后由 Qt 校验并发布，READY 才表示文件真正可用。
    let result = await request('post', `/uploads/${task.id}/commit`, {})
    if (kind === 'resource_package') result = await waitResourcePackage(task.id, onProgress)
    onProgress({ id: task.id, percent: 100, state: result.state })
    return result
  } catch (error) {
    // 请求结果不确定时先查询原任务，避免重复创建或覆盖同名文件。
    try {
      const current = await request('get', `/uploads/${task.id}`)
      if (current.state === 'READY') return current
      if (['RECEIVED', 'VALIDATING', 'COMMITTING'].includes(current.state)) {
        const retried = await request('post', `/uploads/${task.id}/commit`, {})
        return kind === 'resource_package'
          ? await waitResourcePackage(task.id, onProgress)
          : retried
      }
    } catch {
      /* 原始错误及 taskId 留给页面，不把不确定结果报告成功。 */
    }
    error.taskId = task.id
    throw error
  }
}

/** @brief 只查询同一资源更新任务，READY 才表示目录替换完成；刷新可继续查询。 */
async function waitResourcePackage(id, onProgress = () => {}) {
  for (;;) {
    const task = await request('get', `/uploads/${id}`)
    onProgress({ id, percent: task.state === 'READY' ? 100 : 99, state: task.state })
    if (task.state === 'READY') return task
    if (['FAILED', 'CANCELLED'].includes(task.state))
      throw new Error(task.error || '资源包更新失败')
    if (task.state === 'RECEIVED') {
      await request('post', `/uploads/${id}/commit`, {})
      continue
    }
    if (!['VALIDATING', 'COMMITTING', 'RECEIVED'].includes(task.state)) return task
    await new Promise((resolve) => setTimeout(resolve, 1000))
  }
}

export const api = {
  ...localApi,

  /** @brief 读取登录页展示的设备公开信息。 */
  getDeviceInfo: () => request('get', '/device/info'),

  /** @brief 保存热点名称和可选新密码，空密码不修改原连接密钥。 */
  saveWifiSsid: (ssid, password = '') =>
    request('post', '/device/wifi', { ssid, ...(password ? { password } : {}) }),

  /** @brief 登录后保存本次会话的 CSRF 令牌。 */
  verifyPassword: async (credentials) => {
    const data = await request('post', '/auth/login', credentials)
    setCsrf(data.csrf)
    return data
  },
  session,

  /** @brief 注销服务端会话并清除内存令牌。 */
  logout: async () => {
    await request('post', '/auth/logout', {})
    setCsrf('')
  },

  /** @brief 将设备视频清单转换为页面展示数据。 */
  playFileList: async () =>
    (await request('get', '/files')).files.map((file) => ({
      ...file,
      size: formatSize(file.bytes),
      duration: '—',
    })),

  /** @brief 按文件标识请求删除设备视频。 */
  deletePlayFile: ({ id }) => request('delete', `/files/${id}`, {}),

  /** @brief 生成同源的视频下载地址。 */
  downloadUrl: (id) => `/api/v1/files/${encodeURIComponent(id)}/content`,

  /** @brief 创建视频上传任务并提交原始文件字节。 */
  uploadVideo: (file, progress) => uploadFile(file, 'video', '', progress),
  uploadFile,

  /** @brief 上传 Ycest 归档并等待独立解压进程完成目录替换。 */
  uploadResourcePackage: (file, progress) => uploadFile(file, 'resource_package', '', progress),
  waitResourcePackage,

  /** @brief 查询指定上传任务的当前状态。 */
  uploadStatus: (id) => request('get', `/uploads/${id}`),

  /** @brief 取消未完成的上传任务。 */
  cancelUpload: (id) => request('post', `/uploads/${id}/cancel`, {}),

  /** @brief 根据所选 Excel 和图片清单创建导入批次。 */
  createImport: (manifest) => request('post', '/imports', { manifest }),

  /** @brief 查询导入批次状态及结果。 */
  importStatus: (id) => request('get', `/imports/${id}`),

  /** @brief 封存并启动人员导入批次。 */
  startImport: (id) => request('post', `/imports/${id}/start`, {}),

  /** @brief 丢弃尚未开始的导入批次。 */
  discardImport: (id) => request('post', `/imports/${id}/discard`, {}),

  /** @brief 读取设备上的本地人员列表。 */
  personList: localApi.personList,

  /** @brief 按标识读取本地人员详情。 */
  personDetail: localApi.personDetail,
}
