#include "core/Database.h"

#include <QtTest>
#include <QTemporaryDir>

#include <memory>

// The library database as the music player reads it: albums gathered from
// the tracks, an album's files in order, the albums asked for, and what a
// re-import does to a track that was not looked up (H9).
class DatabaseTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<Database> m_db;
    QHash<QString, qint64> m_ids; // path -> id

    void add(const QString &rel, const QString &title, const QString &artist, const QString &albumArtist, const QString &album,
             int year, int disc, int trackNo, int lookup, double duration, qint64 mtime, const QString &genre = QString())
    {
        TrackInfo t;
        t.path = m_tmp.path() + QLatin1Char('/') + rel;
        t.mtime = mtime;
        t.size = 1000;
        t.title = title;
        t.artist = artist;
        t.albumArtist = albumArtist;
        t.album = album;
        t.genre = genre;
        t.year = year;
        t.discNo = disc;
        t.trackNo = trackNo;
        t.duration = duration;
        t.codec = QStringLiteral("flac");
        t.lossless = true;
        t.lookup = lookup;
        QVERIFY(m_db->upsertTrack(t));
        m_ids.insert(rel, t.id);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_db = std::make_unique<Database>(m_tmp.path() + QStringLiteral("/library.db"), m_tmp.path());
        QString error;
        QVERIFY2(m_db->init(&error), qPrintable(error));
        add(QStringLiteral("A/Abbey/02.flac"), QStringLiteral("Something"), QStringLiteral("A"), QStringLiteral("A"), QStringLiteral("Abbey"), 1969, 1, 2, 1, 180, 200, QStringLiteral("Rock"));
        add(QStringLiteral("A/Abbey/01.flac"), QStringLiteral("Come Together"), QStringLiteral("A"), QStringLiteral("A"), QStringLiteral("Abbey"), 1969, 1, 1, 1, 260, 100, QStringLiteral("Rock"));
        add(QStringLiteral("A/Help/01.flac"), QStringLiteral("Help!"), QStringLiteral("A"), QStringLiteral("A"), QStringLiteral("Help"), 1965, 0, 1, 0, 140, 300);
        add(QStringLiteral("B/X/01.flac"), QStringLiteral("Untitled"), QStringLiteral("B; C"), QStringLiteral("B"), QString(), 0, 0, 0, 0, 90, 400, QStringLiteral("Pop; Rock"));
    }

    void summaries()
    {
        const QVector<Database::AlbumSummary> s = m_db->albumSummaries();
        QCOMPARE(s.size(), 3);
        QCOMPARE(s.at(0).album, QStringLiteral("Abbey"));
        QCOMPARE(s.at(0).tracks, 2);
        QCOMPARE(s.at(0).lookedUp, 2);
        QCOMPARE(s.at(0).videos, 0);
        QCOMPARE(s.at(0).year, 1969);
        QCOMPARE(s.at(0).duration, 440.0);
        QCOMPARE(s.at(0).newest, qint64(200));
        QVERIFY(s.at(0).firstPath.endsWith(QStringLiteral("A/Abbey/01.flac"))); // disc and track order, not path order
        QCOMPARE(s.at(0).artists, QStringLiteral("A"));
        QCOMPARE(s.at(0).genres, QStringLiteral("Rock"));
        QCOMPARE(s.at(1).album, QStringLiteral("Help"));
        QCOMPARE(s.at(1).lookedUp, 0);
        QCOMPARE(s.at(2).albumArtist, QStringLiteral("B"));
        QCOMPARE(s.at(2).album, QString());
        QCOMPARE(s.at(2).year, 0);
        QCOMPARE(s.at(2).artists, QStringLiteral("B; C"));
        QCOMPARE(s.at(2).genres, QStringLiteral("Pop; Rock"));
        QCOMPARE(m_db->libraryTrackCount(), 4);
    }

    void albumTracks()
    {
        const QVector<TrackInfo> tracks = m_db->tracksOfAlbum(QStringLiteral("A"), QStringLiteral("Abbey"));
        QCOMPARE(tracks.size(), 2);
        QCOMPARE(tracks.at(0).title, QStringLiteral("Come Together"));
        QCOMPARE(tracks.at(1).title, QStringLiteral("Something"));
        QCOMPARE(m_db->trackIdsOfAlbum(QStringLiteral("A"), QStringLiteral("Abbey")),
                 (QVector<qint64>{m_ids.value(QStringLiteral("A/Abbey/01.flac")), m_ids.value(QStringLiteral("A/Abbey/02.flac"))}));
        QCOMPARE(m_db->tracksOfAlbum(QStringLiteral("A"), QStringLiteral("Nope")).size(), 0);
        // An album without a title (NULL in the table) is the one whose title is empty.
        QCOMPARE(m_db->tracksOfAlbum(QStringLiteral("B"), QString()).size(), 1);
        QCOMPARE(m_db->trackIdsOfAlbum(QStringLiteral("B"), QStringLiteral("")).size(), 1);
    }

    void lookupAlbums()
    {
        QCOMPARE(m_db->lookupAlbums().size(), 0);
        m_db->addLookupAlbum(QStringLiteral("B"), QString());
        m_db->addLookupAlbum(QStringLiteral("B"), QString()); // once
        m_db->addLookupAlbum(QStringLiteral("A"), QStringLiteral("Help"));
        const QVector<Database::AlbumKey> keys = m_db->lookupAlbums();
        QCOMPARE(keys.size(), 2);
        QCOMPARE(keys.at(0).albumArtist, QStringLiteral("A"));
        QCOMPARE(keys.at(1).albumArtist, QStringLiteral("B"));
        m_db->removeLookupAlbum(QStringLiteral("B"), QString());
        QCOMPARE(m_db->lookupAlbums().size(), 1);
        QCOMPARE(Database::albumKey(QStringLiteral("A"), QStringLiteral("Help")), QStringLiteral("A") + QChar(0x1f) + QStringLiteral("Help"));
        // The files indexed but not looked up.
        const QStringList paths = m_db->pathsNotLookedUp();
        QCOMPARE(paths.size(), 2);
        QVERIFY(paths.at(0).endsWith(QStringLiteral("A/Help/01.flac")));
    }

    void reimportBringsIn()
    {
        // Only the tracks that are looked up are counted.
        QHash<QString, int> counts = m_db->trackStateCounts();
        QCOMPARE(counts.value(QStringLiteral("pending")), 2);
        // A re-import names a track outside the video folders: looked up
        // from now on (2), which a scan does not take back.
        m_db->setTrackReimport(m_ids.value(QStringLiteral("A/Help/01.flac")));
        std::optional<TrackInfo> help = m_db->track(m_ids.value(QStringLiteral("A/Help/01.flac")));
        QVERIFY(help.has_value());
        QCOMPARE(help->lookup, 2);
        QVERIFY(help->reimport);
        QCOMPARE(help->state, QStringLiteral("pending"));
        // One in a video folder stays as it is (1).
        m_db->setTrackReimport(m_ids.value(QStringLiteral("A/Abbey/01.flac")));
        QCOMPARE(m_db->track(m_ids.value(QStringLiteral("A/Abbey/01.flac")))->lookup, 1);
        counts = m_db->trackStateCounts();
        QCOMPARE(counts.value(QStringLiteral("pending")), 3);
        QCOMPARE(m_db->pathsNotLookedUp().size(), 1);
        QCOMPARE(m_db->tracksInState({QStringLiteral("pending")}).size(), 3);
    }

    // Where a track sits in its video is kept with the video and with each
    // candidate of one under review, and maps a moment of the track onto
    // the video: the track that plays from its file goes on as its video.
    void audioSpans()
    {
        const QVector<AudioSpan> spans = {{2.5, 60.0, 2.5}, {75.0, 120.25, 7.25}};
        QCOMPARE(audioSpansText(spans), QStringLiteral("2.500-60.000@2.500,75.000-120.250@7.250"));
        QCOMPARE(audioSpansFromText(audioSpansText(spans)).size(), 2);
        QCOMPARE(audioSpansFromText(QStringLiteral("0.000-30.000@-4.000")).first().lag, -4.0);
        QVERIFY(audioSpansFromText(QStringLiteral("junk,1-2")).isEmpty());

        VideoInfo v;
        v.ytId = QStringLiteral("spans1");
        v.path = QStringLiteral("A/One [spans1].mkv");
        v.title = QStringLiteral("One");
        v.audioSource = QStringLiteral("library");
        v.reviewStart = 2.5;
        v.reviewEnd = 120.25;
        v.spans = spans;
        const qint64 id = m_db->insertVideo(v);
        QVERIFY(id > 0);
        std::optional<VideoInfo> back = m_db->video(id);
        QVERIFY(back.has_value());
        QCOMPARE(audioSpansText(back->spans), audioSpansText(spans));
        // Changed with the audio, and gone with it.
        back->spans = {{0.0, 50.0, -1.0}};
        m_db->updateVideoMedia(*back);
        QCOMPARE(m_db->video(id)->spans.size(), 1);
        QCOMPARE(m_db->video(id)->spans.first().lag, -1.0);
        back->spans.clear();
        m_db->updateVideoMedia(*back);
        QVERIFY(m_db->video(id)->spans.isEmpty());

        // A candidate's, for its own stream of the file.
        const std::optional<TrackInfo> track = m_db->track(m_ids.value(QStringLiteral("A/Abbey/01.flac")));
        QVERIFY(track.has_value());
        m_db->addCandidate(id, *track, 1, 0.5, QStringLiteral("FLAC 16/44"), spans);
        const QVector<Database::Candidate> cands = m_db->candidates(id);
        QCOMPARE(cands.size(), 1);
        QCOMPARE(audioSpansText(cands.first().spans), audioSpansText(spans));
        QCOMPARE(m_db->candidateRows(id).first().spans.size(), 2);

        // Track time onto video time: inside a span, by its lag; in a part
        // of the song the video leaves out, the edge of the nearest span;
        // past the end of what the video has, its last moment of the song.
        QCOMPARE(videoTimeOfTrack(spans, 0, 0.0), 2.5);
        QCOMPARE(videoTimeOfTrack(spans, 0, 30.0), 32.5);
        QCOMPARE(videoTimeOfTrack(spans, 0, 100.0), 107.25);
        QCOMPARE(videoTimeOfTrack(spans, 0, 60.0), 60.0);     // the gap begins: the song is cut at 57.5s of the track
        QCOMPARE(videoTimeOfTrack(spans, 0, 67.0), 75.0);     // nearer the span after the cut
        QCOMPARE(videoTimeOfTrack(spans, 0, 200.0), 120.25);
        // Without spans: the track starts where the video's library audio does, or at 0.
        QCOMPARE(videoTimeOfTrack({}, 2.5, 30.0), 32.5);
        QCOMPARE(videoTimeOfTrack({}, 0, 30.0), 30.0);
        QCOMPARE(videoTimeOfTrack({{0.0, 50.0, -4.0}}, 0, 1.0), 0.0); // the video opens 4s into the song
    }
};

QTEST_GUILESS_MAIN(DatabaseTest)
#include "DatabaseTest.moc"
