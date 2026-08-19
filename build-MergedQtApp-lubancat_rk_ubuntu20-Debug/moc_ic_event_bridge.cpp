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
    QByteArrayData data[20];
    char stringdata0[252];
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
QT_MOC_LITERAL(8, 71, 18), // "rs485FrameReceived"
QT_MOC_LITERAL(9, 90, 5), // "frame"
QT_MOC_LITERAL(10, 96, 18), // "rs485SendRequested"
QT_MOC_LITERAL(11, 115, 9), // "sourceTag"
QT_MOC_LITERAL(12, 125, 26), // "cardReaderCommandRequested"
QT_MOC_LITERAL(13, 152, 7), // "command"
QT_MOC_LITERAL(14, 160, 18), // "toastPassRequested"
QT_MOC_LITERAL(15, 179, 18), // "toastFailRequested"
QT_MOC_LITERAL(16, 198, 6), // "reason"
QT_MOC_LITERAL(17, 205, 17), // "rs485SendFinished"
QT_MOC_LITERAL(18, 223, 2), // "ok"
QT_MOC_LITERAL(19, 226, 25) // "cardReaderCommandFinished"

    },
    "IcEventBridge\0cardPassed\0\0cardId\0floor\0"
    "rawFrame\0onlineQrScanned\0qrCode\0"
    "rs485FrameReceived\0frame\0rs485SendRequested\0"
    "sourceTag\0cardReaderCommandRequested\0"
    "command\0toastPassRequested\0"
    "toastFailRequested\0reason\0rs485SendFinished\0"
    "ok\0cardReaderCommandFinished"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_IcEventBridge[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       9,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       9,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    3,   59,    2, 0x06 /* Public */,
       6,    1,   66,    2, 0x06 /* Public */,
       8,    1,   69,    2, 0x06 /* Public */,
      10,    2,   72,    2, 0x06 /* Public */,
      12,    2,   77,    2, 0x06 /* Public */,
      14,    0,   82,    2, 0x06 /* Public */,
      15,    1,   83,    2, 0x06 /* Public */,
      17,    3,   86,    2, 0x06 /* Public */,
      19,    3,   93,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QByteArray,    3,    4,    5,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QByteArray,    9,
    QMetaType::Void, QMetaType::QByteArray, QMetaType::QString,    9,   11,
    QMetaType::Void, QMetaType::QByteArray, QMetaType::QString,   13,   11,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   16,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool, QMetaType::QString,   11,   18,   16,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool, QMetaType::QString,   11,   18,   16,

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
        case 2: _t->rs485FrameReceived((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 3: _t->rs485SendRequested((*reinterpret_cast< const QByteArray(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 4: _t->cardReaderCommandRequested((*reinterpret_cast< const QByteArray(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 5: _t->toastPassRequested(); break;
        case 6: _t->toastFailRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 7: _t->rs485SendFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 8: _t->cardReaderCommandFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
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
            using _t = void (IcEventBridge::*)(const QByteArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::rs485FrameReceived)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QByteArray & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::rs485SendRequested)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QByteArray & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::cardReaderCommandRequested)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::toastPassRequested)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::toastFailRequested)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::rs485SendFinished)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (IcEventBridge::*)(const QString & , bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcEventBridge::cardReaderCommandFinished)) {
                *result = 8;
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
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 9;
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
void IcEventBridge::rs485FrameReceived(const QByteArray & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void IcEventBridge::rs485SendRequested(const QByteArray & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void IcEventBridge::cardReaderCommandRequested(const QByteArray & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void IcEventBridge::toastPassRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void IcEventBridge::toastFailRequested(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void IcEventBridge::rs485SendFinished(const QString & _t1, bool _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void IcEventBridge::cardReaderCommandFinished(const QString & _t1, bool _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
