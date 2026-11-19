/**
 * @file ota_protocol.h
 * @brief Qt 5 下 OTA long 任务号的无损解析和数值序列化。
 * @author Du lin
 * @date 2026-11-18
 */
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

/** @brief OTA 协议纯值工具，可在 IPC 后台线程调用，不保存输入或 FTP 凭据。 */
namespace OtaProtocol {
/** @brief 在 JSON 解析前将 taskId 数字 token 转为内部字符串，保护 64 位精度。
 * @param json 合法或待校验的 UTF-8 JSON，不修改字符串正文。
 * @return 仅 taskId 数字 token 加引号的 JSON；格式合法性仍由 QJsonDocument 校验。
 */
QByteArray preserveTaskIds(const QByteArray &json);
/** @brief 从内部字符串或精确可表示的 JSON 数字读取正 long ID。
 * @param data 含 taskId 的协议 data 对象。
 * @return 正十进制 ID；缺失、浮点、越界或不精确的数值返回空串。
 */
QString taskId(const QJsonObject &data);
/** @brief 序列化协议对象，内部 taskId 字符串恢复为 JSON long 数字。
 * @param object 不包含不受信任内部字段的业务信封。
 * @return UTF-8 JSON；仅已验证的 taskId 转为数值 token。
 */
QByteArray encode(const QJsonObject &object);
}
