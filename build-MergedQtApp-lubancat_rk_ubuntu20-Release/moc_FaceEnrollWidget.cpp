/****************************************************************************
** Meta object code from reading C++ file 'FaceEnrollWidget.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/modules/facegate/FaceGateQt/ui/FaceEnrollWidget.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'FaceEnrollWidget.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_FaceEnrollWidget_t {
    QByteArrayData data[7];
    char stringdata0[83];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FaceEnrollWidget_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FaceEnrollWidget_t qt_meta_stringdata_FaceEnrollWidget = {
    {
QT_MOC_LITERAL(0, 0, 16), // "FaceEnrollWidget"
QT_MOC_LITERAL(1, 17, 15), // "enrollRequested"
QT_MOC_LITERAL(2, 33, 0), // ""
QT_MOC_LITERAL(3, 34, 10), // "PersonInfo"
QT_MOC_LITERAL(4, 45, 6), // "person"
QT_MOC_LITERAL(5, 52, 5), // "image"
QT_MOC_LITERAL(6, 58, 24) // "cameraRequirementChanged"

    },
    "FaceEnrollWidget\0enrollRequested\0\0"
    "PersonInfo\0person\0image\0"
    "cameraRequirementChanged"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FaceEnrollWidget[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       2,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   24,    2, 0x06 /* Public */,
       6,    0,   29,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3, QMetaType::QImage,    4,    5,
    QMetaType::Void,

       0        // eod
};

void FaceEnrollWidget::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FaceEnrollWidget *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->enrollRequested((*reinterpret_cast< const PersonInfo(*)>(_a[1])),(*reinterpret_cast< const QImage(*)>(_a[2]))); break;
        case 1: _t->cameraRequirementChanged(); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< PersonInfo >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (FaceEnrollWidget::*)(const PersonInfo & , const QImage & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceEnrollWidget::enrollRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (FaceEnrollWidget::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceEnrollWidget::cameraRequirementChanged)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject FaceEnrollWidget::staticMetaObject = { {
    &QWidget::staticMetaObject,
    qt_meta_stringdata_FaceEnrollWidget.data,
    qt_meta_data_FaceEnrollWidget,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *FaceEnrollWidget::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FaceEnrollWidget::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_FaceEnrollWidget.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int FaceEnrollWidget::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    }
    return _id;
}

// SIGNAL 0
void FaceEnrollWidget::enrollRequested(const PersonInfo & _t1, const QImage & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void FaceEnrollWidget::cameraRequirementChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
