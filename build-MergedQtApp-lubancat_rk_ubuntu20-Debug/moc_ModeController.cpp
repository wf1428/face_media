/****************************************************************************
** Meta object code from reading C++ file 'ModeController.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/shell/ModeController.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ModeController.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_ModeController_t {
    QByteArrayData data[15];
    char stringdata0[227];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ModeController_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ModeController_t qt_meta_stringdata_ModeController = {
    {
QT_MOC_LITERAL(0, 0, 14), // "ModeController"
QT_MOC_LITERAL(1, 15, 12), // "stateChanged"
QT_MOC_LITERAL(2, 28, 0), // ""
QT_MOC_LITERAL(3, 29, 28), // "ModeController::AppModeState"
QT_MOC_LITERAL(4, 58, 5), // "state"
QT_MOC_LITERAL(5, 64, 12), // "switchFailed"
QT_MOC_LITERAL(6, 77, 7), // "message"
QT_MOC_LITERAL(7, 85, 22), // "handlePresenceDetected"
QT_MOC_LITERAL(8, 108, 18), // "handlePresenceLost"
QT_MOC_LITERAL(9, 127, 12), // "AppModeState"
QT_MOC_LITERAL(10, 140, 16), // "MultimediaActive"
QT_MOC_LITERAL(11, 157, 19), // "SwitchingToFaceGate"
QT_MOC_LITERAL(12, 177, 14), // "FaceGateActive"
QT_MOC_LITERAL(13, 192, 21), // "SwitchingToMultimedia"
QT_MOC_LITERAL(14, 214, 12) // "ShuttingDown"

    },
    "ModeController\0stateChanged\0\0"
    "ModeController::AppModeState\0state\0"
    "switchFailed\0message\0handlePresenceDetected\0"
    "handlePresenceLost\0AppModeState\0"
    "MultimediaActive\0SwitchingToFaceGate\0"
    "FaceGateActive\0SwitchingToMultimedia\0"
    "ShuttingDown"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ModeController[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       4,   14, // methods
       0,    0, // properties
       1,   42, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   34,    2, 0x06 /* Public */,
       5,    1,   37,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       7,    0,   40,    2, 0x0a /* Public */,
       8,    0,   41,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, QMetaType::QString,    6,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,

 // enums: name, alias, flags, count, data
       9,    9, 0x2,    5,   47,

 // enum data: key, value
      10, uint(ModeController::AppModeState::MultimediaActive),
      11, uint(ModeController::AppModeState::SwitchingToFaceGate),
      12, uint(ModeController::AppModeState::FaceGateActive),
      13, uint(ModeController::AppModeState::SwitchingToMultimedia),
      14, uint(ModeController::AppModeState::ShuttingDown),

       0        // eod
};

void ModeController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ModeController *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->stateChanged((*reinterpret_cast< ModeController::AppModeState(*)>(_a[1]))); break;
        case 1: _t->switchFailed((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->handlePresenceDetected(); break;
        case 3: _t->handlePresenceLost(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ModeController::*)(ModeController::AppModeState );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ModeController::stateChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ModeController::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ModeController::switchFailed)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject ModeController::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_ModeController.data,
    qt_meta_data_ModeController,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ModeController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ModeController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ModeController.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ModeController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void ModeController::stateChanged(ModeController::AppModeState _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void ModeController::switchFailed(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
