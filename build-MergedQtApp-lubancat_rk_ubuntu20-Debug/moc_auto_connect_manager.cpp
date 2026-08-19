/****************************************************************************
** Meta object code from reading C++ file 'auto_connect_manager.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/components/settings/auto_connect_manager.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'auto_connect_manager.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_AutoConnectManager_t {
    QByteArrayData data[7];
    char stringdata0[102];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_AutoConnectManager_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_AutoConnectManager_t qt_meta_stringdata_AutoConnectManager = {
    {
QT_MOC_LITERAL(0, 0, 18), // "AutoConnectManager"
QT_MOC_LITERAL(1, 19, 25), // "requestAutoConnectNetwork"
QT_MOC_LITERAL(2, 45, 0), // ""
QT_MOC_LITERAL(3, 46, 21), // "requestAutoPrepareFtp"
QT_MOC_LITERAL(4, 68, 10), // "logMessage"
QT_MOC_LITERAL(5, 79, 4), // "text"
QT_MOC_LITERAL(6, 84, 17) // "autoStartFinished"

    },
    "AutoConnectManager\0requestAutoConnectNetwork\0"
    "\0requestAutoPrepareFtp\0logMessage\0"
    "text\0autoStartFinished"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_AutoConnectManager[] = {

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
       3,    0,   35,    2, 0x06 /* Public */,
       4,    1,   36,    2, 0x06 /* Public */,
       6,    0,   39,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    5,
    QMetaType::Void,

       0        // eod
};

void AutoConnectManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<AutoConnectManager *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->requestAutoConnectNetwork(); break;
        case 1: _t->requestAutoPrepareFtp(); break;
        case 2: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->autoStartFinished(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (AutoConnectManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoConnectManager::requestAutoConnectNetwork)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (AutoConnectManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoConnectManager::requestAutoPrepareFtp)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (AutoConnectManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoConnectManager::logMessage)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (AutoConnectManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&AutoConnectManager::autoStartFinished)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject AutoConnectManager::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_AutoConnectManager.data,
    qt_meta_data_AutoConnectManager,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *AutoConnectManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AutoConnectManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_AutoConnectManager.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int AutoConnectManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void AutoConnectManager::requestAutoConnectNetwork()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void AutoConnectManager::requestAutoPrepareFtp()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void AutoConnectManager::logMessage(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void AutoConnectManager::autoStartFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
