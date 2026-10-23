/**
 * @file imageCompress.js
 * @brief 提供单项人脸录入的压缩及图片转换工具；Excel 批次上传仍保留原文件。
 * @author Du lin
 * @date 2026-10-09
 */

/**
 * @brief 通过 Canvas 限制尺寸并逐步降低 JPEG 质量，生成目标体积附近的 Data URL。
 * Canvas 图片压缩：将图片压到目标大小附近（默认 200~300KB），返回 base64。
 * 参考 yc_linux_web/src/utils/imageCompress.js。
 * 
 * @param {File|Blob} file 图片文件
 * @param {number} targetKB 目标体积（KB）
 * @param {number} maxSize 最大边长（px）
 * @returns {Promise<string>} 压缩后的 base64（dataURL）
 */
export function compressImage(file, targetKB = 250, maxSize = 1280) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.readAsDataURL(file)
    reader.onload = () => {
      const img = new Image()
      img.src = reader.result
      img.onload = () => {
        let quality = 0.9
        let { width, height } = img
        const scale = Math.min(1, maxSize / Math.max(width, height))
        width = Math.round(width * scale)
        height = Math.round(height * scale)

        const canvas = document.createElement('canvas')
        canvas.width = width
        canvas.height = height
        const ctx = canvas.getContext('2d')
        ctx.drawImage(img, 0, 0, width, height)

        /**
         * @brief 逐级降低 JPEG 质量直至接近目标体积。
         * @param {number} q 当前 JPEG 编码质量，通常为 0.1~0.9。
         */
        const compress = (q) => {
          const base64 = canvas.toDataURL('image/jpeg', q)
          const sizeKB = Math.round((base64.length * 3) / 4 / 1024)
          if (sizeKB <= targetKB || q <= 0.1) {
            resolve(base64)
          } else {
            compress(q - 0.1)
          }
        }
        compress(quality)
      }
      img.onerror = reject
    }
    reader.onerror = reject
  })
}

/**
 * @brief 将 Base64 Data URL 转换为保留 MIME 类型的二进制 Blob。
 * base64 转 Blob（供上传使用）
 * @param {string} dataUrl base64 dataURL
 * @returns {Blob}
 */
export function dataUrlToBlob(dataUrl) {
  const [head, body] = dataUrl.split(',')
  const mime = head.match(/:(.*?);/)[1]
  const bin = atob(body)
  const arr = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i)
  return new Blob([arr], { type: mime })
}
