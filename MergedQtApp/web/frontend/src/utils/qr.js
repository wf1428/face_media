/**
 * @file qr.js
 * @brief 将二维码凭证内容绘制为可预览的图片，不负责串口控制。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-23；修改人：Du lin；新增设备通行二维码 JSON 封装，保留原编号及通用二维码绘制接口。
 */

import QRCode from 'qrcode'

/**
 * 按设备 parseOnlineQrCode 的 qrCode 字段格式封装凭证编号。
 * @param {string} qrUnique 已保存的二维码编号，不改名、不改变前导零。
 * @returns {string} 可直接供设备 JSON 解析的二维码文本。
 */
export function credentialQrPayload(qrUnique) {
  return JSON.stringify({ qrCode: String(qrUnique) })
}

/**
 * @brief 生成可在图片控件中展示的二维码 Data URL。
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
