/**
 * @file signalboard_decoder.cpp
 * @brief 解析信号板固定帧中的楼层、方向和运行状态。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "signalboard_decoder.h"

/** @return 字符属于十进制 ASCII 数字时返回 true。 */
static inline bool isDigitChar(char c) { return c >= '0' && c <= '9'; }

/** @return 字符属于大写 ASCII 字母时返回 true。 */
static inline bool isUpperLetterChar(char c)
{
    return c >= 'A' && c <= 'Z';
}

/**
 * @brief 识别只占楼层两个字节之一的单字母楼层。
 *
 * 仅接受“字母+空格”或“空格+字母”，避免把数字、分隔符与字母的组合误判为楼层。
 */
static inline bool isSingleLetterFloor(char tens, char ones, QChar *letter)
{
    // 只兼容单独一个英文字母：
    // 1) tens='A'~'Z', ones=' '
    // 2) tens=' ', ones='A'~'Z'
    //
    // 不兼容 /A、0A、A/、A-、A1 这类组合。
    if (isUpperLetterChar(tens) && ones == ' ') {
        if (letter) {
            *letter = QChar(tens);
        }
        return true;
    }

    if (tens == ' ' && isUpperLetterChar(ones)) {
        if (letter) {
            *letter = QChar(ones);
        }
        return true;
    }

    return false;
}

/**
 * @brief 解析信号板的两字节楼层字段。
 *
 * 支持普通两位数字、以 '/' 或空格占位的单数字，以及 'B'/'-' 表示的地下楼层。
 * 无法识别时保持 valid=false、num=0、text="--"。
 */
static void parseFloorFromAscii(char tens, char ones,
                                bool *valid, int *num, QString *text)
{
    *valid = false;
    *num = 0;
    *text = "--";

    /*
     * 单数字楼层兼容：
     * STM32 采集板可能使用 '/' 或 ' ' 表示“十位无效/无十位”，
     * 例如 "/5" 或 " 5" 都应按 5 楼处理。
     *
     * 注意：这里不改变 "05" 的原有含义。
     * "05" 仍然走下面的两位数字逻辑，floorText 仍为 "05"。
     */
    if ((tens == '/' || tens == ' ') && isDigitChar(ones)) {
        int v = ones - '0';
        *valid = true;
        *num = v;
        *text = QString("%1").arg(v);
        return;
    }

    if (tens == '/' || tens == ' ' || ones == ' ' || ones == '-') return;

    // 正常两位数字
    if (isDigitChar(tens) && isDigitChar(ones)) {
        int v = (tens - '0') * 10 + (ones - '0');
        *valid = true;
        *num = v;
        *text = QString("%1%2").arg(tens).arg(ones);
        return;
    }

    // 负层：'-' 或 'B' + 数字
    if ((tens == '-' || tens == 'B') && isDigitChar(ones)) {
        int n = (ones - '0');
        *valid = true;
        *num = -n; // 或者 num=n 另加 flag
        *text = (tens == 'B') ? QString("B%1").arg(n) : QString("-%1").arg(n);
        return;
    }
}


/**
 * @brief 校验帧头、帧尾和长度，并解析方向、模式及楼层。
 * @param frame 信号板完整帧，格式为 STX + 10 字节数据 + ETX。
 * @return 解析状态；格式或楼层无效时 ok 为 false。
 */
SignalBoardState SignalBoardDecoder::decode(const QByteArray& frame)
{
    SignalBoardState s;
    s.raw = frame;

    //必须是 12 字节
    if (frame.size() != 12) return s;
    //判断帧头、帧尾
    if ((unsigned char)frame[0] != 0x02 || (unsigned char)frame[11] != 0x03) return s;

    /*
     * 给 UI 直接使用的原始 ASCII 字段。
     * UI 后面不需要再从 st.raw[1] / st.raw[5] 取。
     */
    s.dirAscii  = static_cast<unsigned char>(frame[1]);
    s.modeAscii = static_cast<unsigned char>(frame[5]);

    //解析数据帧
    // 方向 '0':停; '1':上行; '3':下行
    char d = frame[1];
    if (d == '0') s.dir = RunDirection::Stop;
    else if (d == '1') s.dir = RunDirection::Up;
    else if (d == '3') s.dir = RunDirection::Down;
    else s.dir = RunDirection::Unknown;

    // 模式/状态
    char m = frame[5];
    if (m == '8') s.mode = WorkMode::Normal;             // 正常
    else if (m == '1') s.mode = WorkMode::Full;          // 满员
    else if (m == '2') s.mode = WorkMode::Overload;      // 超载
    else if (m == '3') s.mode = WorkMode::Fire;          // 消防
    else if (m == '4') s.mode = WorkMode::Repair;        // 检修
    else s.mode = WorkMode::Unknown;

    // 楼层
    // parseFloorFromAscii(frame[7], frame[8], &s.floorValid, &s.floorNumber, &s.floorText);

    // 楼层 使用 st.raw[3] / st.raw[4] 为楼层字段，
    const char tens = frame[3];
    const char ones = frame[4];

    parseFloorFromAscii(tens, ones, &s.floorValid, &s.floorNumber, &s.floorText);

    /*
     * 给 UI 直接使用的楼层显示字段。
     */
    QChar letter;
    if (isSingleLetterFloor(tens, ones, &letter)) {
        // 单字母楼层：A、M、G 等
        s.floorLetter = letter;
        s.floorPrefix = QChar();

        s.tenDigit = 0;
        s.oneDigit = 0;

        s.floorTenOrPrefix = 0;
        s.floorOneDigit = 0;

        s.floorText = QString(letter);
    }
    else if ((tens == 'B' || tens == '-') && isDigitChar(ones)) {
        // B1 / -1
        s.floorPrefix = QChar(tens);
        s.floorLetter = QChar();

        s.tenDigit = 0;
        s.oneDigit = ones - '0';

        /*
         * 注意：
         * floorTenOrPrefix 这里保存的是前缀 ASCII：'B' 或 '-'
         * UI 不能直接把它塞进 my_buf[3] 当十位数字用。
         */
        s.floorTenOrPrefix = static_cast<unsigned char>(tens);
        s.floorOneDigit = static_cast<unsigned char>(ones - '0');
    }
    else if (isDigitChar(ones)) {
        // 11、05、/5、 5
        s.floorPrefix = QChar();
        s.floorLetter = QChar();

        s.tenDigit = isDigitChar(tens) ? (tens - '0') : 0;
        s.oneDigit = ones - '0';

        s.floorTenOrPrefix = static_cast<unsigned char>(s.tenDigit);
        s.floorOneDigit = static_cast<unsigned char>(s.oneDigit);
    }
    else {
        // 空楼层 / 无效楼层
        s.floorPrefix = QChar();
        s.floorLetter = QChar();

        s.tenDigit = 0;
        s.oneDigit = 0;

        s.floorTenOrPrefix = 0;
        s.floorOneDigit = 0;
    }

    s.floor = s.floorNumber;

    /*
     * ok 表示这帧能被有效识别为一个可显示楼层。
     * 数字楼层 / B1 / -1 使用 floorValid；
     * 单字母楼层虽然 floorNumber 没意义，但也属于可显示楼层。
     */
    s.ok = s.floorValid || !s.floorLetter.isNull();

    return s;
}
