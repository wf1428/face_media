/****************************************************************************
** Meta object code from reading C++ file 'mqttmanager.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/components/settings/mqtt/mqttmanager.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'mqttmanager.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_MqttManager_t {
    QByteArrayData data[34];
    char stringdata0[450];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MqttManager_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MqttManager_t qt_meta_stringdata_MqttManager = {
    {
QT_MOC_LITERAL(0, 0, 11), // "MqttManager"
QT_MOC_LITERAL(1, 12, 10), // "logMessage"
QT_MOC_LITERAL(2, 23, 0), // ""
QT_MOC_LITERAL(3, 24, 3), // "msg"
QT_MOC_LITERAL(4, 28, 15), // "ipcStateChanged"
QT_MOC_LITERAL(5, 44, 9), // "connected"
QT_MOC_LITERAL(6, 54, 20), // "mqttStateTextChanged"
QT_MOC_LITERAL(7, 75, 4), // "text"
QT_MOC_LITERAL(8, 80, 20), // "subTopicsTextChanged"
QT_MOC_LITERAL(9, 101, 20), // "mqttConfigFromStatus"
QT_MOC_LITERAL(10, 122, 7), // "mqttObj"
QT_MOC_LITERAL(11, 130, 17), // "streamUrlReceived"
QT_MOC_LITERAL(12, 148, 3), // "url"
QT_MOC_LITERAL(13, 152, 18), // "mqttVolumeReceived"
QT_MOC_LITERAL(14, 171, 6), // "volume"
QT_MOC_LITERAL(15, 178, 17), // "stopLiveRequested"
QT_MOC_LITERAL(16, 196, 20), // "stopAllPlayRequested"
QT_MOC_LITERAL(17, 217, 15), // "liveStreamReady"
QT_MOC_LITERAL(18, 233, 4), // "kind"
QT_MOC_LITERAL(19, 238, 20), // "ftpDownloadTaskReady"
QT_MOC_LITERAL(20, 259, 20), // "getPlayInfoRequested"
QT_MOC_LITERAL(21, 280, 5), // "topic"
QT_MOC_LITERAL(22, 286, 5), // "reqId"
QT_MOC_LITERAL(23, 292, 8), // "deviceId"
QT_MOC_LITERAL(24, 301, 19), // "mqttConnMarkChanged"
QT_MOC_LITERAL(25, 321, 4), // "mark"
QT_MOC_LITERAL(26, 326, 31), // "resumeRecordedPlaybackRequested"
QT_MOC_LITERAL(27, 358, 18), // "videoControlNotice"
QT_MOC_LITERAL(28, 377, 14), // "onIpcConnected"
QT_MOC_LITERAL(29, 392, 17), // "onIpcDisconnected"
QT_MOC_LITERAL(30, 410, 18), // "onIpcFrameReceived"
QT_MOC_LITERAL(31, 429, 5), // "frame"
QT_MOC_LITERAL(32, 435, 10), // "onIpcError"
QT_MOC_LITERAL(33, 446, 3) // "err"

    },
    "MqttManager\0logMessage\0\0msg\0ipcStateChanged\0"
    "connected\0mqttStateTextChanged\0text\0"
    "subTopicsTextChanged\0mqttConfigFromStatus\0"
    "mqttObj\0streamUrlReceived\0url\0"
    "mqttVolumeReceived\0volume\0stopLiveRequested\0"
    "stopAllPlayRequested\0liveStreamReady\0"
    "kind\0ftpDownloadTaskReady\0"
    "getPlayInfoRequested\0topic\0reqId\0"
    "deviceId\0mqttConnMarkChanged\0mark\0"
    "resumeRecordedPlaybackRequested\0"
    "videoControlNotice\0onIpcConnected\0"
    "onIpcDisconnected\0onIpcFrameReceived\0"
    "frame\0onIpcError\0err"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MqttManager[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      19,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      15,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,  109,    2, 0x06 /* Public */,
       4,    1,  112,    2, 0x06 /* Public */,
       6,    1,  115,    2, 0x06 /* Public */,
       8,    1,  118,    2, 0x06 /* Public */,
       9,    1,  121,    2, 0x06 /* Public */,
      11,    1,  124,    2, 0x06 /* Public */,
      13,    1,  127,    2, 0x06 /* Public */,
      15,    0,  130,    2, 0x06 /* Public */,
      16,    0,  131,    2, 0x06 /* Public */,
      17,    2,  132,    2, 0x06 /* Public */,
      19,    0,  137,    2, 0x06 /* Public */,
      20,    3,  138,    2, 0x06 /* Public */,
      24,    1,  145,    2, 0x06 /* Public */,
      26,    0,  148,    2, 0x06 /* Public */,
      27,    1,  149,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      28,    0,  152,    2, 0x08 /* Private */,
      29,    0,  153,    2, 0x08 /* Private */,
      30,    1,  154,    2, 0x08 /* Private */,
      32,    1,  157,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::Bool,    5,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QJsonObject,   10,
    QMetaType::Void, QMetaType::QString,   12,
    QMetaType::Void, QMetaType::Int,   14,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,   12,   18,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString,   21,   22,   23,
    QMetaType::Void, QMetaType::Int,   25,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    7,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QByteArray,   31,
    QMetaType::Void, QMetaType::QString,   33,

       0        // eod
};

void MqttManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MqttManager *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->ipcStateChanged((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 2: _t->mqttStateTextChanged((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->subTopicsTextChanged((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->mqttConfigFromStatus((*reinterpret_cast< const QJsonObject(*)>(_a[1]))); break;
        case 5: _t->streamUrlReceived((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 6: _t->mqttVolumeReceived((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 7: _t->stopLiveRequested(); break;
        case 8: _t->stopAllPlayRequested(); break;
        case 9: _t->liveStreamReady((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 10: _t->ftpDownloadTaskReady(); break;
        case 11: _t->getPlayInfoRequested((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 12: _t->mqttConnMarkChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 13: _t->resumeRecordedPlaybackRequested(); break;
        case 14: _t->videoControlNotice((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 15: _t->onIpcConnected(); break;
        case 16: _t->onIpcDisconnected(); break;
        case 17: _t->onIpcFrameReceived((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 18: _t->onIpcError((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (MqttManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::logMessage)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::ipcStateChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::mqttStateTextChanged)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::subTopicsTextChanged)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(const QJsonObject & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::mqttConfigFromStatus)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::streamUrlReceived)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::mqttVolumeReceived)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::stopLiveRequested)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::stopAllPlayRequested)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(const QString & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::liveStreamReady)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::ftpDownloadTaskReady)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(const QString & , const QString & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::getPlayInfoRequested)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::mqttConnMarkChanged)) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::resumeRecordedPlaybackRequested)) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (MqttManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MqttManager::videoControlNotice)) {
                *result = 14;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject MqttManager::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_MqttManager.data,
    qt_meta_data_MqttManager,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MqttManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MqttManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MqttManager.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int MqttManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 19)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 19;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 19)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 19;
    }
    return _id;
}

// SIGNAL 0
void MqttManager::logMessage(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void MqttManager::ipcStateChanged(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void MqttManager::mqttStateTextChanged(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void MqttManager::subTopicsTextChanged(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void MqttManager::mqttConfigFromStatus(const QJsonObject & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void MqttManager::streamUrlReceived(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void MqttManager::mqttVolumeReceived(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void MqttManager::stopLiveRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void MqttManager::stopAllPlayRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}

// SIGNAL 9
void MqttManager::liveStreamReady(const QString & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void MqttManager::ftpDownloadTaskReady()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void MqttManager::getPlayInfoRequested(const QString & _t1, const QString & _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void MqttManager::mqttConnMarkChanged(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void MqttManager::resumeRecordedPlaybackRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 13, nullptr);
}

// SIGNAL 14
void MqttManager::videoControlNotice(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
