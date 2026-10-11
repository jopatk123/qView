#include "qvthumbnailloader.h"

#include <QImageReader>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QThreadPool>
#include <QThread>

namespace {

// Static pool for thumbnail decoding so it never competes with the main
// image-loading jobs in QtConcurrent's global instance.
QThreadPool *thumbnailThreadPool()
{
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool();
        p->setMaxThreadCount(qMax(2, QThread::idealThreadCount() / 2));
        return p;
    }();
    return pool;
}

QString cacheKey(const QString &filePath, int pixelSize)
{
    return filePath + "\n" + QString::number(pixelSize);
}

} // namespace

QVThumbnailLoader::QVThumbnailLoader(QObject *parent) : QObject(parent)
{
    cache.setMaxCost(MAX_CACHE_COST_KIB);
}

QVThumbnailLoader::~QVThumbnailLoader()
{
    // Drop queued work. Jobs already running only call the static decoder and
    // post their result to this object; destroying it discards those events.
    // The pool is process-wide, so waiting here would freeze the UI on every
    // window close until every window's thumbnails finished.
    generation++;
    pendingQueue.clear();
    pendingPaths.clear();
}

void QVThumbnailLoader::requestThumbnail(const QString &filePath, int pixelSize)
{
    const QString key = cacheKey(filePath, pixelSize);

    // Already loaded (or failed) or already pending
    if (cache.contains(key) || pendingPaths.contains(key))
        return;

    if (runningCount >= MAX_RUNNING) {
        if (pendingQueue.size() >= MAX_QUEUED) {
            // Drop the oldest request during fast scrolling
            const Request dropped = pendingQueue.dequeue();
            pendingPaths.remove(cacheKey(dropped.filePath, dropped.pixelSize));
        }
        pendingQueue.enqueue({ filePath, pixelSize, generation });
        pendingPaths.insert(key);
        return;
    }

    pendingPaths.insert(key);
    runningCount++;

    const int requestGeneration = generation;

    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [watcher, this, filePath, pixelSize, requestGeneration]() {
                watcher->deleteLater();
                runningCount--;
                dispatchNext();

                // Folder changed while this job was running; drop the result
                if (requestGeneration != generation) {
                    pendingPaths.remove(cacheKey(filePath, pixelSize));
                    return;
                }

                const QImage result = watcher->result();

                const QString key = cacheKey(filePath, pixelSize);
                auto *heapImage = new QImage(result);
                const qint64 costBytes = static_cast<qint64>(heapImage->width())
                        * heapImage->height() * heapImage->depth() / 8;
                // A null image is cached too, so broken files aren't retried forever.
                // Null images get the minimum cost of 1 KiB.
                cache.insert(key, heapImage, qMax<qint64>(costBytes / 1024, 1));

                pendingPaths.remove(key);
                emit thumbnailLoaded(filePath, pixelSize);
            });

    watcher->setFuture(QtConcurrent::run(thumbnailThreadPool(), &QVThumbnailLoader::loadThumbnail,
                                         filePath, pixelSize));
}

bool QVThumbnailLoader::isCached(const QString &filePath, int pixelSize) const
{
    return cache.contains(cacheKey(filePath, pixelSize));
}

QImage QVThumbnailLoader::cachedThumbnail(const QString &filePath, int pixelSize) const
{
    const QImage *cached = cache.object(cacheKey(filePath, pixelSize));
    if (cached)
        return *cached;
    return QImage();
}

void QVThumbnailLoader::cancelPending()
{
    generation++;
    pendingQueue.clear();
    pendingPaths.clear();
}

void QVThumbnailLoader::clearCache()
{
    cancelPending();
    cache.clear();
}

void QVThumbnailLoader::dispatchNext()
{
    while (runningCount < MAX_RUNNING && !pendingQueue.isEmpty()) {
        Request request = pendingQueue.dequeue();

        // Requests from before a cancel are stale
        if (request.generation != generation) {
            pendingPaths.remove(cacheKey(request.filePath, request.pixelSize));
            continue;
        }

        pendingPaths.remove(cacheKey(request.filePath, request.pixelSize));
        requestThumbnail(request.filePath, request.pixelSize);
    }
}

QImage QVThumbnailLoader::loadThumbnail(const QString &filePath, int pixelSize)
{
    QImageReader reader;
    reader.setAutoTransform(true); // handle EXIF orientation
    reader.setFileName(filePath);

    QImage result;

    const QSize sourceSize = reader.size();
    if (sourceSize.isValid() && !sourceSize.isEmpty()) {
        // Decode directly at thumbnail size; this is much cheaper than
        // decoding full size for formats like jpeg.
        const QSize target = sourceSize.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio);
        reader.setScaledSize(target);
        result = reader.read();

        // If the reader's size() did not account for the EXIF transformation,
        // the decoded image can come out with swapped dimensions. Retry once
        // with the corrected target size.
        if (!result.isNull() && result.size() != target) {
            const QSize corrected =
                    result.size().scaled(pixelSize, pixelSize, Qt::KeepAspectRatio);
            QImageReader retryReader;
            retryReader.setAutoTransform(true);
            retryReader.setFileName(filePath);
            retryReader.setScaledSize(corrected);
            result = retryReader.read();
        }
    } else {
        // No size hint available (e.g. some vectors); fall back to a full read
        result = reader.read();
    }

    // Final safety net for formats that ignore setScaledSize
    if (!result.isNull()
        && (result.width() > pixelSize * 2 || result.height() > pixelSize * 2)) {
        result = result.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    }

    return result;
}
