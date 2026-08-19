/****************************************************************************
** Meta object code from reading C++ file 'mqttipcclient.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/components/settings/mqtt/mqttipcclient.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'mqttipcclient.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_MqttIpcClient_t {
    QByteArrayData data[15];
    char stringdata0[193];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MqttIpcClient_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MqttIpcClient_t qt_meta_stringdata_MqttIpcClient = {
    {
QT_MOC_LITERAL(0, 0, 13), // "MqttIpcClient"
QT_MOC_LITERAL(1, 14, 9), // "connected"
QT_MOC_LITERAL(2, 24, 0), // ""
QT_MOC_LITERAL(3, 25, 12), // "disconnected"
QT_MOC_LITERAL(4, 38, 13), // "frameReceived"
QT_MOC_LITERAL(5, 52, 5), // "frame"
QT_MOC_LITERAL(6, 58, 13), // "errorOccurred"
QT_MOC_LITERAL(7, 72, 3), // "err"
QT_MOC_LITERAL(8, 76, 10), // "logMessage"
QT_MOC_LITERAL(9, 87, 3), // "msg"
QT_MOC_LITERAL(10, 91, 17), // "onSocketConnected"
QT_MOC_LITERAL(11, 109, 20), // "onSocketDisconnected"
QT_MOC_LITERAL(12, 130, 17), // "onSocketReadyRead"
QT_MOC_LITERAL(13, 148, 13), // "onSocketError"
QT_MOC_LITERAL(14, 162, 30) // "QLocalSocket::LocalSocketError"

    },
    "MqttIpcClient\0connected\0\0disconnected\0"
    "frameReceived\0frame\0errorOccurred\0err\0"
    "logMessage\0msg\0onSocketConnected\0"
    "onSocketDisconnected\0onSocketReadyRead\0"
    "onSocketError\0QLocalSocket::LocalSocketError"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MqttIpcClient[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       9,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       5,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   59,    2, 0x06 /* Public */,
       3,    0,   60,    2, 0x06 /* Public */,
       4,    1,   61,    2, 0x06 /* Public */,
       6,    1,   64,    2, 0x06 /* Public */,
       8,    1,   67,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      10,    0,   70,    2, 0x08 /* Private */,
      11,    0,   71,    2, 0x08 /* Private */,
      12,    0,   72,    2, 0x08 /* Private */,
      13,    1,   73,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QByteArray,    5,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QString,    9,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 14,    7,

       0        // eod
};

void MqttIpcClient::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MqttIpcClient *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->connected(); break;
        case 1: _t->disconnected(); break;
        case 2: _t->frameReceived((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 3: _t->errorOccurred((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->onSocketConnected(); break;
        case 6: _t->onSocketDisconnected(); break;
        case 7: _t->onSocketReadyRead(); break;
        case 8: _t->onSocketError((*reinterpret_cast< QLocalSocket::LocalSocketError(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (MqttIpcClient::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttIpcClient::connected)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (MqttIpcClient::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttIpcClient::disconnected)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (MqttIpcClient::*)(const QByteArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttIpcClient::frameReceived)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (MqttIpcClient::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttIpcClient::errorOccurred)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (MqttIpcClient::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttIpcClient::logMessage)) {
                *result = 4;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject MqttIpcClient::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_MqttIpcClient.data,
    qt_meta_data_MqttIpcClient,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MqttIpcClient::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MqttIpcClient::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MqttIpcClient.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int MqttIpcClient::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void MqttIpcClient::connected()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void MqttIpcClient::disconnected()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void MqttIpcClient::frameReceived(const QByteArray & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void MqttIpcClient::errorOccurred(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void MqttIpcClient::logMessage(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
