/**
 * @file PersonXlsParser.cpp
 * @brief 自包含的 OLE Compound File 与 BIFF8 只读解析实现。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "PersonXlsParser.h"

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QtMath>
#include <QtGlobal>

#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr quint32 kEndOfChain = 0xFFFFFFFEu;
constexpr quint32 kFreeSector = 0xFFFFFFFFu;

/** @return data 中指定偏移处的小端 16 位整数；越界时返回 0。 */
quint16 read16(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

/** @return data 中指定偏移处的小端 32 位整数；越界时返回 0。 */
quint32 read32(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8)
            | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

/** @return 由相邻两个小端 32 位值组成的 64 位整数。 */
quint64 read64(const QByteArray &data, int offset)
{
    return quint64(read32(data, offset)) | (quint64(read32(data, offset + 4)) << 32);
}

/** @return 按 IEEE 754 位模式读取的双精度值。 */
double readDouble(const QByteArray &data, int offset)
{
    const quint64 bits = read64(data, offset);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/** @return 指定位置的 UTF-16LE 文本；范围无效时返回空字符串。 */
QString utf16Text(const QByteArray &data, int offset, int characterCount)
{
    if (characterCount <= 0 || offset < 0 || offset + characterCount * 2 > data.size()) {
        return QString();
    }
    QVector<ushort> characters;
    characters.reserve(characterCount);
    for (int i = 0; i < characterCount; ++i) {
        characters.append(read16(data, offset + i * 2));
    }
    return QString::fromUtf16(characters.constData(), characters.size());
}

/** @return BIFF8 单字节压缩 Unicode 文本；范围无效时返回空字符串。 */
QString compressedUnicodeText(const QByteArray &data, int offset, int characterCount)
{
    if (characterCount <= 0 || offset < 0 || offset + characterCount > data.size()) {
        return QString();
    }
    QString text;
    text.reserve(characterCount);
    for (int i = 0; i < characterCount; ++i) {
        text.append(QChar(uchar(data.at(offset + i))));
    }
    return text;
}

/** @brief 只读解析 OLE Compound File FAT、Mini FAT、目录和 Workbook 数据流。 */
class CompoundFile
{
public:
    /**
     * @brief 校验 OLE 头并建立 FAT、Mini FAT 与目录索引。
     *
     * 所有扇区链都检测越界和循环，避免损坏文件导致无限循环或越界读取。
     */
    bool open(const QString &filePath, QString *error)
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法打开 XLS：%1").arg(file.errorString());
            return false;
        }
        bytes_ = file.readAll();
        static const QByteArray signature = QByteArray::fromHex("D0CF11E0A1B11AE1");
        if (bytes_.size() < 512 || bytes_.left(8) != signature) {
            if (error) *error = QStringLiteral("文件不是有效的 Excel 97-2003 OLE 文档");
            return false;
        }
        const quint16 majorVersion = read16(bytes_, 26);
        const quint16 sectorShift = read16(bytes_, 30);
        const quint16 miniSectorShift = read16(bytes_, 32);
        if ((majorVersion != 3 && majorVersion != 4)
                || (sectorShift != 9 && sectorShift != 12)
                || miniSectorShift != 6) {
            if (error) *error = QStringLiteral("XLS 使用了不支持的 OLE 版本");
            return false;
        }
        majorVersion_ = majorVersion;
        sectorSize_ = 1 << sectorShift;
        miniSectorSize_ = 1 << miniSectorShift;
        miniCutoff_ = read32(bytes_, 56);

        const quint32 fatSectorCount = read32(bytes_, 44);
        const quint32 firstDirectorySector = read32(bytes_, 48);
        const quint32 firstMiniFatSector = read32(bytes_, 60);
        const quint32 miniFatSectorCount = read32(bytes_, 64);
        quint32 nextDifatSector = read32(bytes_, 68);
        const quint32 difatSectorCount = read32(bytes_, 72);

        QVector<quint32> fatSectorIds;
        for (int i = 0; i < 109; ++i) {
            const quint32 sector = read32(bytes_, 76 + i * 4);
            if (sector != kFreeSector) fatSectorIds.append(sector);
        }
        QSet<quint32> visitedDifat;
        const int difatEntriesPerSector = sectorSize_ / 4 - 1;
        for (quint32 i = 0; i < difatSectorCount && nextDifatSector <= 0xFFFFFFFAu; ++i) {
            if (visitedDifat.contains(nextDifatSector)) {
                if (error) *error = QStringLiteral("XLS 的 DIFAT 扇区链存在循环");
                return false;
            }
            visitedDifat.insert(nextDifatSector);
            const QByteArray sector = sectorBytes(nextDifatSector);
            if (sector.size() != sectorSize_) {
                if (error) *error = QStringLiteral("XLS 的 DIFAT 扇区越界");
                return false;
            }
            for (int entry = 0; entry < difatEntriesPerSector; ++entry) {
                const quint32 fatSectorId = read32(sector, entry * 4);
                if (fatSectorId != kFreeSector) fatSectorIds.append(fatSectorId);
            }
            nextDifatSector = read32(sector, sectorSize_ - 4);
        }
        if (fatSectorIds.size() < int(fatSectorCount)) {
            if (error) *error = QStringLiteral("XLS 的 FAT 扇区数量不完整");
            return false;
        }
        for (quint32 i = 0; i < fatSectorCount; ++i) {
            const QByteArray sector = sectorBytes(fatSectorIds.at(int(i)));
            if (sector.size() != sectorSize_) {
                if (error) *error = QStringLiteral("XLS 的 FAT 扇区越界");
                return false;
            }
            for (int offset = 0; offset < sectorSize_; offset += 4) {
                fat_.append(read32(sector, offset));
            }
        }

        const QByteArray directory = readRegularChain(firstDirectorySector, -1, error);
        if (directory.isEmpty()) return false;
        for (int offset = 0; offset + 128 <= directory.size(); offset += 128) {
            const int nameByteLength = read16(directory, offset + 64);
            const quint8 objectType = quint8(directory.at(offset + 66));
            if (objectType == 0 || nameByteLength < 2 || nameByteLength > 64) continue;
            DirectoryEntry entry;
            entry.name = utf16Text(directory, offset, nameByteLength / 2 - 1);
            entry.type = objectType;
            entry.startSector = read32(directory, offset + 116);
            entry.size = majorVersion_ == 3
                    ? quint64(read32(directory, offset + 120))
                    : read64(directory, offset + 120);
            directory_.append(entry);
            if (entry.type == 5) root_ = entry;
        }

        if (miniFatSectorCount > 0 && firstMiniFatSector <= 0xFFFFFFFAu) {
            const QByteArray miniFatBytes = readRegularChain(
                        firstMiniFatSector, qint64(miniFatSectorCount) * sectorSize_, error);
            if (miniFatBytes.isEmpty()) return false;
            for (int offset = 0; offset + 4 <= miniFatBytes.size(); offset += 4) {
                miniFat_.append(read32(miniFatBytes, offset));
            }
        }
        if (root_.startSector <= 0xFFFFFFFAu && root_.size > 0) {
            rootMiniStream_ = readRegularChain(root_.startSector, qint64(root_.size), error);
            if (rootMiniStream_.isEmpty() && root_.size > 0) return false;
        }
        return true;
    }

    /** @return 名为 Workbook 或 Book 的 BIFF 数据流。 */
    QByteArray workbookStream(QString *error) const
    {
        for (const DirectoryEntry &entry : directory_) {
            if (entry.type == 2
                    && (entry.name.compare(QStringLiteral("Workbook"), Qt::CaseInsensitive) == 0
                        || entry.name.compare(QStringLiteral("Book"), Qt::CaseInsensitive) == 0)) {
                if (entry.size < miniCutoff_) {
                    return readMiniChain(entry.startSector, qint64(entry.size), error);
                }
                return readRegularChain(entry.startSector, qint64(entry.size), error);
            }
        }
        if (error) *error = QStringLiteral("XLS 中缺少 Workbook 数据流");
        return QByteArray();
    }

private:
    /** @brief OLE 目录中的存储对象定位信息。 */
    struct DirectoryEntry
    {
        QString name;                           /**< 目录项名称。 */
        quint8 type = 0;                        /**< OLE 对象类型。 */
        quint32 startSector = kEndOfChain;      /**< 数据链首扇区。 */
        quint64 size = 0;                       /**< 数据流长度，单位字节。 */
    };

    /** @return 指定普通扇区的完整数据；编号或范围无效时返回空数组。 */
    QByteArray sectorBytes(quint32 sector) const
    {
        if (sector > 0xFFFFFFFAu) return QByteArray();
        const quint64 offset = (quint64(sector) + 1u) * quint64(sectorSize_);
        if (offset + quint64(sectorSize_) > quint64(bytes_.size())) return QByteArray();
        return bytes_.mid(int(offset), sectorSize_);
    }

    /** @brief 沿 FAT 链读取普通扇区，并按 expectedSize 截断结果。 */
    QByteArray readRegularChain(quint32 startSector, qint64 expectedSize, QString *error) const
    {
        if (expectedSize > std::numeric_limits<int>::max()) {
            if (error) *error = QStringLiteral("XLS 数据流过大");
            return QByteArray();
        }
        QByteArray result;
        QSet<quint32> visited;
        quint32 sector = startSector;
        while (sector <= 0xFFFFFFFAu) {
            if (visited.contains(sector) || int(sector) >= fat_.size()) {
                if (error) *error = QStringLiteral("XLS 的 FAT 数据链无效");
                return QByteArray();
            }
            visited.insert(sector);
            const QByteArray data = sectorBytes(sector);
            if (data.size() != sectorSize_) {
                if (error) *error = QStringLiteral("XLS 的数据扇区越界");
                return QByteArray();
            }
            result.append(data);
            if (expectedSize >= 0 && result.size() >= expectedSize) break;
            sector = fat_.at(int(sector));
            if (sector == kEndOfChain) break;
            if (visited.size() > fat_.size()) {
                if (error) *error = QStringLiteral("XLS 的 FAT 数据链过长");
                return QByteArray();
            }
        }
        if (expectedSize >= 0) {
            if (result.size() < expectedSize) {
                if (error) *error = QStringLiteral("XLS 数据流长度不足");
                return QByteArray();
            }
            result.truncate(int(expectedSize));
        }
        return result;
    }

    /** @brief 沿 Mini FAT 链从根 Mini Stream 读取小数据流。 */
    QByteArray readMiniChain(quint32 startSector, qint64 expectedSize, QString *error) const
    {
        if (expectedSize > std::numeric_limits<int>::max()) {
            if (error) *error = QStringLiteral("XLS Mini FAT 数据流过大");
            return QByteArray();
        }
        QByteArray result;
        QSet<quint32> visited;
        quint32 sector = startSector;
        while (sector <= 0xFFFFFFFAu && result.size() < expectedSize) {
            if (visited.contains(sector) || int(sector) >= miniFat_.size()) {
                if (error) *error = QStringLiteral("XLS 的 Mini FAT 数据链无效");
                return QByteArray();
            }
            visited.insert(sector);
            const quint64 offset = quint64(sector) * quint64(miniSectorSize_);
            if (offset + quint64(miniSectorSize_) > quint64(rootMiniStream_.size())) {
                if (error) *error = QStringLiteral("XLS 的 Mini FAT 数据扇区越界");
                return QByteArray();
            }
            result.append(rootMiniStream_.mid(int(offset), miniSectorSize_));
            sector = miniFat_.at(int(sector));
            if (sector == kEndOfChain) break;
        }
        if (result.size() < expectedSize) {
            if (error) *error = QStringLiteral("XLS 的 Mini FAT 数据流长度不足");
            return QByteArray();
        }
        result.truncate(int(expectedSize));
        return result;
    }

    QByteArray bytes_;                 /**< 完整 OLE 文件内容。 */
    int majorVersion_ = 3;             /**< OLE 主版本，3 或 4。 */
    int sectorSize_ = 512;             /**< 普通扇区大小，单位字节。 */
    int miniSectorSize_ = 64;          /**< Mini FAT 扇区大小，单位字节。 */
    quint32 miniCutoff_ = 4096;        /**< 使用 Mini FAT 的数据流长度阈值。 */
    QVector<quint32> fat_;             /**< 普通扇区链表。 */
    QVector<quint32> miniFat_;         /**< 小扇区链表。 */
    QVector<DirectoryEntry> directory_; /**< OLE 目录项。 */
    DirectoryEntry root_;              /**< 根存储目录项。 */
    QByteArray rootMiniStream_;         /**< Mini FAT 的承载数据流。 */
};

/** @brief 跨 SST 与 CONTINUE 记录连续读取字节的游标。 */
class SegmentedBytes
{
public:
    /** @brief 绑定按 BIFF 记录边界切分的数据段。 */
    explicit SegmentedBytes(const QVector<QByteArray> &segments)
        : segments_(segments)
    {
    }

    /** @return 当前数据段尚未读取的字节数。 */
    int remaining() const
    {
        return segment_ < segments_.size() ? segments_.at(segment_).size() - offset_ : 0;
    }

    /** @brief 移动到下一 CONTINUE 数据段。 */
    bool nextSegment()
    {
        if (segment_ + 1 >= segments_.size()) return false;
        ++segment_;
        offset_ = 0;
        return true;
    }

    /** @brief 读取一个字节，必要时跨越数据段边界。 */
    bool readByte(quint8 *value)
    {
        QByteArray byte;
        if (!readRaw(1, &byte)) return false;
        *value = quint8(byte.at(0));
        return true;
    }

    /** @brief 读取一个小端 16 位整数。 */
    bool readUnsigned16(quint16 *value)
    {
        QByteArray bytes;
        if (!readRaw(2, &bytes)) return false;
        *value = read16(bytes, 0);
        return true;
    }

    /** @brief 读取一个小端 32 位整数。 */
    bool readUnsigned32(quint32 *value)
    {
        QByteArray bytes;
        if (!readRaw(4, &bytes)) return false;
        *value = read32(bytes, 0);
        return true;
    }

    /** @brief 跨段读取指定字节数。 */
    bool readRaw(int count, QByteArray *output)
    {
        output->clear();
        output->reserve(count);
        while (count > 0) {
            if (remaining() == 0 && !nextSegment()) return false;
            const int take = qMin(count, remaining());
            output->append(segments_.at(segment_).mid(offset_, take));
            offset_ += take;
            count -= take;
        }
        return true;
    }

    /** @brief 跨段跳过指定字节数。 */
    bool skipRaw(int count)
    {
        QByteArray ignored;
        return readRaw(count, &ignored);
    }

    /** @return 仅从当前段取得最多 count 个字节。 */
    QByteArray takeFromCurrent(int count)
    {
        const int take = qMin(count, remaining());
        const QByteArray result = segments_.at(segment_).mid(offset_, take);
        offset_ += take;
        return result;
    }

private:
    QVector<QByteArray> segments_; /**< SST 主记录及其 CONTINUE 数据段。 */
    int segment_ = 0;              /**< 当前数据段索引。 */
    int offset_ = 0;               /**< 当前段内偏移。 */
};

/**
 * @brief 解析一条可跨 CONTINUE 记录的 BIFF8 SST 字符串。
 *
 * BIFF8 允许续接段改变单字节/双字节编码标志，因此每次跨段都重新读取标志位。
 */
bool parseSstString(SegmentedBytes *cursor, QString *text, QString *error)
{
    quint16 characterCount = 0;
    quint8 flags = 0;
    if (!cursor->readUnsigned16(&characterCount) || !cursor->readByte(&flags)) {
        if (error) *error = QStringLiteral("XLS 的共享字符串头不完整");
        return false;
    }
    quint16 formatRuns = 0;
    quint32 extendedSize = 0;
    if ((flags & 0x08u) && !cursor->readUnsigned16(&formatRuns)) return false;
    if ((flags & 0x04u) && !cursor->readUnsigned32(&extendedSize)) return false;

    bool highByte = (flags & 0x01u) != 0;
    int charactersRemaining = characterCount;
    text->clear();
    text->reserve(characterCount);
    while (charactersRemaining > 0) {
        if (cursor->remaining() == 0) {
            if (!cursor->nextSegment()) {
                if (error) *error = QStringLiteral("XLS 的共享字符串被意外截断");
                return false;
            }
            quint8 continuationFlags = 0;
            if (!cursor->readByte(&continuationFlags)) return false;
            highByte = (continuationFlags & 0x01u) != 0;
        }
        const int bytesPerCharacter = highByte ? 2 : 1;
        const int fit = cursor->remaining() / bytesPerCharacter;
        if (fit <= 0) {
            if (error) *error = QStringLiteral("XLS 的共享字符串续接位置无效");
            return false;
        }
        const int takeCharacters = qMin(charactersRemaining, fit);
        const QByteArray bytes = cursor->takeFromCurrent(takeCharacters * bytesPerCharacter);
        *text += highByte ? utf16Text(bytes, 0, takeCharacters)
                          : compressedUnicodeText(bytes, 0, takeCharacters);
        charactersRemaining -= takeCharacters;
    }
    if (!cursor->skipRaw(int(formatRuns) * 4 + int(extendedSize))) {
        if (error) *error = QStringLiteral("XLS 的共享字符串格式数据不完整");
        return false;
    }
    return true;
}

/** @brief 解析 SST 及其 CONTINUE 记录，生成按索引排列的共享字符串表。 */
QStringList parseSharedStrings(const QVector<QByteArray> &segments, QString *error)
{
    QStringList strings;
    if (segments.isEmpty() || segments.first().size() < 8) {
        if (error) *error = QStringLiteral("XLS 的共享字符串表不完整");
        return strings;
    }
    SegmentedBytes cursor(segments);
    quint32 totalCount = 0;
    quint32 uniqueCount = 0;
    if (!cursor.readUnsigned32(&totalCount) || !cursor.readUnsigned32(&uniqueCount)) return strings;
    Q_UNUSED(totalCount);
    if (uniqueCount > 1000000u) {
        if (error) *error = QStringLiteral("XLS 的共享字符串数量异常");
        return strings;
    }
    strings.reserve(int(uniqueCount));
    for (quint32 i = 0; i < uniqueCount; ++i) {
        QString text;
        if (!parseSstString(&cursor, &text, error)) return QStringList();
        strings.append(text);
    }
    return strings;
}

/** @return BIFF8 短 Unicode 字符串。 */
QString shortUnicodeString(const QByteArray &data, int offset)
{
    if (offset + 2 > data.size()) return QString();
    const int count = quint8(data.at(offset));
    const quint8 flags = quint8(data.at(offset + 1));
    return (flags & 0x01u) ? utf16Text(data, offset + 2, count)
                           : compressedUnicodeText(data, offset + 2, count);
}

/** @return BIFF8 常规 Unicode 字符串。 */
QString unicodeString(const QByteArray &data, int offset)
{
    if (offset + 3 > data.size()) return QString();
    const int count = read16(data, offset);
    const quint8 flags = quint8(data.at(offset + 2));
    return (flags & 0x01u) ? utf16Text(data, offset + 3, count)
                           : compressedUnicodeText(data, offset + 3, count);
}

/** @return 解码后的 BIFF RK 压缩数值。 */
double rkValue(quint32 encoded)
{
    const bool dividedBy100 = (encoded & 0x01u) != 0;
    const bool integer = (encoded & 0x02u) != 0;
    double value = 0.0;
    if (integer) {
        value = double(qint32(encoded) >> 2);
    } else {
        const quint64 bits = quint64(encoded & 0xFFFFFFFCu) << 32;
        std::memcpy(&value, &bits, sizeof(value));
    }
    return dividedBy100 ? value / 100.0 : value;
}

/**
 * @brief 按 XF 格式把 BIFF 数值转换为日期、补零文本或普通数值文本。
 * @param date1904  工作簿使用 1904 日期系统时为 true。
 */
QString formatNumber(double value,
                     quint16 xfIndex,
                     const QVector<quint16> &xfFormats,
                     const QHash<quint16, QString> &customFormats,
                     bool date1904)
{
    const quint16 formatIndex = xfIndex < xfFormats.size() ? xfFormats.at(xfIndex) : 0;
    const QString customFormat = customFormats.value(formatIndex);
    const QString normalized = customFormat.toLower();
    const bool builtInDate = formatIndex >= 14 && formatIndex <= 22;
    const bool customDate = !normalized.isEmpty()
            && normalized.contains(QLatin1Char('y'))
            && normalized.contains(QLatin1Char('d'));
    if (builtInDate || customDate) {
        const QDate baseDate = date1904 ? QDate(1904, 1, 1) : QDate(1899, 12, 30);
        return baseDate.addDays(qFloor(value)).toString(QStringLiteral("yyyy-MM-dd"));
    }
    if (!customFormat.isEmpty()) {
        QString zeroMask = customFormat;
        zeroMask.remove(QLatin1Char('"'));
        bool onlyZeros = !zeroMask.isEmpty();
        for (const QChar ch : zeroMask) {
            if (ch != QLatin1Char('0')) {
                onlyZeros = false;
                break;
            }
        }
        if (onlyZeros && std::isfinite(value)) {
            return QStringLiteral("%1").arg(qRound64(value), zeroMask.size(), 10, QLatin1Char('0'));
        }
    }
    const double rounded = std::round(value);
    if (std::isfinite(value) && std::fabs(value - rounded) < 1e-9
            && rounded >= double(std::numeric_limits<qint64>::min())
            && rounded <= double(std::numeric_limits<qint64>::max())) {
        return QString::number(qint64(rounded));
    }
    return QString::number(value, 'g', 15);
}

/** @brief 扩展稀疏二维行并设置指定单元格；异常行列号直接忽略。 */
void setCell(QVector<QStringList> *rows, int row, int column, const QString &value)
{
    if (row < 0 || row > 100000 || column < 0 || column > 255) return;
    while (rows->size() <= row) rows->append(QStringList());
    QStringList &target = (*rows)[row];
    while (target.size() <= column) target.append(QString());
    target[column] = value;
}

/** @brief BIFF BOUNDSHEET 记录中的工作表定位信息。 */
struct SheetDescriptor
{
    quint32 offset = 0; /**< 工作表子流在 Workbook 中的偏移。 */
    QString name;       /**< 工作表名称。 */
    quint8 type = 0;    /**< 工作表类型，0 表示普通工作表。 */
};

/**
 * @brief 解析 BIFF8 全局记录和目标工作表单元格记录。
 *
 * 优先选择“人员列表”工作表，否则使用第一张普通工作表；公式单元格使用
 * 缓存结果，字符串结果需等待紧随其后的 STRING 记录。
 */
bool parseWorkbook(const QByteArray &workbook,
                   QVector<QStringList> *rows,
                   QString *error)
{
    if (workbook.size() < 8 || read16(workbook, 0) != 0x0809u
            || read16(workbook, 4) != 0x0600u) {
        if (error) *error = QStringLiteral("仅支持 Excel 97-2003 BIFF8 格式的 XLS 文件");
        return false;
    }
    QVector<SheetDescriptor> sheets;
    QStringList sharedStrings;
    QHash<quint16, QString> customFormats;
    QVector<quint16> xfFormats;
    bool date1904 = false;
    int cursor = 0;
    while (cursor + 4 <= workbook.size()) {
        const quint16 recordType = read16(workbook, cursor);
        const int recordSize = read16(workbook, cursor + 2);
        const int payloadOffset = cursor + 4;
        if (payloadOffset + recordSize > workbook.size()) {
            if (error) *error = QStringLiteral("XLS 的 BIFF 全局记录越界");
            return false;
        }
        const QByteArray payload = workbook.mid(payloadOffset, recordSize);
        if (recordType == 0x002Fu) {
            if (error) *error = QStringLiteral("不支持导入带密码或加密的 XLS 文件");
            return false;
        } else if (recordType == 0x0085u && recordSize >= 8) {
            SheetDescriptor sheet;
            sheet.offset = read32(payload, 0);
            sheet.type = quint8(payload.at(5));
            sheet.name = shortUnicodeString(payload, 6);
            sheets.append(sheet);
        } else if (recordType == 0x00FCu) {
            QVector<QByteArray> segments;
            segments.append(payload);
            int next = payloadOffset + recordSize;
            while (next + 4 <= workbook.size() && read16(workbook, next) == 0x003Cu) {
                const int continueSize = read16(workbook, next + 2);
                if (next + 4 + continueSize > workbook.size()) {
                    if (error) *error = QStringLiteral("XLS 的共享字符串续接记录越界");
                    return false;
                }
                segments.append(workbook.mid(next + 4, continueSize));
                next += 4 + continueSize;
            }
            sharedStrings = parseSharedStrings(segments, error);
            if (error && !error->isEmpty()) return false;
            cursor = next;
            continue;
        } else if (recordType == 0x041Eu && recordSize >= 5) {
            customFormats.insert(read16(payload, 0), unicodeString(payload, 2));
        } else if (recordType == 0x00E0u && recordSize >= 4) {
            xfFormats.append(read16(payload, 2));
        } else if (recordType == 0x0022u && recordSize >= 2) {
            date1904 = read16(payload, 0) != 0;
        }
        if (recordType == 0x000Au) break;
        cursor = payloadOffset + recordSize;
    }

    if (sheets.isEmpty()) {
        if (error) *error = QStringLiteral("XLS 中没有工作表");
        return false;
    }
    SheetDescriptor selected;
    bool selectedFound = false;
    for (const SheetDescriptor &sheet : sheets) {
        if (sheet.type != 0) continue;
        if (!selectedFound) {
            selected = sheet;
            selectedFound = true;
        }
        if (sheet.name == QStringLiteral("人员列表")) {
            selected = sheet;
            break;
        }
    }
    if (!selectedFound || selected.offset + 4 > quint32(workbook.size())) {
        if (error) *error = QStringLiteral("XLS 中没有有效的人员工作表");
        return false;
    }

    int pendingFormulaRow = -1;
    int pendingFormulaColumn = -1;
    cursor = int(selected.offset);
    while (cursor + 4 <= workbook.size()) {
        const quint16 recordType = read16(workbook, cursor);
        const int recordSize = read16(workbook, cursor + 2);
        const int payloadOffset = cursor + 4;
        if (payloadOffset + recordSize > workbook.size()) {
            if (error) *error = QStringLiteral("XLS 的工作表记录越界");
            return false;
        }
        const QByteArray payload = workbook.mid(payloadOffset, recordSize);
        if (recordType == 0x000Au) break;
        if (recordType == 0x00FDu && recordSize >= 10) {
            const quint32 index = read32(payload, 6);
            setCell(rows, read16(payload, 0), read16(payload, 2),
                    index < quint32(sharedStrings.size()) ? sharedStrings.at(int(index)) : QString());
        } else if (recordType == 0x0204u && recordSize >= 9) {
            setCell(rows, read16(payload, 0), read16(payload, 2), unicodeString(payload, 6));
        } else if (recordType == 0x0203u && recordSize >= 14) {
            setCell(rows, read16(payload, 0), read16(payload, 2),
                    formatNumber(readDouble(payload, 6), read16(payload, 4),
                                 xfFormats, customFormats, date1904));
        } else if (recordType == 0x027Eu && recordSize >= 10) {
            setCell(rows, read16(payload, 0), read16(payload, 2),
                    formatNumber(rkValue(read32(payload, 6)), read16(payload, 4),
                                 xfFormats, customFormats, date1904));
        } else if (recordType == 0x00BDu && recordSize >= 12) {
            const int row = read16(payload, 0);
            const int firstColumn = read16(payload, 2);
            const int lastColumn = read16(payload, recordSize - 2);
            const int count = lastColumn - firstColumn + 1;
            for (int i = 0; i < count && 4 + i * 6 + 6 <= recordSize; ++i) {
                const int itemOffset = 4 + i * 6;
                setCell(rows, row, firstColumn + i,
                        formatNumber(rkValue(read32(payload, itemOffset + 2)),
                                     read16(payload, itemOffset), xfFormats,
                                     customFormats, date1904));
            }
        } else if (recordType == 0x0205u && recordSize >= 8) {
            const bool isError = quint8(payload.at(7)) != 0;
            setCell(rows, read16(payload, 0), read16(payload, 2),
                    isError ? QString() : (quint8(payload.at(6)) ? QStringLiteral("true")
                                                                  : QStringLiteral("false")));
        } else if (recordType == 0x0006u && recordSize >= 14) {
            const int row = read16(payload, 0);
            const int column = read16(payload, 2);
            // FormulaValue 的最后两个字节为 0xFFFF 时，前面的字节描述非数字结果。
            if (read16(payload, 12) == 0xFFFFu) {
                const quint8 resultType = quint8(payload.at(6));
                if (resultType == 0) {
                    pendingFormulaRow = row;
                    pendingFormulaColumn = column;
                } else if (resultType == 1) {
                    setCell(rows, row, column,
                            quint8(payload.at(8)) ? QStringLiteral("true") : QStringLiteral("false"));
                } else if (resultType == 3) {
                    setCell(rows, row, column, QString());
                }
            } else {
                setCell(rows, row, column,
                        formatNumber(readDouble(payload, 6), read16(payload, 4),
                                     xfFormats, customFormats, date1904));
            }
        } else if (recordType == 0x0207u && pendingFormulaRow >= 0) {
            setCell(rows, pendingFormulaRow, pendingFormulaColumn, unicodeString(payload, 0));
            pendingFormulaRow = -1;
            pendingFormulaColumn = -1;
        }
        cursor = payloadOffset + recordSize;
    }
    return !rows->isEmpty();
}

} // namespace

/** @brief 从 OLE Workbook 流解析 BIFF8 行，并复用 XLSX 的统一人员映射。 */
PersonXlsxParseResult PersonXlsParser::parse(const QString &filePath)
{
    PersonXlsxParseResult result;
    CompoundFile compound;
    if (!compound.open(filePath, &result.error)) return result;
    const QByteArray workbook = compound.workbookStream(&result.error);
    if (workbook.isEmpty()) return result;
    QVector<QStringList> rows;
    if (!parseWorkbook(workbook, &rows, &result.error)) {
        if (result.error.isEmpty()) result.error = QStringLiteral("XLS 中没有人员数据");
        return result;
    }
    return PersonXlsxParser::parseRows(rows, QStringLiteral("XLS"));
}
