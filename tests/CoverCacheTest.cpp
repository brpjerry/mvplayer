#include "ui/CoverCache.h"

#include <QtTest>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>

// Album covers: an image beside the album's first file, the stand-in marker
// when there is none, the cache keeping up with the file, and (given one)
// the picture embedded in a music file.
class CoverCacheTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString p(const QString &rel) const { return m_tmp.path() + QLatin1Char('/') + rel; }
    QString covers() const { return p(QStringLiteral("covers")); }
    void makeTrack(const QString &rel)
    {
        QVERIFY(QDir().mkpath(QFileInfo(p(rel)).absolutePath()));
        QFile f(p(rel));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not really audio");
        f.close();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
    }

    void folderImage()
    {
        makeTrack(QStringLiteral("A/01.flac"));
        QImage img(800, 600, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(p(QStringLiteral("A/Cover.PNG"))));
        // (Another image in the folder does not get in the way of the named one.)
        QVERIFY(img.save(p(QStringLiteral("A/back.jpg"))));

        const QString out = CoverCache::make(covers(), QStringLiteral("a"), p(QStringLiteral("A/01.flac")));
        QCOMPARE(out, CoverCache::fileFor(covers(), QStringLiteral("a")));
        const QImage got(out);
        QCOMPARE(got.width(), 512);
        QCOMPARE(got.height(), 384);
        QVERIFY(qRed(got.pixel(10, 10)) > 240);

        CoverCache cache;
        cache.setDir(covers());
        QCOMPARE(cache.lookup(QStringLiteral("a"), p(QStringLiteral("A/01.flac")), false), out);
        // The file changed after the cover was made: it is made again.
        QTest::qSleep(1100);
        makeTrack(QStringLiteral("A/01.flac"));
        QCOMPARE(cache.lookup(QStringLiteral("a"), p(QStringLiteral("A/01.flac")), false), QString());
        QSignalSpy ready(&cache, &CoverCache::coverReady);
        QCOMPARE(cache.lookup(QStringLiteral("a"), p(QStringLiteral("A/01.flac")), true), QString());
        QVERIFY(ready.wait(5000));
        QCOMPARE(ready.first().at(1).toString(), out);
        QCOMPARE(cache.lookup(QStringLiteral("a"), p(QStringLiteral("A/01.flac")), false), out);
    }

    void singleImage()
    {
        makeTrack(QStringLiteral("B/01.flac"));
        QImage img(300, 300, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QVERIFY(img.save(p(QStringLiteral("B/whatever.jpg"))));
        const QString out = CoverCache::make(covers(), QStringLiteral("b"), p(QStringLiteral("B/01.flac")));
        QVERIFY(!out.isEmpty());
        QCOMPARE(QImage(out).size(), QSize(300, 300)); // small pictures are kept as they are
    }

    void noImage()
    {
        makeTrack(QStringLiteral("C/01.flac"));
        QCOMPARE(CoverCache::make(covers(), QStringLiteral("c"), p(QStringLiteral("C/01.flac"))), QString());
        QVERIFY(QFile::exists(CoverCache::fileFor(covers(), QStringLiteral("c")) + QStringLiteral(".none")));
        CoverCache cache;
        cache.setDir(covers());
        QSignalSpy ready(&cache, &CoverCache::coverReady);
        QCOMPARE(cache.lookup(QStringLiteral("c"), p(QStringLiteral("C/01.flac")), true), QString());
        QVERIFY(!ready.wait(300)); // marked as having none: not tried again
    }

    // A real music file with a picture in its tags, named by
    // MVPLAYER_COVER_FILE (the smoke test makes one with ffmpeg).
    void embedded()
    {
        const QString file = qEnvironmentVariable("MVPLAYER_COVER_FILE");
        if (file.isEmpty())
            QSKIP("MVPLAYER_COVER_FILE not set");
        const QImage img = CoverCache::extract(file);
        QVERIFY(!img.isNull());
        QVERIFY(img.width() > 0);
        const QString out = CoverCache::make(covers(), QStringLiteral("e"), file);
        QVERIFY(!out.isEmpty());
        QVERIFY(QImage(out).width() <= CoverCache::kMaxSide);
    }
};

QTEST_GUILESS_MAIN(CoverCacheTest)
#include "CoverCacheTest.moc"
