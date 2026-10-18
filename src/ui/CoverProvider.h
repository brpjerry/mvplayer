#pragma once

#include <QQuickImageProvider>

// image://cover/<percent-encoded track path>: the album's picture at its
// own size (the file beside the track, else the one in its tags), for the
// cover shown large. The grid's covers come from CoverCache, scaled down.
class CoverProvider : public QQuickImageProvider
{
public:
    CoverProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
