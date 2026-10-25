#include "ui/CoverProvider.h"

#include "ui/CoverCache.h"

#include <QUrl>

CoverProvider::CoverProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage CoverProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    // A "?" in the path itself arrives percent-encoded; a raw one starts the query.
    const int query = id.indexOf(QLatin1Char('?'));
    const QString path = QUrl::fromPercentEncoding(id.left(query < 0 ? id.size() : query).toUtf8());
    const bool embedded = query >= 0 && id.mid(query + 1).startsWith(QLatin1String("embedded"));
    QImage img = embedded ? CoverCache::extractEmbedded(path) : CoverCache::extract(path);
    // No larger than asked for (the width it is drawn at), never upscaled.
    if (!img.isNull() && requestedSize.width() > 0 && img.width() > requestedSize.width())
        img = img.scaledToWidth(requestedSize.width(), Qt::SmoothTransformation);
    if (size)
        *size = img.size();
    return img;
}
