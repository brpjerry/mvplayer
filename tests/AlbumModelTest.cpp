#include "ui/AlbumModel.h"

#include <QtTest>

// The album grid's model: rows per album, the orders of the sort menu, the
// sidebar facets, the search box and the covers coming in.
class AlbumModelTest : public QObject
{
    Q_OBJECT

    static AlbumInfo album(const QString &artist, const QString &title, int year, int tracks, int videos, qint64 newest,
                           const QString &artists = QString(), const QString &genres = QString())
    {
        AlbumInfo a;
        a.key = Database::albumKey(artist, title);
        a.albumArtist = artist;
        a.album = title;
        a.artists = artists.isEmpty() ? artist : artists;
        a.genres = genres;
        a.year = year;
        a.tracks = tracks;
        a.videos = videos;
        a.lookedUp = videos;
        a.duration = tracks * 200.0;
        a.newest = newest;
        a.firstPath = QStringLiteral("/m/%1/%2/01.flac").arg(artist, title);
        return a;
    }
    static QVector<AlbumInfo> sample()
    {
        return {
            album(QStringLiteral("The Beatles"), QStringLiteral("Abbey Road"), 1969, 17, 3, 300, QString(), QStringLiteral("Rock")),
            album(QStringLiteral("The Beatles"), QStringLiteral("Help!"), 1965, 14, 0, 100, QString(), QStringLiteral("Rock; Pop")),
            album(QStringLiteral("YOASOBI"), QStringLiteral("THE BOOK"), 2021, 12, 12, 200, QStringLiteral("YOASOBI; ikura"), QStringLiteral("J-Pop")),
            album(QStringLiteral("Zed"), QString(), 0, 2, 0, 400),
        };
    }
    static QStringList titles(const AlbumFilterModel &p)
    {
        QStringList out;
        for (int i = 0; i < p.rowCount(); ++i)
            out << p.data(p.index(i, 0), AlbumModel::AlbumRole).toString();
        return out;
    }

private slots:
    void roles()
    {
        AlbumModel model;
        model.reset(sample());
        QCOMPARE(model.rowCount(), 4);
        const QModelIndex i = model.index(0);
        QCOMPARE(model.data(i, AlbumModel::KeyRole).toString(), Database::albumKey(QStringLiteral("The Beatles"), QStringLiteral("Abbey Road")));
        QCOMPARE(model.data(i, AlbumModel::TrackCountRole).toInt(), 17);
        QCOMPARE(model.data(i, AlbumModel::VideoCountRole).toInt(), 3);
        QCOMPARE(model.data(i, AlbumModel::DurationTextRole).toString(), QStringLiteral("56:40"));
        QCOMPARE(model.data(i, AlbumModel::CoverRole).toUrl(), QUrl());
        QCOMPARE(model.rowOfKey(Database::albumKey(QStringLiteral("Zed"), QString())), 3);
        QCOMPARE(model.rowOfKey(QStringLiteral("nope")), -1);
        const QVariantMap m = model.toMap(2);
        QCOMPARE(m.value(QStringLiteral("albumArtist")).toString(), QStringLiteral("YOASOBI"));
        QCOMPARE(m.value(QStringLiteral("artist")).toString(), QStringLiteral("YOASOBI; ikura"));
    }

    void orders()
    {
        AlbumModel model;
        AlbumFilterModel p(&model);
        model.reset(sample());
        // Title, A to Z: an album without a title first.
        QCOMPARE(titles(p), (QStringList{QString(), QStringLiteral("Abbey Road"), QStringLiteral("Help!"), QStringLiteral("THE BOOK")}));
        p.setSortDescending(true);
        QCOMPARE(titles(p), (QStringList{QStringLiteral("THE BOOK"), QStringLiteral("Help!"), QStringLiteral("Abbey Road"), QString()}));
        // Artist: an artist's albums by year.
        p.setSortMode(QStringLiteral("artist"));
        QVERIFY(!p.sortDescending());
        QCOMPARE(titles(p), (QStringList{QStringLiteral("Help!"), QStringLiteral("Abbey Road"), QStringLiteral("THE BOOK"), QString()}));
        // Year: newest first by default, the undated last.
        p.setSortMode(QStringLiteral("year"));
        QVERIFY(p.sortDescending());
        QCOMPARE(titles(p), (QStringList{QStringLiteral("THE BOOK"), QStringLiteral("Abbey Road"), QStringLiteral("Help!"), QString()}));
        p.setSortDescending(false);
        QCOMPARE(titles(p), (QStringList{QString(), QStringLiteral("Help!"), QStringLiteral("Abbey Road"), QStringLiteral("THE BOOK")}));
        // Modified: the album whose files changed last first, by default.
        p.setSortMode(QStringLiteral("modified"));
        QVERIFY(p.sortDescending());
        QCOMPARE(titles(p), (QStringList{QString(), QStringLiteral("Abbey Road"), QStringLiteral("THE BOOK"), QStringLiteral("Help!")}));
        p.setSortDescending(false);
        QCOMPARE(titles(p), (QStringList{QStringLiteral("Help!"), QStringLiteral("THE BOOK"), QStringLiteral("Abbey Road"), QString()}));
        p.setSortMode(QStringLiteral("year"));
        p.setSortDescending(false);
        // Recently added: the newest files first, whatever the sort order.
        p.setFacet(QStringLiteral("recent"));
        QCOMPARE(titles(p), (QStringList{QString(), QStringLiteral("Abbey Road"), QStringLiteral("THE BOOK"), QStringLiteral("Help!")}));
        QCOMPARE(p.rowOfKey(Database::albumKey(QStringLiteral("YOASOBI"), QStringLiteral("THE BOOK"))), 2);
    }

    void facets()
    {
        AlbumModel model;
        AlbumFilterModel p(&model);
        model.reset(sample());
        p.setFacet(QStringLiteral("albumArtist"), QStringLiteral("The Beatles"));
        QCOMPARE(p.count(), 2);
        p.setFacet(QStringLiteral("artist"), QStringLiteral("ikura"));
        QCOMPARE(titles(p), QStringList{QStringLiteral("THE BOOK")});
        p.setFacet(QStringLiteral("genre"), QStringLiteral("Pop"));
        QCOMPARE(titles(p), QStringList{QStringLiteral("Help!")});
        p.setFacet(QStringLiteral("year"), QStringLiteral("1965"));
        QCOMPARE(p.count(), 1);
        p.setFacet(QStringLiteral("album"), QStringLiteral("Abbey Road"));
        QCOMPARE(p.count(), 1);
        p.setFacet(QStringLiteral("all"));
        QCOMPARE(p.count(), 4);
        // An album that is not on show has no row.
        p.setFacet(QStringLiteral("genre"), QStringLiteral("J-Pop"));
        QCOMPARE(p.rowOfKey(Database::albumKey(QStringLiteral("The Beatles"), QStringLiteral("Help!"))), -1);
    }

    void search()
    {
        AlbumModel model;
        AlbumFilterModel p(&model);
        model.reset(sample());
        p.setSearchText(QStringLiteral("book"));
        QCOMPARE(titles(p), QStringList{QStringLiteral("THE BOOK")});
        p.setSearchText(QStringLiteral("beat 1965"));
        QCOMPARE(titles(p), QStringList{QStringLiteral("Help!")});
        p.setSearchText(QStringLiteral("ikura"));
        QCOMPARE(p.count(), 1);
        p.setSearchText(QString());
        QCOMPARE(p.count(), 4);
    }

    void covers()
    {
        AlbumModel model;
        model.reset(sample());
        const QString key = Database::albumKey(QStringLiteral("YOASOBI"), QStringLiteral("THE BOOK"));
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setCover(key, QStringLiteral("/covers/x.jpg"));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(model.data(model.index(2), AlbumModel::CoverRole).toUrl(), QUrl::fromLocalFile(QStringLiteral("/covers/x.jpg")));
        model.setCover(key, QStringLiteral("/covers/x.jpg"));
        QCOMPARE(changed.size(), 1); // the same again changes nothing
        model.setCover(QStringLiteral("nope"), QStringLiteral("/covers/y.jpg"));
        QCOMPARE(changed.size(), 1);
    }
};

QTEST_GUILESS_MAIN(AlbumModelTest)
#include "AlbumModelTest.moc"
