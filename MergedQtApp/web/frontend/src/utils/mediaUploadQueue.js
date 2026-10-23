/**
 * @fileoverview 媒体待上传队列的合并、去重及容量校验，不读取文件内容。
 * @author Du lin
 * @date 2026-10-22
 */

const MiB = 1024 * 1024

/** @description 网页单次选择队列的上限；字节单位与设备上传限制一致。 */
export const MEDIA_UPLOAD_LIMITS = Object.freeze({
  files: 10,
  videoBytes: 500 * MiB,
  imageBytes: 100 * MiB,
  totalBytes: 5 * 1024 * MiB,
})

/**
 * 按现有上传接口支持的扩展名区分视频和广告图片。
 * @param {string} name 原始文件名，扩展名大小写不限。
 * @returns {string} video、ad_image；不支持的扩展名返回空字符串。
 */
export function mediaFileKind(name) {
  if (/\.(png|jpe?g|bmp|gif|webp|tiff?)$/i.test(name)) return 'ad_image'
  return /\.(mp4|avi|mov|mkv|ts|webm|m4v)$/i.test(name) ? 'video' : ''
}

/**
 * 将新增选择合并到当前队列，全部校验成功后才返回合并结果。
 * @param {File[]} current 当前队列中的原始文件。
 * @param {File[]} selected 本次文件选择器返回的原始文件。
 * @returns {{files: File[], added: number, skipped: number, error: object|null}}
 * 成功时保留原顺序并追加新文件；失败时返回原队列副本和翻译错误键。
 * 同名文件忽略大小写去重；需要替换时应先从队列移除旧项。
 */
export function mergeMediaSelection(current, selected) {
  const files = [...current]
  const names = new Set(current.map((file) => file.name.toLowerCase()))
  let skipped = 0
  for (const file of selected) {
    const name = file.name.toLowerCase()
    if (names.has(name)) {
      skipped++
      continue
    }
    names.add(name)
    files.push(file)
  }

  /**
   * 校验失败不提交本次新增文件，避免部分追加导致数量失真。
   * @param {string} key 本地化错误键。
   * @param {object} params 错误文案插值数据。
   * @returns {object} 原队列副本及失败信息。
   */
  const reject = (key, params = {}) => ({
    files: [...current], added: 0, skipped, error: { key, params },
  })
  if (files.length > MEDIA_UPLOAD_LIMITS.files) {
    return reject('video.queueCountLimit', {
      max: MEDIA_UPLOAD_LIMITS.files,
      count: current.length,
      remaining: Math.max(0, MEDIA_UPLOAD_LIMITS.files - current.length),
    })
  }
  for (const file of files) {
    if (!Number.isSafeInteger(file.size) || file.size <= 0)
      return reject('video.queueInvalidSize', { name: file.name })
  }
  if (files.reduce((total, file) => total + file.size, 0) > MEDIA_UPLOAD_LIMITS.totalBytes)
    return reject('video.queueTotalLimit')

  for (const file of files) {
    const kind = mediaFileKind(file.name)
    if (!kind) return reject('video.queueInvalidType', { name: file.name })
    const limit = kind === 'ad_image' ? MEDIA_UPLOAD_LIMITS.imageBytes : MEDIA_UPLOAD_LIMITS.videoBytes
    if (file.size > limit)
      return reject(kind === 'ad_image' ? 'video.queueImageLimit' : 'video.queueVideoLimit', { name: file.name })
  }
  return { files, added: files.length - current.length, skipped, error: null }
}
