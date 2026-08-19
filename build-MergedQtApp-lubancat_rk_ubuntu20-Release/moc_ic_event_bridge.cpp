/****************************************************************************
** Meta object code from reading C++ file 'ic_event_bridge.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/ic_board/ic_event_bridge.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ic_event_bridge.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_IcEventBridge_t {
    QByteArrayData data[36];
    char stringdata0[481];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_IcEventBridge_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_IcEventBridge_t qt_meta_stringdata_IcEventBridge = {
    {
QT_MOC_LITERAL(0, 0, 13), // "IcEventBridge"
QT_MOC_LITERAL(1, 14, 10), // "cardPassed"
QT_MOC_LITERAL(2, 25, 0), // ""
QT_MOC_LITERAL(3, 26, 6), // "cardId"
QT_MOC_LITERAL(4, 33, 5), // "floor"
QT_MOC_LITERAL(5, 39, 8), // "rawFrame"
QT_MOC_LITERAL(6, 48, 15), // "onlineQrScanned"
QT_MOC_LITERAL(7, 64, 6), // "qrCode"
QT_MOC_LITERAL(8, 71, 17), // "onlineV1QrScanned"
QT_MOC_LITERAL(9, 89, 14), // "faceRecognized"
QT_MOC_LITERAL(10, 104, 8), // "personId"
QT_MOC_LITERAL(11, 113, 13), // "localPersonId"
QT_MOC_LITERAL(12, 127, 10), // "personName"
QT_MOC_LITERAL(13, 138, 8), // "faceHash"
QT_MOC_LITERAL(14, 147, 23), // "passwordAccessRequested"
QT_MOC_LITERAL(15, 171, 8), // "password"
QT_MOC_LITERAL(16, 180, 22), // "passwordAccessFinished"
QT_MOC_LITERAL(17, 203, 6), // "floors"
QT_MOC_LITERAL(18, 210, 10), // "rs485Frame"
QT_MOC_LITERAL(19, 221, 7), // "success"
QT_MOC_LITERAL(20, 229, 6), // "reason"
QT_MOC_LITERAL(21, 236, 18), // "faceAccessFinished"
QT_MOC_LITERAL(22, 255, 19), // "faceUploadRequested"
QT_MOC_LITERAL(23, 275, 12), // "snapshotPath"
QT_MOC_LITERAL(24, 288, 18), // "cardAccessFinished"
QT_MOC_LITERAL(25, 307, 18), // "rs485FrameReceived"
QT_MOC_LITERAL(26, 326, 5), // "frame"
QT_MOC_LITERAL(27, 332, 18), // "rs485SendRequested"
QT_MOC_LITERAL(28, 351, 9), // "sourceTag"
QT_MOC_LITERAL(29, 361, 26), // "cardReaderCommandRequested"
QT_MOC_LITERAL(30, 388, 7), // "command"
QT_MOC_LITERAL(31, 396, 18), // "toastPassRequested"
QT_MOC_LITERAL(32, 415, 18), // "toastFailRequested"
QT_MOC_LITERAL(33, 434, 17), // "rs485SendFinished"
QT_MOC_LITERAL(34, 452, 2), // "ok"
QT_MOC_LITERAL(35, 455, 25) // "cardReaderCommandFinished"

    },
    "IcEventBridge\0cardPassed\0\0cardId\0floor\0"
    "rawFrame\0onlineQrScanned\0qrCode\0"
    "onlineV1QrScanned\0faceRecognized\0"
    "personId\0localPersonId\0personName\0"
    "faceHash\0passwordAccessRequested\0"
    "password\0passwordAccessFinished\0floors\0"
    "rs485Frame\0success\0reason\0faceAccessFinished\0"
    "faceUploadRequested\0snapshotPath\0"
    "cardAccessFinished\0rs485FrameReceived\0"
    "frame\0rs485SendRequested\0sourceTag\0"
    "cardReaderCommandRequested\0command\0"
    "toastPassRequested\0toastFailRequested\0"
    "rs485SendFinished\0ok\0cardReaderCommandFinished"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_IcEventBridge[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      16,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      16,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    3,   94,    2, 0x06 /* Public */,
       6,    1,  101,    2, 0x06 /* Public */,
       8,    1,  104,    2, 0x06 /* Public */,
       9,    4,  107,    2, 0x06 /* Public */,
      14,    1,  116,    2, 0x06 /* Public */,
      16,    5,  119,    2, 0x06 /* Public */,
      21,    6,  130,    2, 0x06 /* Public */,
      22,    4,  143,    2, 0x06 /* Public */,
      24,    6,  152,    2, 0x06 /* Public */,
      25,    1,  165,    2, 0x06 /* Public */,
      27,    2,  168,    2, 0x06 /* Public */,
      29,    2,  173,    2, 0x06 /* Public */,
      31,    0,  178,    2, 0x06 /* Public */,
      32,    1,  179,    2, 0x06 /* Public */,
      33,    3,  182,    2, 0x06 /* Public */,
      35,    3,  189,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QByteArray,    3,    4,    5,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QString, QMetaType::LongLong, QMetaType::QString, QMetaType::QString,   10,   11,   12,   13,
    QMetaType::Void, QMetaType::QString,   15,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QByteArray, QMetaType::Bool, QMetaType::QString,   10,   17,   18,   19,   20,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::QByteArray, QMetaType::Bool, QMetaType::QString,   10,   13,   17,   18,   19,   20,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::Bool,   10,   13,   23,   19,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::QByteArray, QMetaType::Bool, QMetaType::QString,   10,    3,   17,   18,   19,   20,
    QMetaType::Void, QMetaType::QByteArray,   26,
    QMetaType::Void, QMetaType::QByteArray, QMetaType::QString,   26,   28,
    QMetaType::Void, QMetaType::QByteArray, QMetaType::QString,   30,   28,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool, QMetaType::QString,   28,   34,   20,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool, QMetaType::QString,   28,   34,   20,

       0        // eod
};

void IcEventBridge::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<IcEventBridge *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->cardPassed((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QByteArray(*)>(_a[3]))); break;
        case 1: _t->onlineQrScanned((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->onlineV1QrScanned((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->faceRecognized((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3])),(*reinterpret_cast< const QString(*)>(_a[4]))); break;
        case 4: _t->passwordAccessRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->passwordAccessFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QByteArray(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4])),(*reinterpret_cast< const QString(*)>(_a[5]))); break;
        case 6: _t->faceAccessFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3])),(*reinterpret_cast< const QByteArray(*)>(_a[4])),(*reinterpret_cast< bool(*)>(_a[5])),(*reinterpret_cast< const QString(*)>(_a[6]))); break;
        case 7: _t->faceUploadRequested((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4]))); break;
        case 8: _t->cardAccessFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3])),(*reinterpret_cast< const QByteArray(*)>(_a[4])),(*reinterpret_cast< bool(*)>(_a[5])),(*reinterpret_cast< const QString(*)>(_a[6]))); break;
        case 9: _t->rs485FrameReceived((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 10: _t->rs485SendRequested((*reinterpret_cast< const QByteArray(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 11: _t->cardReaderCommandRequested((*reinterpret_cast< const QByteArray(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 12: _t->toastPassRequested(); break;
        case 13: _t->toastFailRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 14: _t->rs485SendFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 15: _t->cardReaderCommandFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (IcEventBridge::*)(const QString & , const QString & , const QByteArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::cardPassed)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::onlineQrScanned)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::onlineV1QrScanned)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , qint64 , const QString & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::faceRecognized)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::passwordAccessRequested)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , const QString & , const QByteArray & , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::passwordAccessFinished)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , const QString & , const QString & , const QByteArray & , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::faceAccessFinished)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , const QString & , const QString & , bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::faceUploadRequested)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , const QString & , const QString & , const QByteArray & , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::cardAccessFinished)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QByteArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::rs485FrameReceived)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QByteArray & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::rs485SendRequested)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QByteArray & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::cardReaderCommandRequested)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::toastPassRequested)) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::toastFailRequested)) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::rs485SendFinished)) {
                *result = 14;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::cardReaderCommandFinished)) {
                *result = 15;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject IcEventBridge::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_IcEventBridge.data,
    qt_meta_data_IcEventBridge,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *IcEventBridge::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *IcEventBridge::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_IcEventBridge.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int IcEventBridge::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 16)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 16;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 16)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 16;
    }
    return _id;
}

// SIGNAL 0
void IcEventBridge::cardPassed(const QString & _t1, const QString & _t2, const QByteArray & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void IcEventBridge::onlineQrScanned(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void IcEventBridge::onlineV1QrScanned(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void IcEventBridge::faceRecognized(const QString & _t1, qint64 _t2, const QString & _t3, const QString & _t4)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void IcEventBridge::passwordAccessRequested(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void IcEventBridge::passwordAccessFinished(const QString & _t1, const QString & _t2, const QByteArray & _t3, bool _t4, const QString & _t5)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)), const_cast<void*>(reinterpret_cast<const void*>(&_t5)) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void IcEventBridge::faceAccessFinished(const QString & _t1, const QString & _t2, const QString & _t3, const QByteArray & _t4, bool _t5, const QString & _t6)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)), const_cast<void*>(reinterpret_cast<const void*>(&_t5)), const_cast<void*>(reinterpret_cast<const void*>(&_t6)) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void IcEventBridge::faceUploadRequested(const QString & _t1, const QString & _t2, const QString & _t3, bool _t4)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void IcEventBridge::cardAccessFinished(const QString & _t1, const QString & _t2, const QString & _t3, const QByteArray & _t4, bool _t5, const QString & _t6)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)), const_cast<void*>(reinterpret_cast<const void*>(&_t5)), const_cast<void*>(reinterpret_cast<const void*>(&_t6)) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void IcEventBridge::rs485FrameReceived(const QByteArray & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void IcEventBridge::rs485SendRequested(const QByteArray & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 10, _a);
}

// SIGNAL 11
void IcEventBridge::cardReaderCommandRequested(const QByteArray & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void IcEventBridge::toastPassRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 12, nullptr);
}

// SIGNAL 13
void IcEventBridge::toastFailRequested(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}

// SIGNAL 14
void IcEventBridge::rs485SendFinished(const QString & _t1, bool _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}

// SIGNAL 15
void IcEventBridge::cardReaderCommandFinished(const QString & _t1, bool _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 15, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
