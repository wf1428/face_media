#include "face_image_sync_bridge.h"

#include <QJsonArray>

#include <QCoreApplication>

FaceImageSyncBridge::FaceImageSyncBridge(QObject *parent)
    : QObject(parent)
{
}

FaceImageSyncBridge *FaceImageSyncBridge::instance()
{
    static FaceImageSyncBridge *bridge =
            new FaceImageSyncBridge(QCoreApplication::instance());
    return bridge;
}

void FaceImageSyncBridge::requestImageSync()
{
    emit imageSyncRequested();
}

void FaceImageSyncBridge::requestPersonnelList()
{
    emit personnelListRequested();
}

void FaceImageSyncBridge::requestNetworkFaceGalleryRefresh()
{
    emit networkFaceGalleryRefreshRequested();
}

void FaceImageSyncBridge::reportSyncStatus(bool ok, const QString &message)
{
    emit syncStatusChanged(ok, message);
}

void FaceImageSyncBridge::reportPersonnelList(const QJsonArray &people)
{
    emit personnelListReady(people);
}

void FaceImageSyncBridge::requestStoredFaceValidation(
        const QString &token,
        const QString &personId,
        const QJsonArray &faces)
{
    emit storedFaceValidationRequested(token, personId, faces);
}

void FaceImageSyncBridge::reportStoredFaceValidation(
        const QString &token,
        const QString &personId,
        bool ok,
        const QString &message,
        const QJsonArray &validatedFaces,
        const QJsonArray &failedFaces)
{
    emit storedFaceValidationFinished(
                token, personId, ok, message, validatedFaces, failedFaces);
}
