#include "ui/CoverProvider.h"

#include "ui/CoverCache.h"

#include <QUrl>

CoverProvider::CoverProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage CoverProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QImage img = CoverCache::extract(QUrl::fromPercentEncoding(id.toUtf8()));
    // No larger than asked for (the width it is drawn at), never upscaled.
    if (!img.isNull() && requestedSize.width() > 0 && img.width() > requestedSize.width())
        img = img.scaledToWidth(requestedSize.width(), Qt::SmoothTransformation);
    if (size)
        *size = img.size();
    return img;
}
