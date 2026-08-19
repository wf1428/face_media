/****************************************************************************
** Meta object code from reading C++ file 'icboard_worker.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/common/workers/icboard_worker.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'icboard_worker.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_IcBoardWorker_t {
    QByteArrayData data[11];
    char stringdata0[82];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_IcBoardWorker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_IcBoardWorker_t qt_meta_stringdata_IcBoardWorker = {
    {
QT_MOC_LITERAL(0, 0, 13), // "IcBoardWorker"
QT_MOC_LITERAL(1, 14, 9), // "cardEvent"
QT_MOC_LITERAL(2, 24, 0), // ""
QT_MOC_LITERAL(3, 25, 11), // "IcCardEvent"
QT_MOC_LITERAL(4, 37, 5), // "event"
QT_MOC_LITERAL(5, 43, 8), // "rawFrame"
QT_MOC_LITERAL(6, 52, 5), // "frame"
QT_MOC_LITERAL(7, 58, 5), // "error"
QT_MOC_LITERAL(8, 64, 3), // "err"
QT_MOC_LITERAL(9, 68, 7), // "onFrame"
QT_MOC_LITERAL(10, 76, 5) // "onErr"

    },
    "IcBoardWorker\0cardEvent\0\0IcCardEvent\0"
    "event\0rawFrame\0frame\0error\0err\0onFrame\0"
    "onErr"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_IcBoardWorker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   39,    2, 0x06 /* Public */,
       5,    1,   42,    2, 0x06 /* Public */,
       7,    1,   45,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       9,    1,   48,    2, 0x08 /* Private */,
      10,    1,   51,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, QMetaType::QByteArray,    6,
    QMetaType::Void, QMetaType::QString,    8,

 // slots: parameters
    QMetaType::Void, QMetaType::QByteArray,    6,
    QMetaType::Void, QMetaType::QString,    8,

       0        // eod
};

void IcBoardWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<IcBoardWorker *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->cardEvent((*reinterpret_cast< const IcCardEvent(*)>(_a[1]))); break;
        case 1: _t->rawFrame((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 2: _t->error((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->onFrame((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 4: _t->onErr((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (IcBoardWorker::*)(const IcCardEvent & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcBoardWorker::cardEvent)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (IcBoardWorker::*)(const QByteArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcBoardWorker::rawFrame)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (IcBoardWorker::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&IcBoardWorker::error)) {
                *result = 2;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject IcBoardWorker::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_IcBoardWorker.data,
    qt_meta_data_IcBoardWorker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *IcBoardWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *IcBoardWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_IcBoardWorker.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int IcBoardWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void IcBoardWorker::cardEvent(const IcCardEvent & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void IcBoardWorker::rawFrame(const QByteArray & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void IcBoardWorker::error(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
