/****************************************************************************
** Meta object code from reading C++ file 'ic_offline.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/ic_board/ic_offline.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ic_offline.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_CardSerial2Reader_t {
    QByteArrayData data[16];
    char stringdata0[195];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_CardSerial2Reader_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_CardSerial2Reader_t qt_meta_stringdata_CardSerial2Reader = {
    {
QT_MOC_LITERAL(0, 0, 17), // "CardSerial2Reader"
QT_MOC_LITERAL(1, 18, 9), // "cardFrame"
QT_MOC_LITERAL(2, 28, 0), // ""
QT_MOC_LITERAL(3, 29, 27), // "CardSerial2Reader::CardType"
QT_MOC_LITERAL(4, 57, 4), // "type"
QT_MOC_LITERAL(5, 62, 8), // "rawFrame"
QT_MOC_LITERAL(6, 71, 12), // "errorOccured"
QT_MOC_LITERAL(7, 84, 3), // "err"
QT_MOC_LITERAL(8, 88, 11), // "onReadyRead"
QT_MOC_LITERAL(9, 100, 11), // "onPortError"
QT_MOC_LITERAL(10, 112, 28), // "QSerialPort::SerialPortError"
QT_MOC_LITERAL(11, 141, 16), // "onWait115Timeout"
QT_MOC_LITERAL(12, 158, 8), // "CardType"
QT_MOC_LITERAL(13, 167, 7), // "Unknown"
QT_MOC_LITERAL(14, 175, 10), // "Visitor103"
QT_MOC_LITERAL(15, 186, 8) // "Multi115"

    },
    "CardSerial2Reader\0cardFrame\0\0"
    "CardSerial2Reader::CardType\0type\0"
    "rawFrame\0errorOccured\0err\0onReadyRead\0"
    "onPortError\0QSerialPort::SerialPortError\0"
    "onWait115Timeout\0CardType\0Unknown\0"
    "Visitor103\0Multi115"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_CardSerial2Reader[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       1,   52, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   39,    2, 0x06 /* Public */,
       6,    1,   44,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       8,    0,   47,    2, 0x08 /* Private */,
       9,    1,   48,    2, 0x08 /* Private */,
      11,    0,   51,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3, QMetaType::QByteArray,    4,    5,
    QMetaType::Void, QMetaType::QString,    7,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 10,    7,
    QMetaType::Void,

 // enums: name, alias, flags, count, data
      12,   12, 0x2,    3,   57,

 // enum data: key, value
      13, uint(CardSerial2Reader::CardType::Unknown),
      14, uint(CardSerial2Reader::CardType::Visitor103),
      15, uint(CardSerial2Reader::CardType::Multi115),

       0        // eod
};

void CardSerial2Reader::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<CardSerial2Reader *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->cardFrame((*reinterpret_cast< CardSerial2Reader::CardType(*)>(_a[1])),(*reinterpret_cast< QByteArray(*)>(_a[2]))); break;
        case 1: _t->errorOccured((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 2: _t->onReadyRead(); break;
        case 3: _t->onPortError((*reinterpret_cast< QSerialPort::SerialPortError(*)>(_a[1]))); break;
        case 4: _t->onWait115Timeout(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (CardSerial2Reader::*)(CardSerial2Reader::CardType , QByteArray );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CardSerial2Reader::cardFrame)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (CardSerial2Reader::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CardSerial2Reader::errorOccured)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject CardSerial2Reader::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_CardSerial2Reader.data,
    qt_meta_data_CardSerial2Reader,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *CardSerial2Reader::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *CardSerial2Reader::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_CardSerial2Reader.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int CardSerial2Reader::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void CardSerial2Reader::cardFrame(CardSerial2Reader::CardType _t1, QByteArray _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void CardSerial2Reader::errorOccured(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
