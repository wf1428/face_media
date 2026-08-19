/****************************************************************************
** Meta object code from reading C++ file 'ic_mqtt_gateway.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/components/settings/mqtt/ic_mqtt_gateway.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ic_mqtt_gateway.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_IcMqttGateway_t {
    QByteArrayData data[16];
    char stringdata0[161];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_IcMqttGateway_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_IcMqttGateway_t qt_meta_stringdata_IcMqttGateway = {
    {
QT_MOC_LITERAL(0, 0, 13), // "IcMqttGateway"
QT_MOC_LITERAL(1, 14, 13), // "publishPacket"
QT_MOC_LITERAL(2, 28, 0), // ""
QT_MOC_LITERAL(3, 29, 6), // "packet"
QT_MOC_LITERAL(4, 36, 3), // "tag"
QT_MOC_LITERAL(5, 40, 10), // "logMessage"
QT_MOC_LITERAL(6, 51, 3), // "msg"
QT_MOC_LITERAL(7, 55, 12), // "onCardPassed"
QT_MOC_LITERAL(8, 68, 6), // "cardId"
QT_MOC_LITERAL(9, 75, 5), // "floor"
QT_MOC_LITERAL(10, 81, 8), // "rawFrame"
QT_MOC_LITERAL(11, 90, 17), // "onOnlineQrScanned"
QT_MOC_LITERAL(12, 108, 6), // "qrCode"
QT_MOC_LITERAL(13, 115, 20), // "onRs485FrameReceived"
QT_MOC_LITERAL(14, 136, 5), // "frame"
QT_MOC_LITERAL(15, 142, 18) // "onKeepAliveTimeout"

    },
    "IcMqttGateway\0publishPacket\0\0packet\0"
    "tag\0logMessage\0msg\0onCardPassed\0cardId\0"
    "floor\0rawFrame\0onOnlineQrScanned\0"
    "qrCode\0onRs485FrameReceived\0frame\0"
    "onKeepAliveTimeout"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_IcMqttGateway[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   44,    2, 0x06 /* Public */,
       5,    1,   49,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       7,    3,   52,    2, 0x08 /* Private */,
      11,    1,   59,    2, 0x08 /* Private */,
      13,    1,   62,    2, 0x08 /* Private */,
      15,    0,   65,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QJsonObject, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::QString,    6,

 // slots: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QByteArray,    8,    9,   10,
    QMetaType::Void, QMetaType::QString,   12,
    QMetaType::Void, QMetaType::QByteArray,   14,
    QMetaType::Void,

       0        // eod
};

void IcMqttGateway::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<IcMqttGateway *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->publishPacket((*reinterpret_cast< const QJsonObject(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 1: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->onCardPassed((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QByteArray(*)>(_a[3]))); break;
        case 3: _t->onOnlineQrScanned((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->onRs485FrameReceived((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 5: _t->onKeepAliveTimeout(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (IcMqttGateway::*)(const QJsonObject & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcMqttGateway::publishPacket)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (IcMqttGateway::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcMqttGateway::logMessage)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject IcMqttGateway::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_IcMqttGateway.data,
    qt_meta_data_IcMqttGateway,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *IcMqttGateway::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *IcMqttGateway::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_IcMqttGateway.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int IcMqttGateway::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void IcMqttGateway::publishPacket(const QJsonObject & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void IcMqttGateway::logMessage(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
