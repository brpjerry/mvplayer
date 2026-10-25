#pragma once

#include <QQuickImageProvider>

// image://cover/<percent-encoded track path>: the album's picture at its
// own size (the file beside the track, else the one in its tags), for the
// cover shown large. With "?embedded=<anything>" after the path, only the
// picture in the file's tags (the tag editor's preview; the suffix also
// makes the picture load afresh once the file was written). The grid's
// covers come from CoverCache, scaled down.
class CoverProvider : public QQuickImageProvider
{
public:
    CoverProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
