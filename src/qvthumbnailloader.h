#ifndef QVTHUMBNAILLOADER_H
#define QVTHUMBNAILLOADER_H

#include <QObject>
#include <QCache>
#include <QImage>
#include <QQueue>
#include <QSet>

class QVThumbnailLoader : public QObject
{
    Q_OBJECT
public:
    struct Request
    {
        QString filePath;
        int pixelSize = 0;
        int generation = 0;
    };

    explicit QVThumbnailLoader(QObject *parent = nullptr);
    ~QVThumbnailLoader() override;

    // Queue a thumbnail for asynchronous loading. Cheap no-op if cached or pending.
    void requestThumbnail(const QString &filePath, int pixelSize);

    // True if a result (even a failed/empty one) is cached for this key.
    bool isCached(const QString &filePath, int pixelSize) const;

    // Returns the cached thumbnail, or a null image if not loaded (or failed to load).
    QImage cachedThumbnail(const QString &filePath, int pixelSize) const;

    // Drop all pending requests and invalidate results that are still in flight
    // (e.g. when the folder changed). The cache is kept.
    void cancelPending();

    // Clear the memory cache (e.g. when the thumbnail size setting changed).
    void clearCache();

    // Decode a thumbnail-sized image. Runs on a worker thread.
    static QImage loadThumbnail(const QString &filePath, int pixelSize);

signals:
    void thumbnailLoaded(const QString &filePath, int pixelSize);

private:
    void dispatchNext();

    // LRU cache, cost is measured in KiB like QVImageCore's image cache
    QCache<QString, QImage> cache;
    QQueue<Request> pendingQueue;
    QSet<QString> pendingPaths;
    int runningCount = 0;
    int generation = 0;

    static const int MAX_RUNNING = 4;
    static const int MAX_QUEUED = 256;
    // 256 MiB worth of decoded thumbnails
    static const int MAX_CACHE_COST_KIB = 262144;
};

#endif // QVTHUMBNAILLOADER_H
