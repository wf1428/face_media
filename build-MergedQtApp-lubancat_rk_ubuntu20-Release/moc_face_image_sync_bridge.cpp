/****************************************************************************
** Meta object code from reading C++ file 'face_image_sync_bridge.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/common/face_image_sync_bridge.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'face_image_sync_bridge.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_FaceImageSyncBridge_t {
    QByteArrayData data[24];
    char stringdata0[423];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FaceImageSyncBridge_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FaceImageSyncBridge_t qt_meta_stringdata_FaceImageSyncBridge = {
    {
QT_MOC_LITERAL(0, 0, 19), // "FaceImageSyncBridge"
QT_MOC_LITERAL(1, 20, 18), // "imageSyncRequested"
QT_MOC_LITERAL(2, 39, 0), // ""
QT_MOC_LITERAL(3, 40, 22), // "personnelListRequested"
QT_MOC_LITERAL(4, 63, 34), // "networkFaceGalleryRefreshRequ..."
QT_MOC_LITERAL(5, 98, 17), // "syncStatusChanged"
QT_MOC_LITERAL(6, 116, 2), // "ok"
QT_MOC_LITERAL(7, 119, 7), // "message"
QT_MOC_LITERAL(8, 127, 18), // "personnelListReady"
QT_MOC_LITERAL(9, 146, 6), // "people"
QT_MOC_LITERAL(10, 153, 29), // "storedFaceValidationRequested"
QT_MOC_LITERAL(11, 183, 5), // "token"
QT_MOC_LITERAL(12, 189, 8), // "personId"
QT_MOC_LITERAL(13, 198, 5), // "faces"
QT_MOC_LITERAL(14, 204, 28), // "storedFaceValidationFinished"
QT_MOC_LITERAL(15, 233, 14), // "validatedFaces"
QT_MOC_LITERAL(16, 248, 11), // "failedFaces"
QT_MOC_LITERAL(17, 260, 16), // "requestImageSync"
QT_MOC_LITERAL(18, 277, 20), // "requestPersonnelList"
QT_MOC_LITERAL(19, 298, 32), // "requestNetworkFaceGalleryRefresh"
QT_MOC_LITERAL(20, 331, 16), // "reportSyncStatus"
QT_MOC_LITERAL(21, 348, 19), // "reportPersonnelList"
QT_MOC_LITERAL(22, 368, 27), // "requestStoredFaceValidation"
QT_MOC_LITERAL(23, 396, 26) // "reportStoredFaceValidation"

    },
    "FaceImageSyncBridge\0imageSyncRequested\0"
    "\0personnelListRequested\0"
    "networkFaceGalleryRefreshRequested\0"
    "syncStatusChanged\0ok\0message\0"
    "personnelListReady\0people\0"
    "storedFaceValidationRequested\0token\0"
    "personId\0faces\0storedFaceValidationFinished\0"
    "validatedFaces\0failedFaces\0requestImageSync\0"
    "requestPersonnelList\0"
    "requestNetworkFaceGalleryRefresh\0"
    "reportSyncStatus\0reportPersonnelList\0"
    "requestStoredFaceValidation\0"
    "reportStoredFaceValidation"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FaceImageSyncBridge[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      14,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       7,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   84,    2, 0x06 /* Public */,
       3,    0,   85,    2, 0x06 /* Public */,
       4,    0,   86,    2, 0x06 /* Public */,
       5,    2,   87,    2, 0x06 /* Public */,
       8,    1,   92,    2, 0x06 /* Public */,
      10,    3,   95,    2, 0x06 /* Public */,
      14,    6,  102,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      17,    0,  115,    2, 0x0a /* Public */,
      18,    0,  116,    2, 0x0a /* Public */,
      19,    0,  117,    2, 0x0a /* Public */,
      20,    2,  118,    2, 0x0a /* Public */,
      21,    1,  123,    2, 0x0a /* Public */,
      22,    3,  126,    2, 0x0a /* Public */,
      23,    6,  133,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    6,    7,
    QMetaType::Void, QMetaType::QJsonArray,    9,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QJsonArray,   11,   12,   13,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::Bool, QMetaType::QString, QMetaType::QJsonArray, QMetaType::QJsonArray,   11,   12,    6,    7,   15,   16,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    6,    7,
    QMetaType::Void, QMetaType::QJsonArray,    9,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QJsonArray,   11,   12,   13,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::Bool, QMetaType::QString, QMetaType::QJsonArray, QMetaType::QJsonArray,   11,   12,    6,    7,   15,   16,

       0        // eod
};

void FaceImageSyncBridge::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FaceImageSyncBridge *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->imageSyncRequested(); break;
        case 1: _t->personnelListRequested(); break;
        case 2: _t->networkFaceGalleryRefreshRequested(); break;
        case 3: _t->syncStatusChanged((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 4: _t->personnelListReady((*reinterpret_cast< const QJsonArray(*)>(_a[1]))); break;
        case 5: _t->storedFaceValidationRequested((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QJsonArray(*)>(_a[3]))); break;
        case 6: _t->storedFaceValidationFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< bool(*)>(_a[3])),(*reinterpret_cast< const QString(*)>(_a[4])),(*reinterpret_cast< const QJsonArray(*)>(_a[5])),(*reinterpret_cast< const QJsonArray(*)>(_a[6]))); break;
        case 7: _t->requestImageSync(); break;
        case 8: _t->requestPersonnelList(); break;
        case 9: _t->requestNetworkFaceGalleryRefresh(); break;
        case 10: _t->reportSyncStatus((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 11: _t->reportPersonnelList((*reinterpret_cast< const QJsonArray(*)>(_a[1]))); break;
        case 12: _t->requestStoredFaceValidation((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QJsonArray(*)>(_a[3]))); break;
        case 13: _t->reportStoredFaceValidation((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< bool(*)>(_a[3])),(*reinterpret_cast< const QString(*)>(_a[4])),(*reinterpret_cast< const QJsonArray(*)>(_a[5])),(*reinterpret_cast< const QJsonArray(*)>(_a[6]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (FaceImageSyncBridge::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceImageSyncBridge::imageSyncRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (FaceImageSyncBridge::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceImageSyncBridge::personnelListRequested)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (FaceImageSyncBridge::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceImageSyncBridge::networkFaceGalleryRefreshRequested)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (FaceImageSyncBridge::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceImageSyncBridge::syncStatusChanged)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (FaceImageSyncBridge::*)(const QJsonArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceImageSyncBridge::personnelListReady)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (FaceImageSyncBridge::*)(const QString & , const QString & , const QJsonArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceImageSyncBridge::storedFaceValidationRequested)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (FaceImageSyncBridge::*)(const QString & , const QString & , bool , const QString & , const QJsonArray & , const QJsonArray & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceImageSyncBridge::storedFaceValidationFinished)) {
                *result = 6;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject FaceImageSyncBridge::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_FaceImageSyncBridge.data,
    qt_meta_data_FaceImageSyncBridge,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *FaceImageSyncBridge::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FaceImageSyncBridge::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_FaceImageSyncBridge.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int FaceImageSyncBridge::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 14)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 14;
    }
    return _id;
}

// SIGNAL 0
void FaceImageSyncBridge::imageSyncRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void FaceImageSyncBridge::personnelListRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void FaceImageSyncBridge::networkFaceGalleryRefreshRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void FaceImageSyncBridge::syncStatusChanged(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void FaceImageSyncBridge::personnelListReady(const QJsonArray & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void FaceImageSyncBridge::storedFaceValidationRequested(const QString & _t1, const QString & _t2, const QJsonArray & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void FaceImageSyncBridge::storedFaceValidationFinished(const QString & _t1, const QString & _t2, bool _t3, const QString & _t4, const QJsonArray & _t5, const QJsonArray & _t6)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)), const_cast<void*>(reinterpret_cast<const void*>(&_t5)), const_cast<void*>(reinterpret_cast<const void*>(&_t6)) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
