/**
 * @file PersonXlsxParser.cpp
 * @brief 不依赖外部命令的轻量 XLSX/ZIP/XML 网络人员解析器。
 */

#include "PersonXlsxParser.h"

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QTime>
#include <QXmlStreamReader>

#include <cstring>

#include <zlib.h>

namespace {

quint16 little16(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 little32(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8)
            | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

struct ZipEntry
{
    quint16 method = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 localOffset = 0;
};

class XlsxArchive
{
public:
    bool open(const QString &path, QString *error)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法打开 XLSX：%1").arg(file.errorString());
            return false;
        }
        bytes_ = file.readAll();
        if (bytes_.size() < 22) {
            if (error) *error = QStringLiteral("XLSX 文件过小或已损坏");
            return false;
        }

        const quint32 eocdSignature = 0x06054b50;
        const int searchStart = qMax(0, bytes_.size() - 65557);
        int eocd = -1;
        for (int i = bytes_.size() - 22; i >= searchStart; --i) {
            if (little32(bytes_, i) == eocdSignature) {
                eocd = i;
                break;
            }
        }
        if (eocd < 0) {
            if (error) *error = QStringLiteral("XLSX ZIP 目录不存在");
            return false;
        }

        const quint16 entryCount = little16(bytes_, eocd + 10);
        int cursor = int(little32(bytes_, eocd + 16));
        for (quint16 i = 0; i < entryCount; ++i) {
            if (little32(bytes_, cursor) != 0x02014b50 || cursor + 46 > bytes_.size()) {
                if (error) *error = QStringLiteral("XLSX ZIP 目录项已损坏");
                return false;
            }
            const int nameLength = little16(bytes_, cursor + 28);
            const int extraLength = little16(bytes_, cursor + 30);
            const int commentLength = little16(bytes_, cursor + 32);
            if (cursor + 46 + nameLength + extraLength + commentLength > bytes_.size()) {
                if (error) *error = QStringLiteral("XLSX ZIP 目录项越界");
                return false;
            }
            const QString name = QString::fromUtf8(bytes_.mid(cursor + 46, nameLength));
            ZipEntry entry;
            entry.method = little16(bytes_, cursor + 10);
            entry.compressedSize = little32(bytes_, cursor + 20);
            entry.uncompressedSize = little32(bytes_, cursor + 24);
            entry.localOffset = little32(bytes_, cursor + 42);
            entries_.insert(name, entry);
            cursor += 46 + nameLength + extraLength + commentLength;
        }
        return true;
    }

    QByteArray read(const QString &name, QString *error) const
    {
        const auto it = entries_.constFind(name);
        if (it == entries_.constEnd()) {
            if (error) *error = QStringLiteral("XLSX 内缺少文件：%1").arg(name);
            return QByteArray();
        }
        const ZipEntry entry = it.value();
        const int local = int(entry.localOffset);
        if (little32(bytes_, local) != 0x04034b50 || local + 30 > bytes_.size()) {
            if (error) *error = QStringLiteral("XLSX ZIP 本地文件头已损坏：%1").arg(name);
            return QByteArray();
        }
        const int dataOffset = local + 30 + little16(bytes_, local + 26)
                + little16(bytes_, local + 28);
        if (dataOffset < 0 || dataOffset + int(entry.compressedSize) > bytes_.size()) {
            if (error) *error = QStringLiteral("XLSX ZIP 数据越界：%1").arg(name);
            return QByteArray();
        }
        const QByteArray compressed = bytes_.mid(dataOffset, int(entry.compressedSize));
        if (entry.method == 0) {
            return compressed;
        }
        if (entry.method != 8) {
            if (error) *error = QStringLiteral("XLSX 使用了不支持的压缩方式：%1").arg(entry.method);
            return QByteArray();
        }

        QByteArray output;
        output.resize(int(entry.uncompressedSize));
        z_stream stream;
        std::memset(&stream, 0, sizeof(stream));
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData()));
        stream.avail_in = uInt(compressed.size());
        stream.next_out = reinterpret_cast<Bytef *>(output.data());
        stream.avail_out = uInt(output.size());
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            if (error) *error = QStringLiteral("XLSX 解压初始化失败：%1").arg(name);
            return QByteArray();
        }
        const int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (result != Z_STREAM_END) {
            if (error) *error = QStringLiteral("XLSX 解压失败：%1").arg(name);
            return QByteArray();
        }
        output.resize(int(stream.total_out));
        return output;
    }

    bool contains(const QString &name) const { return entries_.contains(name); }

private:
    QByteArray bytes_;
    QHash<QString, ZipEntry> entries_;
};

QString normalizedTarget(QString target)
{
    target.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (target.startsWith(QStringLiteral("../"))) target.remove(0, 3);
    if (target.startsWith(QLatin1Char('/'))) target.remove(0, 1);
    if (!target.startsWith(QStringLiteral("xl/"))) target.prepend(QStringLiteral("xl/"));
    return target;
}

QString worksheetPath(const XlsxArchive &archive, QString *error)
{
    const QByteArray workbookXml = archive.read(QStringLiteral("xl/workbook.xml"), error);
    if (workbookXml.isEmpty()) return QString();
    QString relationshipId;
    QXmlStreamReader workbook(workbookXml);
    while (!workbook.atEnd()) {
        workbook.readNext();
        if (workbook.isStartElement() && workbook.name() == QStringLiteral("sheet")) {
            const auto attributes = workbook.attributes();
            if (attributes.value(QStringLiteral("name")).toString() == QStringLiteral("人员列表")
                    || relationshipId.isEmpty()) {
                relationshipId = attributes.value(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
                                                   QStringLiteral("id")).toString();
                if (attributes.value(QStringLiteral("name")).toString() == QStringLiteral("人员列表")) break;
            }
        }
    }
    if (workbook.hasError() || relationshipId.isEmpty()) {
        if (error) *error = QStringLiteral("XLSX 工作表信息无效");
        return QString();
    }

    const QByteArray relsXml = archive.read(QStringLiteral("xl/_rels/workbook.xml.rels"), error);
    if (relsXml.isEmpty()) return QString();
    QXmlStreamReader rels(relsXml);
    while (!rels.atEnd()) {
        rels.readNext();
        if (rels.isStartElement() && rels.name() == QStringLiteral("Relationship")) {
            const auto attributes = rels.attributes();
            if (attributes.value(QStringLiteral("Id")).toString() == relationshipId) {
                return normalizedTarget(attributes.value(QStringLiteral("Target")).toString());
            }
        }
    }
    if (error) *error = QStringLiteral("XLSX 工作表关系不存在");
    return QString();
}

QStringList sharedStrings(const XlsxArchive &archive, QString *error)
{
    QStringList strings;
    if (!archive.contains(QStringLiteral("xl/sharedStrings.xml"))) return strings;
    const QByteArray xml = archive.read(QStringLiteral("xl/sharedStrings.xml"), error);
    if (xml.isEmpty() && error && !error->isEmpty()) return strings;
    QXmlStreamReader reader(xml);
    QString current;
    bool inItem = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QStringLiteral("si")) {
            current.clear();
            inItem = true;
        } else if (reader.isStartElement() && reader.name() == QStringLiteral("t") && inItem) {
            current += reader.readElementText(QXmlStreamReader::IncludeChildElements);
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("si")) {
            strings.append(current);
            inItem = false;
        }
    }
    if (reader.hasError() && error) *error = QStringLiteral("XLSX 共享字符串解析失败：%1").arg(reader.errorString());
    return strings;
}

int columnIndex(const QString &cellReference)
{
    int result = 0;
    for (const QChar ch : cellReference) {
        if (!ch.isLetter()) break;
        result = result * 26 + (ch.toUpper().unicode() - QChar('A').unicode() + 1);
    }
    return result - 1;
}

QVector<QStringList> worksheetRows(const QByteArray &xml,
                                   const QStringList &strings,
                                   QString *error)
{
    QVector<QStringList> rows;
    QXmlStreamReader reader(xml);
    QStringList row;
    QString cellType;
    QString cellReference;
    QString value;
    bool inCell = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QStringLiteral("row")) {
            row.clear();
        } else if (reader.isStartElement() && reader.name() == QStringLiteral("c")) {
            inCell = true;
            cellType = reader.attributes().value(QStringLiteral("t")).toString();
            cellReference = reader.attributes().value(QStringLiteral("r")).toString();
            value.clear();
        } else if (reader.isStartElement() && inCell
                   && (reader.name() == QStringLiteral("v")
                       || reader.name() == QStringLiteral("t"))) {
            value += reader.readElementText(QXmlStreamReader::IncludeChildElements);
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("c")) {
            QString text = value;
            if (cellType == QStringLiteral("s")) {
                bool ok = false;
                const int index = value.toInt(&ok);
                text = ok && index >= 0 && index < strings.size() ? strings.at(index) : QString();
            } else if (cellType == QStringLiteral("b")) {
                text = value == QStringLiteral("1") ? QStringLiteral("true") : QStringLiteral("false");
            }
            const int column = columnIndex(cellReference);
            if (column >= 0) {
                while (row.size() <= column) row.append(QString());
                row[column] = text;
            }
            inCell = false;
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("row")) {
            rows.append(row);
        }
    }
    if (reader.hasError() && error) *error = QStringLiteral("XLSX 工作表解析失败：%1").arg(reader.errorString());
    return rows;
}

QString cell(const QStringList &row, const QHash<QString, int> &columns, const QString &name)
{
    const int index = columns.value(name, -1);
    return index >= 0 && index < row.size() ? row.at(index).trimmed() : QString();
}

QStringList commaValues(const QString &text)
{
    QStringList values = text.split(QLatin1Char(','), QString::SkipEmptyParts);
    for (QString &value : values) value = value.trimmed();
    values.removeAll(QString());
    return values;
}

QPair<double, double> moneyPair(QString text)
{
    text.remove(QChar(0x00a5));
    const QStringList parts = text.split(QLatin1Char('/'));
    bool firstOk = false;
    bool secondOk = false;
    const double first = parts.value(0).trimmed().toDouble(&firstOk);
    const double second = parts.value(1).trimmed().toDouble(&secondOk);
    return qMakePair(firstOk ? first : 0.0, secondOk ? second : (firstOk ? first : 0.0));
}

QPair<qlonglong, qlonglong> countPair(const QString &text)
{
    const QStringList parts = text.split(QLatin1Char('/'));
    bool firstOk = false;
    bool secondOk = false;
    const qlonglong first = parts.value(0).trimmed().toLongLong(&firstOk);
    const qlonglong second = parts.value(1).trimmed().toLongLong(&secondOk);
    return qMakePair(firstOk ? first : 0, secondOk ? second : (firstOk ? first : 0));
}

QJsonArray parseTimingRules(const QString &text)
{
    QJsonArray result;
    static const QRegularExpression expression(
                QStringLiteral("((?:周[一二三四五六日天][,，]?)+)\\s*(\\d{1,2}:\\d{2})(?::\\d{2})?\\s*[-~至]\\s*(\\d{1,2}:\\d{2})(?::\\d{2})?"));
    auto matchIterator = expression.globalMatch(text);
    while (matchIterator.hasNext()) {
        const QRegularExpressionMatch match = matchIterator.next();
        QJsonArray days;
        const QString dayText = match.captured(1);
        const QList<QPair<QString, int>> dayMap = {
            {QStringLiteral("周一"), 1}, {QStringLiteral("周二"), 2},
            {QStringLiteral("周三"), 3}, {QStringLiteral("周四"), 4},
            {QStringLiteral("周五"), 5}, {QStringLiteral("周六"), 6},
            {QStringLiteral("周日"), 7}, {QStringLiteral("周天"), 7}
        };
        for (const auto &entry : dayMap) {
            if (dayText.contains(entry.first)) days.append(entry.second);
        }
        QJsonArray range;
        range.append(match.captured(2) + QStringLiteral(":00"));
        range.append(match.captured(3) + QStringLiteral(":00"));
        QJsonObject timing;
        timing.insert(QStringLiteral("days"), days);
        timing.insert(QStringLiteral("timeRange"), range);
        result.append(timing);
    }
    return result;
}

QJsonArray parseFloors(const QString &text)
{
    QJsonArray result;
    const QStringList deviceParts = text.split(QLatin1Char(';'), QString::SkipEmptyParts);
    for (QString devicePart : deviceParts) {
        devicePart = devicePart.trimmed();
        const int colon = devicePart.indexOf(QLatin1Char(':'));
        if (colon <= 0) continue;
        const QString device = devicePart.left(colon).trimmed();
        QJsonArray floors;
        const QStringList numbers = commaValues(devicePart.mid(colon + 1));
        for (const QString &numberText : numbers) {
            bool ok = false;
            const int floor = numberText.toInt(&ok);
            if (ok) floors.append(floor);
        }
        if (!device.isEmpty() && !floors.isEmpty()) {
            QJsonObject item;
            item.insert(QStringLiteral("deviceId"), device);
            item.insert(QStringLiteral("floor"), floors);
            result.append(item);
        }
    }
    return result;
}

QJsonObject makeRecordData(const QStringList &row, const QHash<QString, int> &columns)
{
    const QString now = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QString personTypeText = cell(row, columns, QStringLiteral("人员类型"));
    const QString featuresText = cell(row, columns, QStringLiteral("启用功能"));
    const QStringList features = commaValues(featuresText);

    QJsonObject person;
    person.insert(QStringLiteral("name"), cell(row, columns, QStringLiteral("姓名")));
    person.insert(QStringLiteral("employeeNo"), cell(row, columns, QStringLiteral("工号")));
    person.insert(QStringLiteral("department"), cell(row, columns, QStringLiteral("部门")));
    person.insert(QStringLiteral("position"), cell(row, columns, QStringLiteral("职位")));
    person.insert(QStringLiteral("phone"), cell(row, columns, QStringLiteral("手机号")));
    person.insert(QStringLiteral("idCard"), cell(row, columns, QStringLiteral("身份证号")));
    const QDate hireDate = QDate::fromString(cell(row, columns, QStringLiteral("入职日期")), QStringLiteral("yyyy-MM-dd"));
    person.insert(QStringLiteral("hireDate"), hireDate.isValid()
                  ? QJsonValue(double(QDateTime(hireDate, QTime(0, 0)).toMSecsSinceEpoch()))
                  : QJsonValue());
    person.insert(QStringLiteral("hireDateText"), cell(row, columns, QStringLiteral("入职日期")));
    person.insert(QStringLiteral("personType"), personTypeText == QStringLiteral("员工")
                  ? QStringLiteral("EMPLOYEE")
                  : (personTypeText == QStringLiteral("访客") ? QStringLiteral("VISITOR") : personTypeText));
    person.insert(QStringLiteral("status"), 1);
    person.insert(QStringLiteral("remark"), QString());
    person.insert(QStringLiteral("account"), cell(row, columns, QStringLiteral("账号")));
    person.insert(QStringLiteral("initialPassword"), cell(row, columns, QStringLiteral("初始密码")));
    person.insert(QStringLiteral("groupName"), cell(row, columns, QStringLiteral("分组名称")));
    person.insert(QStringLiteral("hasFace"), cell(row, columns, QStringLiteral("是否有人脸")) == QStringLiteral("是"));
    person.insert(QStringLiteral("accessTemplateName"), cell(row, columns, QStringLiteral("通行模板名称")));
    person.insert(QStringLiteral("ruleTemplateName"), cell(row, columns, QStringLiteral("规则模板名称")));
    person.insert(QStringLiteral("enabledFeaturesText"), featuresText);
    person.insert(QStringLiteral("controlTypeText"), cell(row, columns, QStringLiteral("控梯类型")));
    person.insert(QStringLiteral("relayText"), cell(row, columns, QStringLiteral("继电器")));
    person.insert(QStringLiteral("remoteCallText"), cell(row, columns, QStringLiteral("远程呼梯")));

    QJsonArray cards;
    for (const QString &cardId : commaValues(cell(row, columns, QStringLiteral("IC卡号")))) {
        QJsonObject card;
        card.insert(QStringLiteral("cardId"), cardId);
        card.insert(QStringLiteral("name"), QStringLiteral("导入IC卡"));
        card.insert(QStringLiteral("bindTime"), now);
        cards.append(card);
    }

    QJsonArray qrCodes;
    for (const QString &qrUnique : commaValues(cell(row, columns, QStringLiteral("二维码")))) {
        QJsonObject qr;
        qr.insert(QStringLiteral("qrUnique"), qrUnique);
        qr.insert(QStringLiteral("qrName"), QStringLiteral("导入二维码"));
        qr.insert(QStringLiteral("qrType"), QStringLiteral("CREDENTIAL"));
        qr.insert(QStringLiteral("purpose"), QStringLiteral("ACCESS"));
        qr.insert(QStringLiteral("bindTime"), now);
        qrCodes.append(qr);
    }

    QJsonObject rules;
    auto enabled = [&](const QString &name) { return features.contains(name); };
    rules.insert(QStringLiteral("time"), enabled(QStringLiteral("定时")));
    rules.insert(QStringLiteral("count"), enabled(QStringLiteral("次数")));
    rules.insert(QStringLiteral("relay"), enabled(QStringLiteral("继电器")));
    rules.insert(QStringLiteral("amount"), enabled(QStringLiteral("金额")));
    rules.insert(QStringLiteral("duration"), enabled(QStringLiteral("期限")));
    rules.insert(QStringLiteral("remoteCall"), enabled(QStringLiteral("远程呼梯")));
    rules.insert(QStringLiteral("cardPassword"), enabled(QStringLiteral("密码通行"))
                 || cell(row, columns, QStringLiteral("密码通行")) == QStringLiteral("是"));
    rules.insert(QStringLiteral("controlElevator"), enabled(QStringLiteral("控梯")));

    const auto counts = countPair(cell(row, columns, QStringLiteral("剩余次数")));
    rules.insert(QStringLiteral("countDataRemaining"), double(counts.first));
    rules.insert(QStringLiteral("countDataTotal"), double(counts.second));
    bool countAddOk = false;
    const qlonglong countAdd = cell(row, columns, QStringLiteral("增量次数")).toLongLong(&countAddOk);
    rules.insert(QStringLiteral("countDataAddCount"), double(countAddOk ? countAdd : 0));

    const auto amounts = moneyPair(cell(row, columns, QStringLiteral("剩余金额")));
    rules.insert(QStringLiteral("amountDataRemaining"), amounts.first);
    rules.insert(QStringLiteral("amountDataTotal"), amounts.second);
    bool amountAddOk = false;
    QString amountAddText = cell(row, columns, QStringLiteral("增量金额"));
    amountAddText.remove(QChar(0x00a5));
    const double amountAdd = amountAddText.toDouble(&amountAddOk);
    rules.insert(QStringLiteral("amountDataAddAmount"), amountAddOk ? amountAdd : 0.0);
    QString priceText = cell(row, columns, QStringLiteral("单价"));
    priceText.remove(QChar(0x00a5));
    bool priceOk = false;
    const double price = priceText.toDouble(&priceOk);
    rules.insert(QStringLiteral("amountDataUnitPrice"), priceOk ? price : 0.0);

    const QString duration = cell(row, columns, QStringLiteral("有效期限"));
    const QStringList durationParts = duration.split(QRegularExpression(QStringLiteral("\\s*[~至]\\s*")));
    rules.insert(QStringLiteral("durationDataStartTime"), durationParts.value(0).trimmed());
    rules.insert(QStringLiteral("durationDataEndTime"), durationParts.value(1).trimmed());
    const QString controlType = cell(row, columns, QStringLiteral("控梯类型"));
    rules.insert(QStringLiteral("controlElevatorDataValueType"), controlType == QStringLiteral("直梯") ? 1 : 0);
    rules.insert(QStringLiteral("timingRules"), parseTimingRules(cell(row, columns, QStringLiteral("定时规则"))));
    rules.insert(QStringLiteral("timingRuleText"), cell(row, columns, QStringLiteral("定时规则")));
    rules.insert(QStringLiteral("remoteCallText"), cell(row, columns, QStringLiteral("远程呼梯")));
    rules.insert(QStringLiteral("relayText"), cell(row, columns, QStringLiteral("继电器")));

    QJsonObject data;
    data.insert(QStringLiteral("person"), person);
    data.insert(QStringLiteral("icCards"), cards);
    data.insert(QStringLiteral("qrCodes"), qrCodes);
    data.insert(QStringLiteral("rules"), rules);
    data.insert(QStringLiteral("floors"), parseFloors(cell(row, columns, QStringLiteral("设备及楼层"))));
    return data;
}

} // namespace

PersonXlsxParseResult PersonXlsxParser::parse(const QString &filePath)
{
    PersonXlsxParseResult result;
    XlsxArchive archive;
    if (!archive.open(filePath, &result.error)) return result;

    const QString sheetPath = worksheetPath(archive, &result.error);
    if (sheetPath.isEmpty()) return result;
    const QStringList strings = sharedStrings(archive, &result.error);
    if (!result.error.isEmpty()) return result;
    const QByteArray sheetXml = archive.read(sheetPath, &result.error);
    if (sheetXml.isEmpty()) return result;
    const QVector<QStringList> rows = worksheetRows(sheetXml, strings, &result.error);
    if (!result.error.isEmpty() || rows.isEmpty()) {
        if (result.error.isEmpty()) result.error = QStringLiteral("XLSX 中没有人员数据");
        return result;
    }

    return parseRows(rows, QStringLiteral("XLSX"));
}

PersonXlsxParseResult PersonXlsxParser::parseRows(const QVector<QStringList> &rows,
                                                  const QString &formatName)
{
    PersonXlsxParseResult result;
    const QString displayFormat = formatName.trimmed().isEmpty()
            ? QStringLiteral("Excel") : formatName.trimmed();
    if (rows.isEmpty()) {
        result.error = QStringLiteral("%1 中没有人员数据").arg(displayFormat);
        return result;
    }

    QHash<QString, int> columns;
    for (int i = 0; i < rows.first().size(); ++i) columns.insert(rows.first().at(i).trimmed(), i);
    const QStringList requiredHeaders = {
        QStringLiteral("姓名"), QStringLiteral("账号"), QStringLiteral("工号"),
        QStringLiteral("手机号"), QStringLiteral("IC卡号"), QStringLiteral("二维码"),
        QStringLiteral("设备及楼层"), QStringLiteral("启用功能")
    };
    for (const QString &header : requiredHeaders) {
        if (!columns.contains(header)) {
            result.error = QStringLiteral("%1 缺少必要列：%2")
                    .arg(displayFormat, header);
            return result;
        }
    }

    QSet<QString> accountsInFile;
    for (int rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
        const QStringList &row = rows.at(rowIndex);
        const QString sequence = cell(row, columns, QStringLiteral("序号"));
        const QString name = cell(row, columns, QStringLiteral("姓名"));
        const QString account = cell(row, columns, QStringLiteral("账号"));
        if (sequence == QStringLiteral("自动生成")) continue;
        if (name == QStringLiteral("张三") && account == QStringLiteral("zhangsan")
                && cell(row, columns, QStringLiteral("工号")) == QStringLiteral("EMP001")) {
            continue;
        }
        if (name.isEmpty() && account.isEmpty()) continue;
        if (name.isEmpty() || account.isEmpty()) {
            result.warnings.append(QStringLiteral("第 %1 行缺少姓名或账号，已跳过").arg(rowIndex + 1));
            continue;
        }
        const QString normalizedAccount = account.toCaseFolded();
        if (accountsInFile.contains(normalizedAccount)) {
            result.warnings.append(QStringLiteral("第 %1 行账号“%2”在文件内重复，已跳过")
                                   .arg(rowIndex + 1).arg(account));
            continue;
        }
        accountsInFile.insert(normalizedAccount);
        PersonSpreadsheetRecord record;
        record.name = name;
        record.account = account;
        record.sourceRow = rowIndex + 1;
        record.data = makeRecordData(row, columns);
        result.records.append(record);
    }

    if (result.records.isEmpty()) {
        result.error = QStringLiteral("%1 中没有可导入的人员记录")
                .arg(displayFormat);
        return result;
    }
    result.ok = true;
    return result;
}
