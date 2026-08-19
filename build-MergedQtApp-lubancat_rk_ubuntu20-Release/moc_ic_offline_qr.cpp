/****************************************************************************
** Meta object code from reading C++ file 'ic_offline_qr.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/ic_board/ic_offline_qr.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ic_offline_qr.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_OfflineQr_t {
    QByteArrayData data[10];
    char stringdata0[111];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_OfflineQr_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_OfflineQr_t qt_meta_stringdata_OfflineQr = {
    {
QT_MOC_LITERAL(0, 0, 9), // "OfflineQr"
QT_MOC_LITERAL(1, 10, 7), // "sigBeep"
QT_MOC_LITERAL(2, 18, 0), // ""
QT_MOC_LITERAL(3, 19, 14), // "sigRelayAction"
QT_MOC_LITERAL(4, 34, 8), // "relayNum"
QT_MOC_LITERAL(5, 43, 10), // "relayTimes"
QT_MOC_LITERAL(6, 54, 19), // "sigRs485SendRequest"
QT_MOC_LITERAL(7, 74, 5), // "frame"
QT_MOC_LITERAL(8, 80, 27), // "sigSetSystemDateTimeRequest"
QT_MOC_LITERAL(9, 108, 2) // "dt"

    },
    "OfflineQr\0sigBeep\0\0sigRelayAction\0"
    "relayNum\0relayTimes\0sigRs485SendRequest\0"
    "frame\0sigSetSystemDateTimeRequest\0dt"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_OfflineQr[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       4,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   34,    2, 0x06 /* Public */,
       3,    2,   35,    2, 0x06 /* Public */,
       6,    1,   40,    2, 0x06 /* Public */,
       8,    1,   43,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,    4,    5,
    QMetaType::Void, QMetaType::QByteArray,    7,
    QMetaType::Void, QMetaType::QDateTime,    9,

       0        // eod
};

void OfflineQr::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<OfflineQr *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->sigBeep(); break;
        case 1: _t->sigRelayAction((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 2: _t->sigRs485SendRequest((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 3: _t->sigSetSystemDateTimeRequest((*reinterpret_cast< const QDateTime(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (OfflineQr::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OfflineQr::sigBeep)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (OfflineQr::*)(int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OfflineQr::sigRelayAction)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (OfflineQr::*)(const QByteArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OfflineQr::sigRs485SendRequest)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (OfflineQr::*)(const QDateTime & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OfflineQr::sigSetSystemDateTimeRequest)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject OfflineQr::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_OfflineQr.data,
    qt_meta_data_OfflineQr,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *OfflineQr::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *OfflineQr::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_OfflineQr.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int OfflineQr::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 4)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void OfflineQr::sigBeep()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void OfflineQr::sigRelayAction(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void OfflineQr::sigRs485SendRequest(const QByteArray & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void OfflineQr::sigSetSystemDateTimeRequest(const QDateTime & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
