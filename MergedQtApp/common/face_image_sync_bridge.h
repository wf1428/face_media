#ifndef FACE_IMAGE_SYNC_BRIDGE_H
#define FACE_IMAGE_SYNC_BRIDGE_H

#include <QJsonArray>
#include <QObject>
#include <QString>

class FaceImageSyncBridge : public QObject
{
    Q_OBJECT

public:
    static FaceImageSyncBridge *instance();

public slots:
    void requestImageSync();
    void requestPersonnelList();
    void requestNetworkFaceGalleryRefresh();
    void reportSyncStatus(bool ok, const QString &message);
    void reportPersonnelList(const QJsonArray &people);
    void requestStoredFaceValidation(const QString &token,
                                     const QString &personId,
                                     const QJsonArray &faces);
    void reportStoredFaceValidation(const QString &token,
                                    const QString &personId,
                                    bool ok,
                                    const QString &message,
                                    const QJsonArray &validatedFaces,
                                    const QJsonArray &failedFaces);

signals:
    void imageSyncRequested();
    void personnelListRequested();
    void networkFaceGalleryRefreshRequested();
    void syncStatusChanged(bool ok, const QString &message);
    void personnelListReady(const QJsonArray &people);
    void storedFaceValidationRequested(const QString &token,
                                       const QString &personId,
                                       const QJsonArray &faces);
    void storedFaceValidationFinished(const QString &token,
                                      const QString &personId,
                                      bool ok,
                                      const QString &message,
                                      const QJsonArray &validatedFaces,
                                      const QJsonArray &failedFaces);

private:
    explicit FaceImageSyncBridge(QObject *parent = nullptr);
};

#endif // FACE_IMAGE_SYNC_BRIDGE_H
