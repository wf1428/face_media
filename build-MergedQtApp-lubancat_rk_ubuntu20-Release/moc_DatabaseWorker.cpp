/****************************************************************************
** Meta object code from reading C++ file 'DatabaseWorker.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../MergedQtApp/modules/facegate/FaceGateQt/database/DatabaseWorker.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QVector>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'DatabaseWorker.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_DatabaseWorker_t {
    QByteArrayData data[68];
    char stringdata0[1001];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_DatabaseWorker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_DatabaseWorker_t qt_meta_stringdata_DatabaseWorker = {
    {
QT_MOC_LITERAL(0, 0, 14), // "DatabaseWorker"
QT_MOC_LITERAL(1, 15, 13), // "databaseReady"
QT_MOC_LITERAL(2, 29, 0), // ""
QT_MOC_LITERAL(3, 30, 2), // "ok"
QT_MOC_LITERAL(4, 33, 7), // "message"
QT_MOC_LITERAL(5, 41, 13), // "galleryLoaded"
QT_MOC_LITERAL(6, 55, 19), // "QVector<FaceRecord>"
QT_MOC_LITERAL(7, 75, 7), // "records"
QT_MOC_LITERAL(8, 83, 20), // "networkGalleryLoaded"
QT_MOC_LITERAL(9, 104, 26), // "recognitionGalleriesLoaded"
QT_MOC_LITERAL(10, 131, 12), // "localRecords"
QT_MOC_LITERAL(11, 144, 14), // "networkRecords"
QT_MOC_LITERAL(12, 159, 12), // "peopleLoaded"
QT_MOC_LITERAL(13, 172, 26), // "QVector<PersonAdminRecord>"
QT_MOC_LITERAL(14, 199, 22), // "passedVerifyLogsLoaded"
QT_MOC_LITERAL(15, 222, 28), // "QVector<VerifyLogViewRecord>"
QT_MOC_LITERAL(16, 251, 16), // "verifyLogsLoaded"
QT_MOC_LITERAL(17, 268, 15), // "syncTasksLoaded"
QT_MOC_LITERAL(18, 284, 23), // "QVector<SyncTaskRecord>"
QT_MOC_LITERAL(19, 308, 18), // "systemEventsLoaded"
QT_MOC_LITERAL(20, 327, 23), // "QVector<SystemEventLog>"
QT_MOC_LITERAL(21, 351, 18), // "storageStatsLoaded"
QT_MOC_LITERAL(22, 370, 12), // "StorageStats"
QT_MOC_LITERAL(23, 383, 5), // "stats"
QT_MOC_LITERAL(24, 389, 14), // "enrollFinished"
QT_MOC_LITERAL(25, 404, 20), // "peopleImportFinished"
QT_MOC_LITERAL(26, 425, 13), // "personUpdated"
QT_MOC_LITERAL(27, 439, 13), // "personDeleted"
QT_MOC_LITERAL(28, 453, 16), // "verifyLogWritten"
QT_MOC_LITERAL(29, 470, 23), // "operatorAuditLogWritten"
QT_MOC_LITERAL(30, 494, 21), // "systemEventLogWritten"
QT_MOC_LITERAL(31, 516, 15), // "cleanupFinished"
QT_MOC_LITERAL(32, 532, 4), // "open"
QT_MOC_LITERAL(33, 537, 5), // "close"
QT_MOC_LITERAL(34, 543, 13), // "reloadGallery"
QT_MOC_LITERAL(35, 557, 10), // "loadPeople"
QT_MOC_LITERAL(36, 568, 14), // "includeDeleted"
QT_MOC_LITERAL(37, 583, 20), // "loadPassedVerifyLogs"
QT_MOC_LITERAL(38, 604, 14), // "loadVerifyLogs"
QT_MOC_LITERAL(39, 619, 15), // "VerifyLogFilter"
QT_MOC_LITERAL(40, 635, 6), // "filter"
QT_MOC_LITERAL(41, 642, 13), // "loadSyncTasks"
QT_MOC_LITERAL(42, 656, 16), // "loadSystemEvents"
QT_MOC_LITERAL(43, 673, 16), // "loadStorageStats"
QT_MOC_LITERAL(44, 690, 13), // "addPersonFace"
QT_MOC_LITERAL(45, 704, 10), // "PersonInfo"
QT_MOC_LITERAL(46, 715, 6), // "person"
QT_MOC_LITERAL(47, 722, 15), // "FaceFeatureData"
QT_MOC_LITERAL(48, 738, 7), // "feature"
QT_MOC_LITERAL(49, 746, 9), // "imagePath"
QT_MOC_LITERAL(50, 756, 17), // "importPeopleBasic"
QT_MOC_LITERAL(51, 774, 19), // "QVector<PersonInfo>"
QT_MOC_LITERAL(52, 794, 6), // "people"
QT_MOC_LITERAL(53, 801, 14), // "updatePersonNo"
QT_MOC_LITERAL(54, 816, 8), // "personId"
QT_MOC_LITERAL(55, 825, 11), // "newPersonNo"
QT_MOC_LITERAL(56, 837, 19), // "updatePersonEnabled"
QT_MOC_LITERAL(57, 857, 7), // "enabled"
QT_MOC_LITERAL(58, 865, 12), // "deletePerson"
QT_MOC_LITERAL(59, 878, 12), // "addVerifyLog"
QT_MOC_LITERAL(60, 891, 9), // "VerifyLog"
QT_MOC_LITERAL(61, 901, 3), // "log"
QT_MOC_LITERAL(62, 905, 19), // "addOperatorAuditLog"
QT_MOC_LITERAL(63, 925, 16), // "OperatorAuditLog"
QT_MOC_LITERAL(64, 942, 17), // "addSystemEventLog"
QT_MOC_LITERAL(65, 960, 14), // "SystemEventLog"
QT_MOC_LITERAL(66, 975, 20), // "cleanupOldVerifyLogs"
QT_MOC_LITERAL(67, 996, 4) // "days"

    },
    "DatabaseWorker\0databaseReady\0\0ok\0"
    "message\0galleryLoaded\0QVector<FaceRecord>\0"
    "records\0networkGalleryLoaded\0"
    "recognitionGalleriesLoaded\0localRecords\0"
    "networkRecords\0peopleLoaded\0"
    "QVector<PersonAdminRecord>\0"
    "passedVerifyLogsLoaded\0"
    "QVector<VerifyLogViewRecord>\0"
    "verifyLogsLoaded\0syncTasksLoaded\0"
    "QVector<SyncTaskRecord>\0systemEventsLoaded\0"
    "QVector<SystemEventLog>\0storageStatsLoaded\0"
    "StorageStats\0stats\0enrollFinished\0"
    "peopleImportFinished\0personUpdated\0"
    "personDeleted\0verifyLogWritten\0"
    "operatorAuditLogWritten\0systemEventLogWritten\0"
    "cleanupFinished\0open\0close\0reloadGallery\0"
    "loadPeople\0includeDeleted\0"
    "loadPassedVerifyLogs\0loadVerifyLogs\0"
    "VerifyLogFilter\0filter\0loadSyncTasks\0"
    "loadSystemEvents\0loadStorageStats\0"
    "addPersonFace\0PersonInfo\0person\0"
    "FaceFeatureData\0feature\0imagePath\0"
    "importPeopleBasic\0QVector<PersonInfo>\0"
    "people\0updatePersonNo\0personId\0"
    "newPersonNo\0updatePersonEnabled\0enabled\0"
    "deletePerson\0addVerifyLog\0VerifyLog\0"
    "log\0addOperatorAuditLog\0OperatorAuditLog\0"
    "addSystemEventLog\0SystemEventLog\0"
    "cleanupOldVerifyLogs\0days"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_DatabaseWorker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      37,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      18,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,  199,    2, 0x06 /* Public */,
       5,    1,  204,    2, 0x06 /* Public */,
       8,    1,  207,    2, 0x06 /* Public */,
       9,    2,  210,    2, 0x06 /* Public */,
      12,    1,  215,    2, 0x06 /* Public */,
      14,    1,  218,    2, 0x06 /* Public */,
      16,    1,  221,    2, 0x06 /* Public */,
      17,    1,  224,    2, 0x06 /* Public */,
      19,    1,  227,    2, 0x06 /* Public */,
      21,    1,  230,    2, 0x06 /* Public */,
      24,    2,  233,    2, 0x06 /* Public */,
      25,    2,  238,    2, 0x06 /* Public */,
      26,    2,  243,    2, 0x06 /* Public */,
      27,    2,  248,    2, 0x06 /* Public */,
      28,    2,  253,    2, 0x06 /* Public */,
      29,    2,  258,    2, 0x06 /* Public */,
      30,    2,  263,    2, 0x06 /* Public */,
      31,    2,  268,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      32,    0,  273,    2, 0x0a /* Public */,
      33,    0,  274,    2, 0x0a /* Public */,
      34,    0,  275,    2, 0x0a /* Public */,
      35,    1,  276,    2, 0x0a /* Public */,
      35,    0,  279,    2, 0x2a /* Public | MethodCloned */,
      37,    0,  280,    2, 0x0a /* Public */,
      38,    1,  281,    2, 0x0a /* Public */,
      41,    0,  284,    2, 0x0a /* Public */,
      42,    0,  285,    2, 0x0a /* Public */,
      43,    0,  286,    2, 0x0a /* Public */,
      44,    3,  287,    2, 0x0a /* Public */,
      50,    1,  294,    2, 0x0a /* Public */,
      53,    2,  297,    2, 0x0a /* Public */,
      56,    2,  302,    2, 0x0a /* Public */,
      58,    1,  307,    2, 0x0a /* Public */,
      59,    1,  310,    2, 0x0a /* Public */,
      62,    1,  313,    2, 0x0a /* Public */,
      64,    1,  316,    2, 0x0a /* Public */,
      66,    1,  319,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, 0x80000000 | 6,    7,
    QMetaType::Void, 0x80000000 | 6,    7,
    QMetaType::Void, 0x80000000 | 6, 0x80000000 | 6,   10,   11,
    QMetaType::Void, 0x80000000 | 13,    7,
    QMetaType::Void, 0x80000000 | 15,    7,
    QMetaType::Void, 0x80000000 | 15,    7,
    QMetaType::Void, 0x80000000 | 18,    7,
    QMetaType::Void, 0x80000000 | 20,    7,
    QMetaType::Void, 0x80000000 | 22,   23,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    3,    4,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   36,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 39,   40,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 45, 0x80000000 | 47, QMetaType::QString,   46,   48,   49,
    QMetaType::Void, 0x80000000 | 51,   52,
    QMetaType::Void, QMetaType::LongLong, QMetaType::QString,   54,   55,
    QMetaType::Void, QMetaType::LongLong, QMetaType::Bool,   54,   57,
    QMetaType::Void, QMetaType::LongLong,   54,
    QMetaType::Void, 0x80000000 | 60,   61,
    QMetaType::Void, 0x80000000 | 63,   61,
    QMetaType::Void, 0x80000000 | 65,   61,
    QMetaType::Void, QMetaType::Int,   67,

       0        // eod
};

void DatabaseWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<DatabaseWorker *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->databaseReady((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 1: _t->galleryLoaded((*reinterpret_cast< const QVector<FaceRecord>(*)>(_a[1]))); break;
        case 2: _t->networkGalleryLoaded((*reinterpret_cast< const QVector<FaceRecord>(*)>(_a[1]))); break;
        case 3: _t->recognitionGalleriesLoaded((*reinterpret_cast< const QVector<FaceRecord>(*)>(_a[1])),(*reinterpret_cast< const QVector<FaceRecord>(*)>(_a[2]))); break;
        case 4: _t->peopleLoaded((*reinterpret_cast< const QVector<PersonAdminRecord>(*)>(_a[1]))); break;
        case 5: _t->passedVerifyLogsLoaded((*reinterpret_cast< const QVector<VerifyLogViewRecord>(*)>(_a[1]))); break;
        case 6: _t->verifyLogsLoaded((*reinterpret_cast< const QVector<VerifyLogViewRecord>(*)>(_a[1]))); break;
        case 7: _t->syncTasksLoaded((*reinterpret_cast< const QVector<SyncTaskRecord>(*)>(_a[1]))); break;
        case 8: _t->systemEventsLoaded((*reinterpret_cast< const QVector<SystemEventLog>(*)>(_a[1]))); break;
        case 9: _t->storageStatsLoaded((*reinterpret_cast< const StorageStats(*)>(_a[1]))); break;
        case 10: _t->enrollFinished((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 11: _t->peopleImportFinished((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 12: _t->personUpdated((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 13: _t->personDeleted((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 14: _t->verifyLogWritten((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 15: _t->operatorAuditLogWritten((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 16: _t->systemEventLogWritten((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 17: _t->cleanupFinished((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 18: _t->open(); break;
        case 19: _t->close(); break;
        case 20: _t->reloadGallery(); break;
        case 21: _t->loadPeople((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 22: _t->loadPeople(); break;
        case 23: _t->loadPassedVerifyLogs(); break;
        case 24: _t->loadVerifyLogs((*reinterpret_cast< const VerifyLogFilter(*)>(_a[1]))); break;
        case 25: _t->loadSyncTasks(); break;
        case 26: _t->loadSystemEvents(); break;
        case 27: _t->loadStorageStats(); break;
        case 28: _t->addPersonFace((*reinterpret_cast< const PersonInfo(*)>(_a[1])),(*reinterpret_cast< const FaceFeatureData(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 29: _t->importPeopleBasic((*reinterpret_cast< const QVector<PersonInfo>(*)>(_a[1]))); break;
        case 30: _t->updatePersonNo((*reinterpret_cast< qint64(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 31: _t->updatePersonEnabled((*reinterpret_cast< qint64(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2]))); break;
        case 32: _t->deletePerson((*reinterpret_cast< qint64(*)>(_a[1]))); break;
        case 33: _t->addVerifyLog((*reinterpret_cast< const VerifyLog(*)>(_a[1]))); break;
        case 34: _t->addOperatorAuditLog((*reinterpret_cast< const OperatorAuditLog(*)>(_a[1]))); break;
        case 35: _t->addSystemEventLog((*reinterpret_cast< const SystemEventLog(*)>(_a[1]))); break;
        case 36: _t->cleanupOldVerifyLogs((*reinterpret_cast< int(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 1:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<FaceRecord> >(); break;
            }
            break;
        case 2:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<FaceRecord> >(); break;
            }
            break;
        case 3:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 1:
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<FaceRecord> >(); break;
            }
            break;
        case 4:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<PersonAdminRecord> >(); break;
            }
            break;
        case 5:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<VerifyLogViewRecord> >(); break;
            }
            break;
        case 6:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<VerifyLogViewRecord> >(); break;
            }
            break;
        case 7:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<SyncTaskRecord> >(); break;
            }
            break;
        case 8:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<SystemEventLog> >(); break;
            }
            break;
        case 9:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< StorageStats >(); break;
            }
            break;
        case 24:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< VerifyLogFilter >(); break;
            }
            break;
        case 28:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 1:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< FaceFeatureData >(); break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< PersonInfo >(); break;
            }
            break;
        case 29:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<PersonInfo> >(); break;
            }
            break;
        case 33:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< VerifyLog >(); break;
            }
            break;
        case 34:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< OperatorAuditLog >(); break;
            }
            break;
        case 35:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< SystemEventLog >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::databaseReady)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<FaceRecord> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::galleryLoaded)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<FaceRecord> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::networkGalleryLoaded)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<FaceRecord> & , const QVector<FaceRecord> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::recognitionGalleriesLoaded)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<PersonAdminRecord> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::peopleLoaded)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<VerifyLogViewRecord> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::passedVerifyLogsLoaded)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<VerifyLogViewRecord> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::verifyLogsLoaded)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<SyncTaskRecord> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::syncTasksLoaded)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const QVector<SystemEventLog> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::systemEventsLoaded)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(const StorageStats & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::storageStatsLoaded)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::enrollFinished)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::peopleImportFinished)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::personUpdated)) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::personDeleted)) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::verifyLogWritten)) {
                *result = 14;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::operatorAuditLogWritten)) {
                *result = 15;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::systemEventLogWritten)) {
                *result = 16;
                return;
            }
        }
        {
            using _t = void (DatabaseWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&DatabaseWorker::cleanupFinished)) {
                *result = 17;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject DatabaseWorker::staticMetaObject = { {
    &QObject::staticMetaObject,
    qt_meta_stringdata_DatabaseWorker.data,
    qt_meta_data_DatabaseWorker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *DatabaseWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *DatabaseWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_DatabaseWorker.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int DatabaseWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 37)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 37;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 37)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 37;
    }
    return _id;
}

// SIGNAL 0
void DatabaseWorker::databaseReady(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void DatabaseWorker::galleryLoaded(const QVector<FaceRecord> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void DatabaseWorker::networkGalleryLoaded(const QVector<FaceRecord> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void DatabaseWorker::recognitionGalleriesLoaded(const QVector<FaceRecord> & _t1, const QVector<FaceRecord> & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void DatabaseWorker::peopleLoaded(const QVector<PersonAdminRecord> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void DatabaseWorker::passedVerifyLogsLoaded(const QVector<VerifyLogViewRecord> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void DatabaseWorker::verifyLogsLoaded(const QVector<VerifyLogViewRecord> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void DatabaseWorker::syncTasksLoaded(const QVector<SyncTaskRecord> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void DatabaseWorker::systemEventsLoaded(const QVector<SystemEventLog> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void DatabaseWorker::storageStatsLoaded(const StorageStats & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void DatabaseWorker::enrollFinished(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 10, _a);
}

// SIGNAL 11
void DatabaseWorker::peopleImportFinished(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void DatabaseWorker::personUpdated(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void DatabaseWorker::personDeleted(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}

// SIGNAL 14
void DatabaseWorker::verifyLogWritten(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}

// SIGNAL 15
void DatabaseWorker::operatorAuditLogWritten(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 15, _a);
}

// SIGNAL 16
void DatabaseWorker::systemEventLogWritten(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 16, _a);
}

// SIGNAL 17
void DatabaseWorker::cleanupFinished(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 17, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
