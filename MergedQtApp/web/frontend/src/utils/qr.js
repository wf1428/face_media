/**
 * @file qr.js
 * @brief 保留原二维码绘制工具，本版本不连接设备控制。
 * @author Du lin
 * @date 2026-10-09
 */

import QRCode from 'qrcode'

/**
 * 生成二维码 Data URL（供 <img> 展示 / 点击放大预览）
 * @param {string} text 二维码内容（通行码/URL 等）
 * @param {object} options qrcode 库选项（margin/width/color 等）
 * @returns {Promise<string>} data:image/png;base64,...
 */
export function generateQrDataUrl(text, options = {}) {
  return QRCode.toDataURL(String(text), {
    margin: 2,
    width: 240,
    errorCorrectionLevel: 'M',
    ...options,
  })
}
