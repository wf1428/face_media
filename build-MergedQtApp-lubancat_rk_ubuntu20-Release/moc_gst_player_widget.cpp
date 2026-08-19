/****************************************************************************
** Meta object code from reading C++ file 'gst_player_widget.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/components/multimedia/gst_player_widget.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'gst_player_widget.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_GstPlayerWidget_t {
    QByteArrayData data[18];
    char stringdata0[204];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_GstPlayerWidget_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_GstPlayerWidget_t qt_meta_stringdata_GstPlayerWidget = {
    {
QT_MOC_LITERAL(0, 0, 15), // "GstPlayerWidget"
QT_MOC_LITERAL(1, 16, 13), // "videoFinished"
QT_MOC_LITERAL(2, 30, 0), // ""
QT_MOC_LITERAL(3, 31, 12), // "errorOccured"
QT_MOC_LITERAL(4, 44, 7), // "message"
QT_MOC_LITERAL(5, 52, 8), // "prepared"
QT_MOC_LITERAL(6, 61, 15), // "positionChanged"
QT_MOC_LITERAL(7, 77, 10), // "positionMs"
QT_MOC_LITERAL(8, 88, 10), // "durationMs"
QT_MOC_LITERAL(9, 99, 17), // "videoFrameArrived"
QT_MOC_LITERAL(10, 117, 5), // "msecs"
QT_MOC_LITERAL(11, 123, 17), // "audioFrameArrived"
QT_MOC_LITERAL(12, 141, 13), // "snapshotReady"
QT_MOC_LITERAL(13, 155, 5), // "image"
QT_MOC_LITERAL(14, 161, 14), // "snapshotFailed"
QT_MOC_LITERAL(15, 176, 6), // "reason"
QT_MOC_LITERAL(16, 183, 7), // "pollBus"
QT_MOC_LITERAL(17, 191, 12) // "pollPosition"

    },
    "GstPlayerWidget\0videoFinished\0\0"
    "errorOccured\0message\0prepared\0"
    "positionChanged\0positionMs\0durationMs\0"
    "videoFrameArrived\0msecs\0audioFrameArrived\0"
    "snapshotReady\0image\0snapshotFailed\0"
    "reason\0pollBus\0pollPosition"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_GstPlayerWidget[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      10,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       8,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   64,    2, 0x06 /* Public */,
       3,    1,   65,    2, 0x06 /* Public */,
       5,    0,   68,    2, 0x06 /* Public */,
       6,    2,   69,    2, 0x06 /* Public */,
       9,    1,   74,    2, 0x06 /* Public */,
      11,    1,   77,    2, 0x06 /* Public */,
      12,    1,   80,    2, 0x06 /* Public */,
      14,    1,   83,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      16,    0,   86,    2, 0x08 /* Private */,
      17,    0,   87,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    4,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,    7,    8,
    QMetaType::Void, QMetaType::LongLong,   10,
    QMetaType::Void, QMetaType::LongLong,   10,
    QMetaType::Void, QMetaType::QImage,   13,
    QMetaType::Void, QMetaType::QString,   15,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void GstPlayerWidget::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<GstPlayerWidget *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->videoFinished(); break;
        case 1: _t->errorOccured((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->prepared(); break;
        case 3: _t->positionChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 4: _t->videoFrameArrived((*reinterpret_cast< qint64(*)>(_a[1]))); break;
        case 5: _t->audioFrameArrived((*reinterpret_cast< qint64(*)>(_a[1]))); break;
        case 6: _t->snapshotReady((*reinterpret_cast< const QImage(*)>(_a[1]))); break;
        case 7: _t->snapshotFailed((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 8: _t->pollBus(); break;
        case 9: _t->pollPosition(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (GstPlayerWidget::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::videoFinished)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (GstPlayerWidget::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::errorOccured)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (GstPlayerWidget::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::prepared)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (GstPlayerWidget::*)(int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::positionChanged)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (GstPlayerWidget::*)(qint64 );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::videoFrameArrived)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (GstPlayerWidget::*)(qint64 );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::audioFrameArrived)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (GstPlayerWidget::*)(const QImage & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::snapshotReady)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (GstPlayerWidget::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&GstPlayerWidget::snapshotFailed)) {
                *result = 7;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject GstPlayerWidget::staticMetaObject = { {
    &QWidget::staticMetaObject,
    qt_meta_stringdata_GstPlayerWidget.data,
    qt_meta_data_GstPlayerWidget,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *GstPlayerWidget::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *GstPlayerWidget::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_GstPlayerWidget.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int GstPlayerWidget::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 10)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 10;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 10)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 10;
    }
    return _id;
}

// SIGNAL 0
void GstPlayerWidget::videoFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void GstPlayerWidget::errorOccured(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void GstPlayerWidget::prepared()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void GstPlayerWidget::positionChanged(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void GstPlayerWidget::videoFrameArrived(qint64 _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void GstPlayerWidget::audioFrameArrived(qint64 _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void GstPlayerWidget::snapshotReady(const QImage & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void GstPlayerWidget::snapshotFailed(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
