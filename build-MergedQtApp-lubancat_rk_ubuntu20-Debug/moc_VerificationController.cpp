/****************************************************************************
** Meta object code from reading C++ file 'VerificationController.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/modules/facegate/FaceGateQt/core/VerificationController.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QVector>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'VerificationController.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_VerificationController_t {
    QByteArrayData data[19];
    char stringdata0[221];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_VerificationController_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_VerificationController_t qt_meta_stringdata_VerificationController = {
    {
QT_MOC_LITERAL(0, 0, 22), // "VerificationController"
QT_MOC_LITERAL(1, 23, 12), // "stateChanged"
QT_MOC_LITERAL(2, 36, 0), // ""
QT_MOC_LITERAL(3, 37, 11), // "VerifyState"
QT_MOC_LITERAL(4, 49, 5), // "state"
QT_MOC_LITERAL(5, 55, 7), // "message"
QT_MOC_LITERAL(6, 63, 12), // "facesUpdated"
QT_MOC_LITERAL(7, 76, 21), // "QVector<DetectedFace>"
QT_MOC_LITERAL(8, 98, 5), // "faces"
QT_MOC_LITERAL(9, 104, 18), // "verificationPassed"
QT_MOC_LITERAL(10, 123, 9), // "VerifyLog"
QT_MOC_LITERAL(11, 133, 3), // "log"
QT_MOC_LITERAL(12, 137, 18), // "verificationFailed"
QT_MOC_LITERAL(13, 156, 7), // "onFrame"
QT_MOC_LITERAL(14, 164, 11), // "CameraFrame"
QT_MOC_LITERAL(15, 176, 5), // "frame"
QT_MOC_LITERAL(16, 182, 16), // "onLivenessResult"
QT_MOC_LITERAL(17, 199, 14), // "LivenessResult"
QT_MOC_LITERAL(18, 214, 6) // "result"

    },
    "VerificationController\0stateChanged\0"
    "\0VerifyState\0state\0message\0facesUpdated\0"
    "QVector<DetectedFace>\0faces\0"
    "verificationPassed\0VerifyLog\0log\0"
    "verificationFailed\0onFrame\0CameraFrame\0"
    "frame\0onLivenessResult\0LivenessResult\0"
    "result"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_VerificationController[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   44,    2, 0x06 /* Public */,
       6,    1,   49,    2, 0x06 /* Public */,
       9,    1,   52,    2, 0x06 /* Public */,
      12,    1,   55,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      13,    1,   58,    2, 0x0a /* Public */,
      16,    1,   61,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3, QMetaType::QString,    4,    5,
    QMetaType::Void, 0x80000000 | 7,    8,
    QMetaType::Void, 0x80000000 | 10,   11,
    QMetaType::Void, 0x80000000 | 10,   11,

 // slots: parameters
    QMetaType::Void, 0x80000000 | 14,   15,
    QMetaType::Void, 0x80000000 | 17,   18,

       0        // eod
};

void VerificationController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<VerificationController *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->stateChanged((*reinterpret_cast< VerifyState(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 1: _t->facesUpdated((*reinterpret_cast< const QVector<DetectedFace>(*)>(_a[1]))); break;
        case 2: _t->verificationPassed((*reinterpret_cast< const VerifyLog(*)>(_a[1]))); break;
        case 3: _t->verificationFailed((*reinterpret_cast< const VerifyLog(*)>(_a[1]))); break;
        case 4: _t->onFrame((*reinterpret_cast< const CameraFrame(*)>(_a[1]))); break;
        case 5: _t->onLivenessResult((*reinterpret_cast< const LivenessResult(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< VerifyState >(); break;
            }
            break;
        case 1:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<DetectedFace> >(); break;
            }
            break;
        case 2:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< VerifyLog >(); break;
            }
            break;
        case 3:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< VerifyLog >(); break;
            }
            break;
        case 4:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< CameraFrame >(); break;
            }
            break;
        case 5:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< LivenessResult >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (VerificationController::*)(VerifyState , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VerificationController::stateChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (VerificationController::*)(const QVector<DetectedFace> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VerificationController::facesUpdated)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (VerificationController::*)(const VerifyLog & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VerificationController::verificationPassed)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (VerificationController::*)(const VerifyLog & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VerificationController::verificationFailed)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject VerificationController::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_VerificationController.data,
    qt_meta_data_VerificationController,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *VerificationController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *VerificationController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_VerificationController.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int VerificationController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void VerificationController::stateChanged(VerifyState _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void VerificationController::facesUpdated(const QVector<DetectedFace> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void VerificationController::verificationPassed(const VerifyLog & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void VerificationController::verificationFailed(const VerifyLog & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
