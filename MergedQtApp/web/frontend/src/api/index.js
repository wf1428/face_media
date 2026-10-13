/**
 * @file index.js
 * @brief V1 真实接口白名单，只接入登录、视频管理和本地 Excel/图片导入。
 * @author Du lin
 * @date 2026-10-09
 */
import { request, setCsrf } from './request'
/** @return 字节数展示文本。 */
export function formatSize(bytes) { return Number.isFinite(Number(bytes)) ? (bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MiB' : Math.max(1, Math.ceil(bytes / 1024)) + ' KiB') : '—' }
/** @return 刷新页时由 Cookie 恢复的真实会话。 */
async function session() { const data = await request('get', '/auth/session'); setCsrf(data.csrf); return data }
/** @return 上传并发布 File 的结果；失败查询同一任务，禁止盲目重建。 */
async function uploadFile(file, kind, batchId, onProgress = () => {}) {
  const task = await request('post', '/uploads', { name: file.name, size: file.size, kind, ...(batchId ? { batch_id: batchId } : {}) })
  try {
    await request('put', `/uploads/${task.id}/content`, file, { timeout: 0, headers: { 'Content-Type': 'application/octet-stream' }, onUploadProgress: event => onProgress({ id: task.id, percent: Math.min(99, Math.floor(event.loaded / file.size * 100)), state: 'RECEIVING' }) })
    onProgress({ id: task.id, percent: 99, state: 'VALIDATING' })
    const result = await request('post', `/uploads/${task.id}/commit`, {})
    onProgress({ id: task.id, percent: 100, state: result.state }); return result
  } catch (error) {
    try {
      const current = await request('get', `/uploads/${task.id}`)
      if (current.state === 'READY') return current
      if (['RECEIVED', 'VALIDATING', 'COMMITTING'].includes(current.state)) return await request('post', `/uploads/${task.id}/commit`, {})
    } catch { /* 原始错误及 taskId 留给页面，不把不确定结果报告成功。 */ }
    error.taskId = task.id; throw error
  }
}
export const api = {
  getDeviceInfo: () => request('get', '/device/info'),
  verifyPassword: async credentials => { const data = await request('post', '/auth/login', credentials); setCsrf(data.csrf); return data },
  session,
  logout: async () => { await request('post', '/auth/logout', {}); setCsrf('') },
  playFileList: async () => (await request('get', '/files')).files.map(file => ({ ...file, size: formatSize(file.bytes), duration: '—' })),
  deletePlayFile: ({ id }) => request('delete', `/files/${id}`, {}),
  downloadUrl: id => `/api/v1/files/${encodeURIComponent(id)}/content`,
  uploadVideo: (file, progress) => uploadFile(file, 'video', '', progress),
  uploadFile,
  uploadStatus: id => request('get', `/uploads/${id}`),
  cancelUpload: id => request('post', `/uploads/${id}/cancel`, {}),
  createImport: manifest => request('post', '/imports', { manifest }),
  importStatus: id => request('get', `/imports/${id}`),
  startImport: id => request('post', `/imports/${id}/start`, {}),
  discardImport: id => request('post', `/imports/${id}/discard`, {}),
  personList: async () => (await request('get', '/persons')).persons,
  personDetail: ({ id }) => request('get', `/persons/${encodeURIComponent(id)}`)
}
