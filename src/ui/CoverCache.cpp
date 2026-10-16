#include "ui/CoverCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <taglib/fileref.h>
#include <taglib/tvariant.h>

CoverCache::CoverCache(QObject *parent)
    : QObject(parent)
{
    // Decoding a picture is quick; reading thousands of files is not, and
    // the grid needs the interface more than it needs every cover at once.
    m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 4, 2));
}

CoverCache::~CoverCache()
{
    m_pool.clear();
    m_pool.waitForDone();
}

void CoverCache::setDir(const QString &dir)
{
    m_dir = dir;
}

QString CoverCache::fileFor(const QString &dir, const QString &key)
{
    const QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    return dir + QLatin1Char('/') + QString::fromLatin1(hash) + QStringLiteral(".jpg");
}

QString CoverCache::lookup(const QString &key, const QString &trackPath, bool request)
{
    if (m_dir.isEmpty() || trackPath.isEmpty())
        return {};
    const QString file = fileFor(m_dir, key);
    const QFileInfo source(trackPath);
    const QFileInfo made(file);
    if (made.exists() && made.lastModified() >= source.lastModified())
        return file;
    const QFileInfo none(file + QStringLiteral(".none"));
    if (none.exists() && none.lastModified() >= source.lastModified())
        return {};
    if (request && !m_inFlight.contains(key)) {
        m_inFlight.insert(key);
        const QString dir = m_dir;
        m_pool.start([this, dir, key, trackPath] {
            const QString path = make(dir, key, trackPath);
            QMetaObject::invokeMethod(this, [this, key, path] {
                m_inFlight.remove(key);
                if (!path.isEmpty())
                    emit coverReady(key, path);
            }, Qt::QueuedConnection);
        });
    }
    return {};
}

QString CoverCache::make(const QString &dir, const QString &key, const QString &trackPath)
{
    QDir().mkpath(dir);
    const QString file = fileFor(dir, key);
    const QString none = file + QStringLiteral(".none");
    QImage img = extract(trackPath);
    if (img.isNull()) {
        QFile marker(none);
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
            marker.close();
        QFile::remove(file);
        return {};
    }
    if (qMax(img.width(), img.height()) > kMaxSide)
        img = img.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // A JPEG has no alpha; a transparent corner would come out black.
    if (img.hasAlphaChannel()) {
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                const qreal a = c.alphaF();
                flat.setPixelColor(x, y, QColor::fromRgbF(c.redF() * a + 1 - a, c.greenF() * a + 1 - a, c.blueF() * a + 1 - a));
            }
        }
        img = flat;
    }
    const QString tmp = file + QStringLiteral(".tmp");
    if (!img.save(tmp, "JPEG", 88)) {
        QFile::remove(tmp);
        return {};
    }
    QFile::remove(file);
    if (!QFile::rename(tmp, file)) {
        QFile::remove(tmp);
        return {};
    }
    QFile::remove(none);
    return file;
}

QImage CoverCache::extract(const QString &trackPath)
{
    const QFileInfo fi(trackPath);
    const QDir dir = fi.absoluteDir();
    // An image beside the file, by the names players and rippers use; a
    // folder with one image in it means that one.
    static const QStringList names = {
        QStringLiteral("cover"), QStringLiteral("folder"), QStringLiteral("front"), QStringLiteral("album"),
        QStringLiteral("albumart"), QStringLiteral("albumartsmall"), QStringLiteral("thumb"),
    };
    const QStringList images = dir.entryList({QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"), QStringLiteral("*.webp")},
                                             QDir::Files, QDir::Name);
    QString pick;
    for (const QString &n : names) {
        for (const QString &img : images) {
            if (QFileInfo(img).completeBaseName().compare(n, Qt::CaseInsensitive) == 0) {
                pick = img;
                break;
            }
        }
        if (!pick.isEmpty())
            break;
    }
    if (pick.isEmpty() && images.size() == 1)
        pick = images.first();
    if (!pick.isEmpty()) {
        QImage img(dir.filePath(pick));
        if (!img.isNull())
            return img;
    }

    // The picture in the file's tags, the front cover where it says which.
#ifdef Q_OS_WIN
    const std::wstring name = QDir::toNativeSeparators(trackPath).toStdWString();
    TagLib::FileRef f(name.c_str(), false);
#else
    TagLib::FileRef f(QFile::encodeName(trackPath).constData(), false);
#endif
    if (f.isNull())
        return {};
    const TagLib::List<TagLib::VariantMap> pictures = f.complexProperties("PICTURE");
    const TagLib::VariantMap *best = nullptr;
    for (const TagLib::VariantMap &p : pictures) {
        if (!best)
            best = &p;
        if (p.value("pictureType").toString() == "Front Cover") {
            best = &p;
            break;
        }
    }
    if (!best)
        return {};
    const TagLib::ByteVector data = best->value("data").toByteVector();
    if (data.isEmpty())
        return {};
    return QImage::fromData(reinterpret_cast<const uchar *>(data.data()), int(data.size()));
}
