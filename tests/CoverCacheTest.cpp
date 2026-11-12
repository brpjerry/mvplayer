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

    // A picture put in the folder after the album was marked as having none
    // (copied in just after its tracks, which keep their old times): the
    // folder changed, so the cover is looked for again and found.
    void imageAddedLater()
    {
        makeTrack(QStringLiteral("D/01.flac"));
        QCOMPARE(CoverCache::make(covers(), QStringLiteral("d"), p(QStringLiteral("D/01.flac"))), QString());
        CoverCache cache;
        cache.setDir(covers());
        QCOMPARE(cache.lookup(QStringLiteral("d"), p(QStringLiteral("D/01.flac")), false), QString());

        QTest::qSleep(1100); // past the folder time's resolution on any file system
        QImage img(400, 400, QImage::Format_RGB32);
        img.fill(Qt::blue);
        QVERIFY(img.save(p(QStringLiteral("D/Cover.jpg"))));
        QSignalSpy ready(&cache, &CoverCache::coverReady);
        QCOMPARE(cache.lookup(QStringLiteral("d"), p(QStringLiteral("D/01.flac")), true), QString());
        QVERIFY(ready.wait(5000));
        const QString out = CoverCache::fileFor(covers(), QStringLiteral("d"));
        QCOMPARE(ready.first().at(1).toString(), out);
        QVERIFY(!QFile::exists(out + QStringLiteral(".none")));
        QVERIFY(qBlue(QImage(out).pixel(10, 10)) > 240);
        // Made now, it holds: not made again on the next look.
        QCOMPARE(cache.lookup(QStringLiteral("d"), p(QStringLiteral("D/01.flac")), false), out);
    }

    // A made cover whose picture is replaced by another file, then taken
    // away, the track untouched: made again from the new one, then marked
    // as having none.
    void imageReplacedThenRemoved()
    {
        makeTrack(QStringLiteral("E/01.flac"));
        QImage red(400, 400, QImage::Format_RGB32);
        red.fill(Qt::red);
        QVERIFY(red.save(p(QStringLiteral("E/cover.jpg"))));
        const QString out = CoverCache::make(covers(), QStringLiteral("e2"), p(QStringLiteral("E/01.flac")));
        QVERIFY(qRed(QImage(out).pixel(10, 10)) > 240);
        CoverCache cache;
        cache.setDir(covers());
        QCOMPARE(cache.lookup(QStringLiteral("e2"), p(QStringLiteral("E/01.flac")), false), out);

        QTest::qSleep(1100);
        QVERIFY(QFile::remove(p(QStringLiteral("E/cover.jpg"))));
        QImage blue(400, 400, QImage::Format_RGB32);
        blue.fill(Qt::blue);
        QVERIFY(blue.save(p(QStringLiteral("E/front.png"))));
        QCOMPARE(cache.lookup(QStringLiteral("e2"), p(QStringLiteral("E/01.flac")), false), QString());
        QSignalSpy ready(&cache, &CoverCache::coverReady);
        QCOMPARE(cache.lookup(QStringLiteral("e2"), p(QStringLiteral("E/01.flac")), true), QString());
        QVERIFY(ready.wait(5000));
        QVERIFY(qBlue(QImage(out).pixel(10, 10)) > 240);

        QTest::qSleep(1100);
        QVERIFY(QFile::remove(p(QStringLiteral("E/front.png"))));
        QCOMPARE(cache.lookup(QStringLiteral("e2"), p(QStringLiteral("E/01.flac")), true), QString());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(out + QStringLiteral(".none")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(out), 5000); // the marker is written first, the old cover removed after
        QCOMPARE(cache.lookup(QStringLiteral("e2"), p(QStringLiteral("E/01.flac")), false), QString());
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
