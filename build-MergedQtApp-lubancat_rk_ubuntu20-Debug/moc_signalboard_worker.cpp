/****************************************************************************
** Meta object code from reading C++ file 'signalboard_worker.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/common/workers/signalboard_worker.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'signalboard_worker.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_SignalBoardWorker_t {
    QByteArrayData data[15];
    char stringdata0[160];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_SignalBoardWorker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_SignalBoardWorker_t qt_meta_stringdata_SignalBoardWorker = {
    {
QT_MOC_LITERAL(0, 0, 17), // "SignalBoardWorker"
QT_MOC_LITERAL(1, 18, 13), // "stateReceived"
QT_MOC_LITERAL(2, 32, 0), // ""
QT_MOC_LITERAL(3, 33, 16), // "SignalBoardState"
QT_MOC_LITERAL(4, 50, 5), // "state"
QT_MOC_LITERAL(5, 56, 13), // "onlineChanged"
QT_MOC_LITERAL(6, 70, 6), // "online"
QT_MOC_LITERAL(7, 77, 6), // "reason"
QT_MOC_LITERAL(8, 84, 5), // "start"
QT_MOC_LITERAL(9, 90, 4), // "stop"
QT_MOC_LITERAL(10, 95, 11), // "onReadyRead"
QT_MOC_LITERAL(11, 107, 7), // "onError"
QT_MOC_LITERAL(12, 115, 28), // "QSerialPort::SerialPortError"
QT_MOC_LITERAL(13, 144, 1), // "e"
QT_MOC_LITERAL(14, 146, 13) // "onOnlineCheck"

    },
    "SignalBoardWorker\0stateReceived\0\0"
    "SignalBoardState\0state\0onlineChanged\0"
    "online\0reason\0start\0stop\0onReadyRead\0"
    "onError\0QSerialPort::SerialPortError\0"
    "e\0onOnlineCheck"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_SignalBoardWorker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       7,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   49,    2, 0x06 /* Public */,
       5,    2,   52,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       8,    0,   57,    2, 0x0a /* Public */,
       9,    0,   58,    2, 0x0a /* Public */,
      10,    0,   59,    2, 0x08 /* Private */,
      11,    1,   60,    2, 0x08 /* Private */,
      14,    0,   63,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    6,    7,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 12,   13,
    QMetaType::Void,

       0        // eod
};

void SignalBoardWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<SignalBoardWorker *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->stateReceived((*reinterpret_cast< SignalBoardState(*)>(_a[1]))); break;
        case 1: _t->onlineChanged((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< QString(*)>(_a[2]))); break;
        case 2: _t->start(); break;
        case 3: _t->stop(); break;
        case 4: _t->onReadyRead(); break;
        case 5: _t->onError((*reinterpret_cast< QSerialPort::SerialPortError(*)>(_a[1]))); break;
        case 6: _t->onOnlineCheck(); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< SignalBoardState >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (SignalBoardWorker::*)(SignalBoardState );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&SignalBoardWorker::stateReceived)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (SignalBoardWorker::*)(bool , QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&SignalBoardWorker::onlineChanged)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject SignalBoardWorker::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_SignalBoardWorker.data,
    qt_meta_data_SignalBoardWorker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *SignalBoardWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SignalBoardWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_SignalBoardWorker.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int SignalBoardWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    }
    return _id;
}

// SIGNAL 0
void SignalBoardWorker::stateReceived(SignalBoardState _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void SignalBoardWorker::onlineChanged(bool _t1, QString _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
