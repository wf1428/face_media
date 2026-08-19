/****************************************************************************
** Meta object code from reading C++ file 'FaceInferenceWorker.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/modules/facegate/FaceGateQt/core/FaceInferenceWorker.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QVector>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'FaceInferenceWorker.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_FaceInferenceWorker_t {
    QByteArrayData data[45];
    char stringdata0[596];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FaceInferenceWorker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FaceInferenceWorker_t qt_meta_stringdata_FaceInferenceWorker = {
    {
QT_MOC_LITERAL(0, 0, 19), // "FaceInferenceWorker"
QT_MOC_LITERAL(1, 20, 12), // "engineStatus"
QT_MOC_LITERAL(2, 33, 0), // ""
QT_MOC_LITERAL(3, 34, 7), // "message"
QT_MOC_LITERAL(4, 42, 12), // "stateChanged"
QT_MOC_LITERAL(5, 55, 11), // "VerifyState"
QT_MOC_LITERAL(6, 67, 5), // "state"
QT_MOC_LITERAL(7, 73, 12), // "facesUpdated"
QT_MOC_LITERAL(8, 86, 21), // "QVector<DetectedFace>"
QT_MOC_LITERAL(9, 108, 5), // "faces"
QT_MOC_LITERAL(10, 114, 18), // "verificationPassed"
QT_MOC_LITERAL(11, 133, 9), // "VerifyLog"
QT_MOC_LITERAL(12, 143, 3), // "log"
QT_MOC_LITERAL(13, 147, 18), // "verificationFailed"
QT_MOC_LITERAL(14, 166, 14), // "frameProcessed"
QT_MOC_LITERAL(15, 181, 18), // "adminAnalysisReady"
QT_MOC_LITERAL(16, 200, 11), // "CameraFrame"
QT_MOC_LITERAL(17, 212, 5), // "frame"
QT_MOC_LITERAL(18, 218, 18), // "FaceAnalysisResult"
QT_MOC_LITERAL(19, 237, 8), // "analysis"
QT_MOC_LITERAL(20, 246, 22), // "enrollmentFeatureReady"
QT_MOC_LITERAL(21, 269, 10), // "PersonInfo"
QT_MOC_LITERAL(22, 280, 6), // "person"
QT_MOC_LITERAL(23, 287, 9), // "imagePath"
QT_MOC_LITERAL(24, 297, 2), // "ok"
QT_MOC_LITERAL(25, 300, 9), // "errorText"
QT_MOC_LITERAL(26, 310, 15), // "FaceFeatureData"
QT_MOC_LITERAL(27, 326, 7), // "feature"
QT_MOC_LITERAL(28, 334, 9), // "duplicate"
QT_MOC_LITERAL(29, 344, 10), // "FaceRecord"
QT_MOC_LITERAL(30, 355, 13), // "similarRecord"
QT_MOC_LITERAL(31, 369, 15), // "duplicateCosine"
QT_MOC_LITERAL(32, 385, 24), // "processVerificationFrame"
QT_MOC_LITERAL(33, 410, 17), // "processAdminFrame"
QT_MOC_LITERAL(34, 428, 13), // "updateGallery"
QT_MOC_LITERAL(35, 442, 19), // "QVector<FaceRecord>"
QT_MOC_LITERAL(36, 462, 7), // "records"
QT_MOC_LITERAL(37, 470, 19), // "updateConfiguration"
QT_MOC_LITERAL(38, 490, 9), // "AppConfig"
QT_MOC_LITERAL(39, 500, 6), // "config"
QT_MOC_LITERAL(40, 507, 18), // "setLivenessEnabled"
QT_MOC_LITERAL(41, 526, 7), // "enabled"
QT_MOC_LITERAL(42, 534, 17), // "resetVerification"
QT_MOC_LITERAL(43, 552, 24), // "extractEnrollmentFeature"
QT_MOC_LITERAL(44, 577, 18) // "duplicateThreshold"

    },
    "FaceInferenceWorker\0engineStatus\0\0"
    "message\0stateChanged\0VerifyState\0state\0"
    "facesUpdated\0QVector<DetectedFace>\0"
    "faces\0verificationPassed\0VerifyLog\0"
    "log\0verificationFailed\0frameProcessed\0"
    "adminAnalysisReady\0CameraFrame\0frame\0"
    "FaceAnalysisResult\0analysis\0"
    "enrollmentFeatureReady\0PersonInfo\0"
    "person\0imagePath\0ok\0errorText\0"
    "FaceFeatureData\0feature\0duplicate\0"
    "FaceRecord\0similarRecord\0duplicateCosine\0"
    "processVerificationFrame\0processAdminFrame\0"
    "updateGallery\0QVector<FaceRecord>\0"
    "records\0updateConfiguration\0AppConfig\0"
    "config\0setLivenessEnabled\0enabled\0"
    "resetVerification\0extractEnrollmentFeature\0"
    "duplicateThreshold"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FaceInferenceWorker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      15,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       8,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   89,    2, 0x06 /* Public */,
       4,    2,   92,    2, 0x06 /* Public */,
       7,    1,   97,    2, 0x06 /* Public */,
      10,    1,  100,    2, 0x06 /* Public */,
      13,    1,  103,    2, 0x06 /* Public */,
      14,    0,  106,    2, 0x06 /* Public */,
      15,    2,  107,    2, 0x06 /* Public */,
      20,    8,  112,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      32,    1,  129,    2, 0x0a /* Public */,
      33,    1,  132,    2, 0x0a /* Public */,
      34,    1,  135,    2, 0x0a /* Public */,
      37,    1,  138,    2, 0x0a /* Public */,
      40,    1,  141,    2, 0x0a /* Public */,
      42,    1,  144,    2, 0x0a /* Public */,
      43,    3,  147,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, 0x80000000 | 5, QMetaType::QString,    6,    3,
    QMetaType::Void, 0x80000000 | 8,    9,
    QMetaType::Void, 0x80000000 | 11,   12,
    QMetaType::Void, 0x80000000 | 11,   12,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 16, 0x80000000 | 18,   17,   19,
    QMetaType::Void, 0x80000000 | 21, QMetaType::QString, QMetaType::Bool, QMetaType::QString, 0x80000000 | 26, QMetaType::Bool, 0x80000000 | 29, QMetaType::Float,   22,   23,   24,   25,   27,   28,   30,   31,

 // slots: parameters
    QMetaType::Void, 0x80000000 | 16,   17,
    QMetaType::Void, 0x80000000 | 16,   17,
    QMetaType::Void, 0x80000000 | 35,   36,
    QMetaType::Void, 0x80000000 | 38,   39,
    QMetaType::Void, QMetaType::Bool,   41,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, 0x80000000 | 21, QMetaType::QString, QMetaType::Float,   22,   23,   44,

       0        // eod
};

void FaceInferenceWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FaceInferenceWorker *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->engineStatus((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->stateChanged((*reinterpret_cast< VerifyState(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 2: _t->facesUpdated((*reinterpret_cast< const QVector<DetectedFace>(*)>(_a[1]))); break;
        case 3: _t->verificationPassed((*reinterpret_cast< const VerifyLog(*)>(_a[1]))); break;
        case 4: _t->verificationFailed((*reinterpret_cast< const VerifyLog(*)>(_a[1]))); break;
        case 5: _t->frameProcessed(); break;
        case 6: _t->adminAnalysisReady((*reinterpret_cast< const CameraFrame(*)>(_a[1])),(*reinterpret_cast< const FaceAnalysisResult(*)>(_a[2]))); break;
        case 7: _t->enrollmentFeatureReady((*reinterpret_cast< const PersonInfo(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< bool(*)>(_a[3])),(*reinterpret_cast< const QString(*)>(_a[4])),(*reinterpret_cast< const FaceFeatureData(*)>(_a[5])),(*reinterpret_cast< bool(*)>(_a[6])),(*reinterpret_cast< const FaceRecord(*)>(_a[7])),(*reinterpret_cast< float(*)>(_a[8]))); break;
        case 8: _t->processVerificationFrame((*reinterpret_cast< const CameraFrame(*)>(_a[1]))); break;
        case 9: _t->processAdminFrame((*reinterpret_cast< const CameraFrame(*)>(_a[1]))); break;
        case 10: _t->updateGallery((*reinterpret_cast< const QVector<FaceRecord>(*)>(_a[1]))); break;
        case 11: _t->updateConfiguration((*reinterpret_cast< const AppConfig(*)>(_a[1]))); break;
        case 12: _t->setLivenessEnabled((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 13: _t->resetVerification((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 14: _t->extractEnrollmentFeature((*reinterpret_cast< const PersonInfo(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< float(*)>(_a[3]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 1:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< VerifyState >(); break;
            }
            break;
        case 2:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<DetectedFace> >(); break;
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
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< VerifyLog >(); break;
            }
            break;
        case 6:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< CameraFrame >(); break;
            case 1:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< FaceAnalysisResult >(); break;
            }
            break;
        case 7:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 4:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< FaceFeatureData >(); break;
            case 6:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< FaceRecord >(); break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< PersonInfo >(); break;
            }
            break;
        case 8:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< CameraFrame >(); break;
            }
            break;
        case 9:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< CameraFrame >(); break;
            }
            break;
        case 10:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<FaceRecord> >(); break;
            }
            break;
        case 14:
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
            using _t = void (FaceInferenceWorker::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::engineStatus)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (FaceInferenceWorker::*)(VerifyState , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::stateChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (FaceInferenceWorker::*)(const QVector<DetectedFace> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::facesUpdated)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (FaceInferenceWorker::*)(const VerifyLog & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::verificationPassed)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (FaceInferenceWorker::*)(const VerifyLog & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::verificationFailed)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (FaceInferenceWorker::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::frameProcessed)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (FaceInferenceWorker::*)(const CameraFrame & , const FaceAnalysisResult & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::adminAnalysisReady)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (FaceInferenceWorker::*)(const PersonInfo & , const QString & , bool , const QString & , const FaceFeatureData & , bool , const FaceRecord & , float );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FaceInferenceWorker::enrollmentFeatureReady)) {
                *result = 7;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject FaceInferenceWorker::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_FaceInferenceWorker.data,
    qt_meta_data_FaceInferenceWorker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *FaceInferenceWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FaceInferenceWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_FaceInferenceWorker.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int FaceInferenceWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    }
    return _id;
}

// SIGNAL 0
void FaceInferenceWorker::engineStatus(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void FaceInferenceWorker::stateChanged(VerifyState _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void FaceInferenceWorker::facesUpdated(const QVector<DetectedFace> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void FaceInferenceWorker::verificationPassed(const VerifyLog & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void FaceInferenceWorker::verificationFailed(const VerifyLog & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void FaceInferenceWorker::frameProcessed()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void FaceInferenceWorker::adminAnalysisReady(const CameraFrame & _t1, const FaceAnalysisResult & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void FaceInferenceWorker::enrollmentFeatureReady(const PersonInfo & _t1, const QString & _t2, bool _t3, const QString & _t4, const FaceFeatureData & _t5, bool _t6, const FaceRecord & _t7, float _t8)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)), const_cast<void*>(reinterpret_cast<const void*>(&_t5)), const_cast<void*>(reinterpret_cast<const void*>(&_t6)), const_cast<void*>(reinterpret_cast<const void*>(&_t7)), const_cast<void*>(reinterpret_cast<const void*>(&_t8)) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
