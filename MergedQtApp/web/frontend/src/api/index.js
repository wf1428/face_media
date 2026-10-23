/**
 * @file index.js
 * @brief 标准本地业务 API，保留原会话、视频流上传和 Excel/图片导入接口。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-22；修改人：Du lin；媒体队列可在创建任务后登记原任务号，保留现有上传、校验和失败核对流程。
 */

import { request, setCsrf } from './request'
import { localApi } from './local'

/**
 * @brief 将文件字节数转换为易读的容量文本。
 * @return 字节数展示文本。
 * @param {number} bytes 文件长度，单位为字节。
 */
export function formatSize(bytes) {
  return Number.isFinite(Number(bytes))
    ? bytes >= 1048576
      ? (bytes / 1048576).toFixed(1) + ' MiB'
      : Math.max(1, Math.ceil(bytes / 1024)) + ' KiB'
    : '—'
}

/**
 * @brief 使用 Cookie 恢复真实会话并更新内存中的 CSRF 令牌。
 * @return 刷新页时由 Cookie 恢复的真实会话。
 */
async function session() {
  const data = await request('get', '/auth/session')
  setCsrf(data.csrf)
  return data
}

/**
 * @brief 上传原始文件并确认实际发布状态；失败时保留同一任务以供查询。
 * @return 上传并发布 File 的结果；失败查询同一任务，禁止盲目重建。
 * @param {File} file 浏览器选中的原始文件；上传时保留名称和字节。
 * @param {string} kind 服务端资源类型，决定目录、限额和内容校验规则。
 * @param {string} batchId 人员导入批次号，普通媒体上传可为空。
 * @param {Function} onProgress 进度回调，参数包含任务 id、百分比和实际状态。
 * @param {object} options 上传选项；smallImageMode 选择图片目录，onTaskCreated 可接收已创建任务以记录断线恢复标识。
 */
async function uploadFile(file, kind, batchId, onProgress = () => {}, options = {}) {
  // 先取得服务端任务编号；文件名只作为业务元数据，文件内容单独传输。
  const task = await request('post', '/uploads', {
    name: file.name,
    size: file.size,
    kind,
    ...(kind === 'ad_image' ? { smallImageMode: options.smallImageMode ?? 0 } : {}),
    ...(batchId ? { batch_id: batchId } : {}),
  })
  if (kind === 'resource_package') sessionStorage.setItem('ycest-resource-task', task.id)
  try {
    // 仅选择队列使用此回调；先记录任务号，再传文件，刷新或断线后可核对原任务。
    options.onTaskCreated?.(task)
    // 直接上传浏览器 File 原始字节，不在前端改名或压缩。
    await request('put', `/uploads/${task.id}/content`, file, {
      timeout: 0,
      headers: { 'Content-Type': 'application/octet-stream' },

      /**
       * @brief 将上传字节数转换为当前任务的进度事件。
       * @param {Event} event 文件选择或进度事件，按具体回调读取 target 或 loaded。
       */
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

/**
 * @brief 只查询同一资源更新任务，READY 才表示目录替换完成；刷新可继续查询。
 * @param {string|number} id 服务端记录或任务标识。
 * @param {Function} onProgress 进度回调，参数包含任务 id、百分比和实际状态。
 */
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

  /**
   * @brief 读取登录页展示的设备公开信息。
   * @returns {Promise<*>} 公开设备信息。请求失败时抛出错误。
   */
  getDeviceInfo: () => request('get', '/device/info'),

  /**
   * @brief 保存热点名称和可选新密码，空密码不修改原连接密钥。
   * @param {string} ssid 待保存的热点名称。
   * @param {string} password 密码文本；热点接口空值表示保留原密码。
   * @returns {Promise<*>} 实际保存的热点配置。请求失败时抛出错误。
   */
  saveWifiSsid: (ssid, password = '') =>
    request('post', '/device/wifi', { ssid, ...(password ? { password } : {}) }),

  /**
   * @brief 登录后保存本次会话的 CSRF 令牌。
   * @param {object} credentials 登录账号和密码，仅用于本次认证。
   * @returns {Promise<*>} 会话资料及 CSRF 令牌。请求失败时抛出错误。
   */
  verifyPassword: async (credentials) => {
    const data = await request('post', '/auth/login', credentials)
    setCsrf(data.csrf)
    return data
  },
  session,

  /**
   * @brief 注销服务端会话并清除内存令牌。
   * @returns {Promise<void>} 注销完成后清除内存 CSRF；失败抛出错误。
   */
  logout: async () => {
    await request('post', '/auth/logout', {})
    setCsrf('')
  },

  /**
   * @brief 将设备 video 目录中的视频和图片清单转换为页面展示数据。
   * @returns {Promise<*>} video 目录的视频和图片清单，附带展示容量。请求失败时抛出错误。
   */
  playFileList: async () =>
    (await request('get', '/files')).files.map((file) => ({
      ...file,
      size: formatSize(file.bytes),
      duration: '—',
    })),

  /**
   * @brief 按文件版本标识请求删除设备视频或图片。
   * @param {object} args 包含目标记录 id 的请求对象。
   * @param {string|number} args.id 目标记录标识。
   * @returns {Promise<*>} 文件删除结果。请求失败时抛出错误。
   */
  deletePlayFile: ({ id }) => request('delete', `/files/${id}`, {}),

  /**
   * @brief 生成同源媒体下载地址。
   * @param {string|number} id 服务端记录或任务标识。
   * @returns {string} 携带文件标识的同源下载 URL，实际下载仍需有效会话。
   */
  downloadUrl: (id) => `/api/v1/files/${encodeURIComponent(id)}/content`,

  /**
   * @brief 创建视频上传任务并提交原始文件字节。
   * @param {File} file 浏览器选中的原始文件；上传时保留名称和字节。
   * @param {Function} progress 上传实际进度的接收回调。
   * @returns {Promise<*>} 视频上传及发布结果。请求失败时抛出错误。
   */
  uploadVideo: (file, progress) => uploadFile(file, 'video', '', progress),
  /**
   * @brief 广告图片与视频共用上传发布流程，小图选项只确定保存目录。
   * @param {File} file 浏览器选中的原始文件；上传时保留名称和字节。
   * @param {Function} progress 上传实际进度的接收回调。
   * @param {object} options 可选请求、上传或绘制配置，未提供时使用默认值。
   * @returns {Promise<*>} 按资源类型和小图选项发布后的任务结果。请求失败时抛出错误。
   */
  uploadMedia: (file, progress, options = {}) => uploadFile(file,
    /\.(png|jpe?g|bmp|gif|webp|tiff?)$/i.test(file.name) ? 'ad_image' : 'video', '', progress, options),
  uploadFile,

  /**
   * @brief 上传 Ycest 归档并等待 Qt 独立资源线程完成目录替换。
   * @param {File} file 浏览器选中的原始文件；上传时保留名称和字节。
   * @param {Function} progress 上传实际进度的接收回调。
   * @returns {Promise<*>} 资源目录更新完成的任务结果。请求失败时抛出错误。
   */
  uploadResourcePackage: (file, progress) => uploadFile(file, 'resource_package', '', progress),
  waitResourcePackage,

  /**
   * @brief 查询指定上传任务的当前状态。
   * @param {string|number} id 服务端记录或任务标识。
   * @returns {Promise<*>} 上传任务当前状态。请求失败时抛出错误。
   */
  uploadStatus: (id) => request('get', `/uploads/${id}`),

  /**
   * @brief 取消未完成的上传任务。
   * @param {string|number} id 服务端记录或任务标识。
   * @returns {Promise<*>} 取消任务结果。请求失败时抛出错误。
   */
  cancelUpload: (id) => request('post', `/uploads/${id}/cancel`, {}),

  /**
   * @brief 根据所选 Excel 和图片清单创建导入批次。
   * @param {Array<object>} manifest 导入文件清单，包含 name、size 和 kind。
   * @returns {Promise<*>} 新建导入批次。请求失败时抛出错误。
   */
  createImport: (manifest) => request('post', '/imports', { manifest }),

  /**
   * @brief 查询导入批次状态及结果。
   * @param {string|number} id 服务端记录或任务标识。
   * @returns {Promise<*>} 批次实际状态及分项结果。请求失败时抛出错误。
   */
  importStatus: (id) => request('get', `/imports/${id}`),

  /**
   * @brief 封存并启动人员导入批次。
   * @param {string|number} id 服务端记录或任务标识。
   * @returns {Promise<*>} 已封存批次的受理状态。请求失败时抛出错误。
   */
  startImport: (id) => request('post', `/imports/${id}/start`, {}),

  /**
   * @brief 丢弃尚未开始的导入批次。
   * @param {string|number} id 服务端记录或任务标识。
   * @returns {Promise<*>} 丢弃未开始批次的结果。请求失败时抛出错误。
   */
  discardImport: (id) => request('post', `/imports/${id}/discard`, {}),

  /** @brief 读取设备上的本地人员列表。 */
  personList: localApi.personList,

  /** @brief 按标识读取本地人员详情。 */
  personDetail: localApi.personDetail,
}
