/**
 * @file mqttpacketparser.h
 * @brief 后台解析mqttd标准及旧格式信封；不访问QObject、UI或声卡。
 * @author Du lin
 * @date 2026-11-03
 */
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
/** @brief 纯值解析结果，由工作任务生成、UI消费；payload字符串已解析为对象。 */
struct MqttParsedPacket {
    QJsonObject object; /**< 标准化信封；日志/无效输入时为空。 */
    QString display,error; /**< 小型原日志摘要及兼容解析错误；不输出大包正文。 */
};
/** @brief 解析标准、topic+JSON及原坏格式回退，业务字符串payload在后台完成二次解析。
 * @param[in] raw 已完整组帧的数据，所有权由值对象共享；函数不保存外部引用。
 * @return 标准化结果及诊断；无效输入object为空。
 * @note 可在工作线程调用，不等待设备、不调用UI；大数据只能由工作任务调用。
 */
MqttParsedPacket parseMqttPacket(const QByteArray &raw);
