/**
 * @file ota_protocol.cpp
 * @brief 用 JSON 字符串边界扫描保留 OTA 任务号的十进制原值。
 * @author Du lin
 * @date 2026-11-18
 */
#include "ota_protocol.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

QByteArray OtaProtocol::preserveTaskIds(const QByteArray &json)
{
    QByteArray result;
    result.reserve(json.size() + 16);
    int cursor = 0;
    while (cursor < json.size()) {
        if (json.at(cursor) != '"') { result += json.at(cursor++); continue; }
        const int start = cursor++;
        while (cursor < json.size()) {
            if (json.at(cursor) == '\\') { cursor += 2; continue; }
            if (json.at(cursor++) == '"') break;
        }
        result += json.mid(start, cursor - start);
        if (json.mid(start, cursor - start) != "\"taskId\"") continue;
        int value = cursor;
        while (value < json.size() && QByteArray(" \t\r\n").contains(json.at(value))) ++value;
        if (value == json.size() || json.at(value++) != ':') continue;
        while (value < json.size() && QByteArray(" \t\r\n").contains(json.at(value))) ++value;
        if (value == json.size() || json.at(value) == '"') continue;
        int end = value;
        while (end < json.size() && !QByteArray(",}] \t\r\n").contains(json.at(end))) ++end;
        if (end == value) continue;
        result += json.mid(cursor, value - cursor);
        result += '"'; result += json.mid(value, end - value); result += '"';
        cursor = end;
    }
    return result;
}

QString OtaProtocol::taskId(const QJsonObject &data)
{
    const auto value = data.value(QStringLiteral("taskId"));
    QString text;
    if (value.isString()) text = value.toString();
    else if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number) || number <= 0 || number > 9007199254740991.0 || std::floor(number) != number) return {};
        text = QString::number(qint64(number));
    }
    if (!QRegularExpression(QStringLiteral("^[1-9][0-9]{0,18}$")).match(text).hasMatch()) return {};
    bool ok = false;
    const auto number = text.toLongLong(&ok);
    return ok && number > 0 ? text : QString();
}

QByteArray OtaProtocol::encode(const QJsonObject &object)
{
    // Only this exact key is rewritten. Values have already passed signed-long validation.
    QString text = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    const QRegularExpression field(QStringLiteral("\"taskId\":\"([1-9][0-9]{0,18})\""));
    auto matches = field.globalMatch(text);
    QList<QPair<int, int>> spans;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!taskId({{QStringLiteral("taskId"), match.captured(1)}}).isEmpty())
            spans.prepend({match.capturedStart(1) - 1, match.capturedLength(1) + 2});
    }
    for (const auto &span : spans) text.replace(span.first, span.second, text.mid(span.first + 1, span.second - 2));
    return text.toUtf8();
}
