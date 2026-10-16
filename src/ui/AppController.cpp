#include "ui/AppController.h"
#include "core/Judge.h"
#include <QDirIterator>
#include <QDesktopServices>
#include <functional>

#include "core/ArtistChannels.h"
#include "core/Util.h"
#include "core/YtDlp.h"

#include <QCollator>

#include <cmath>
#include <QDir>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QStandardPaths>
#include <QThreadPool>

// ---------------------------------------------------------------------------
// JobModel
// ---------------------------------------------------------------------------

int JobModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_jobs.size();
}

QVariant JobModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_jobs.size())
        return {};
    const JobStatus &j = m_jobs.at(index.row());
    switch (role) {
    case TitleRole: return j.title;
    case ArtistRole: return j.artist;
    case StageRole: return j.stage;
    case ProgressRole: return j.progress;
    case FinishedRole: return j.finished;
    case OutcomeRole: return j.outcome;
    case DetailRole: return j.detail;
    }
    return {};
}

QHash<int, QByteArray> JobModel::roleNames() const
{
    return {
        {TitleRole, "title"}, {ArtistRole, "artist"}, {StageRole, "stage"}, {ProgressRole, "progress"},
        {FinishedRole, "finished"}, {OutcomeRole, "outcome"}, {DetailRole, "detail"},
    };
}

void JobModel::update(const JobStatus &s)
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].trackId == s.trackId && !m_jobs[i].finished) {
            m_jobs[i] = s;
            emit dataChanged(index(i, 0), index(i, 0));
            return;
        }
    }
    // Newest first; keep the list short.
    beginInsertRows(QModelIndex(), 0, 0);
    m_jobs.prepend(s);
    endInsertRows();
    const int cap = 60;
    if (m_jobs.size() > cap) {
        beginRemoveRows(QModelIndex(), cap, m_jobs.size() - 1);
        m_jobs.resize(cap);
        endRemoveRows();
    }
}

void JobModel::clear()
{
    beginResetModel();
    m_jobs.clear();
    endResetModel();
}

// ---------------------------------------------------------------------------
// AppController
// ---------------------------------------------------------------------------

AppController::AppController(const AppOptions &options, QObject *parent)
    : QObject(parent)
    , m_options(options)
    , m_model(new VideoModel(this))
    , m_filter(new VideoFilterModel(m_model, this))
    , m_jobs(new JobModel(this))
    , m_albumModel(new AlbumModel(this))
    , m_albums(new AlbumFilterModel(m_albumModel, this))
{
    if (options.configFile.isEmpty())
#ifdef Q_OS_WIN
        // A file rather than the registry, so settings can be edited by hand
        // as on Linux: %APPDATA%/mvplayer/mvplayer.ini
        m_settings = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope,
                                                 QStringLiteral("mvplayer"), QStringLiteral("mvplayer"));
#else
        m_settings = std::make_unique<QSettings>(QStringLiteral("mvplayer"), QStringLiteral("mvplayer"));
#endif
    else
        m_settings = std::make_unique<QSettings>(options.configFile, QSettings::IniFormat);
    // The Claude API key lives beside the settings, like the cookies.
    m_judge = std::make_unique<Judge>(QFileInfo(m_settings->fileName()).absoluteDir().filePath(QStringLiteral("claude-api-key")), this);
    connect(m_judge.get(), &Judge::keyChanged, this, &AppController::judgeChanged);
    connect(m_judge.get(), &Judge::checked, this, &AppController::judgeChanged);
    m_cfg.judgeKeyFile = QFileInfo(m_settings->fileName()).absoluteDir().filePath(QStringLiteral("claude-api-key"));

    m_cfg.chatGptAuthFile = QFileInfo(m_settings->fileName()).absoluteDir().filePath(QStringLiteral("chatgpt.json"));
    m_chatGpt = std::make_unique<ChatGptAuth>(m_cfg.chatGptAuthFile, this);
    connect(m_chatGpt.get(), &ChatGptAuth::openBrowser, this, [this](const QUrl &url) {
        if (!QDesktopServices::openUrl(url)) m_chatGpt->cancelSignIn();
    });

    const QString defaultMvDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath(QStringLiteral("MVs"));
    m_cfg.musicDirs = m_settings->value(QStringLiteral("library/musicDirs")).toStringList();
    // Earlier versions kept a single folder.
    const QString legacy = m_settings->value(QStringLiteral("library/musicDir")).toString();
    if (m_cfg.musicDirs.isEmpty() && !legacy.isEmpty())
        m_cfg.musicDirs << legacy;
    m_cfg.mvDir = m_settings->value(QStringLiteral("library/mvDir"), defaultMvDir).toString();
    m_cfg.replaceAudio = m_settings->value(QStringLiteral("import/replaceAudio"), true).toBool();
    m_cfg.allowUnofficial = m_settings->value(QStringLiteral("import/allowUnofficial"), false).toBool();
    m_cfg.skipStillImages = m_settings->value(QStringLiteral("import/skipStillImages"), true).toBool();
    m_ignoreSmallChannels = m_settings->value(QStringLiteral("import/ignoreSmallChannels"), true).toBool();
    m_minSubscribers = qMax(1, m_settings->value(QStringLiteral("import/minSubscribers"), 5000).toInt());
    m_cfg.minSubscribers = m_ignoreSmallChannels ? m_minSubscribers : 0;
    m_cfg.concurrency = m_settings->value(QStringLiteral("import/concurrency"), 2).toInt();
    m_cfg.judgeMode = m_settings->value(QStringLiteral("import/judgeMode"), QStringLiteral("off")).toString();
    if (m_cfg.judgeMode != QLatin1String("advice") && m_cfg.judgeMode != QLatin1String("judge"))
        m_cfg.judgeMode = QStringLiteral("off");
    m_cfg.judgeModel = m_settings->value(QStringLiteral("import/judgeModel"), QStringLiteral("sonnet")).toString();
    if (!Judge::models().contains(m_cfg.judgeModel))
        m_cfg.judgeModel = QStringLiteral("sonnet");
    m_cfg.judgeProvider = m_settings->value(QStringLiteral("import/judgeProvider"), QStringLiteral("claude")).toString();
    if (m_cfg.judgeProvider != QLatin1String("chatgpt")) m_cfg.judgeProvider = QStringLiteral("claude");
    m_cfg.chatGptAccount = m_chatGpt->account();
    if (m_cfg.judgeProvider == QLatin1String("chatgpt")) m_cfg.judgeModel = m_chatGpt->model();
    connect(m_chatGpt.get(), &ChatGptAuth::changed, this, [this] {
        const QString model = m_cfg.judgeProvider == QLatin1String("chatgpt") ? m_chatGpt->model() : m_cfg.judgeModel;
        if (m_cfg.chatGptAccount != m_chatGpt->account() || m_cfg.judgeModel != model) {
            m_cfg.chatGptAccount = m_chatGpt->account();
            m_cfg.judgeModel = model;
            if (m_manager) m_manager->setSettings(m_cfg);
            emit settingsChanged();
        }
        emit judgeChanged();
    });
    m_cfg.judgeThreads = qBound(1, m_settings->value(QStringLiteral("import/judgeThreads"), 1).toInt(), 8);
    m_judgeAlone = m_settings->value(QStringLiteral("judge/alone"), 0).toInt();
    m_judgeLeft = m_settings->value(QStringLiteral("judge/left"), 0).toInt();
    // By default the language the desktop is in.
    QString language = QLocale::system().name().section(QLatin1Char('_'), 0, 0).toLower();
    if (language.size() < 2 || language == QLatin1String("c"))
        language = QStringLiteral("en");
    m_cfg.subtitleLangs = m_settings->value(QStringLiteral("import/subtitleLangs"), QStringList{language}).toStringList();
    m_cfg.subtitleLangs.removeAll(QString());
    m_subtitlesOn = m_settings->value(QStringLiteral("player/subtitles"), true).toBool();
    m_subtitleOutline = qBound(0.0, m_settings->value(QStringLiteral("player/subtitleOutline"), 2.2).toDouble(), 6.0);
    m_subtitleShadow = qBound(0.0, m_settings->value(QStringLiteral("player/subtitleShadow"), 0.0).toDouble(), 6.0);
    m_autoExpand = m_settings->value(QStringLiteral("player/autoExpand"), true).toBool();
    m_cfg.ytdlpArgs = m_settings->value(QStringLiteral("import/ytdlpArgs")).toStringList();
    if (YtDlp::looksLikeCookies(cookiesPath()))
        m_cfg.cookiesFile = cookiesPath();
    m_cfg.accountOnBotCheck = m_settings->value(QStringLiteral("import/accountOnBotCheck"), false).toBool();
    m_keepEverything = m_settings->value(QStringLiteral("import/keepEverything"), false).toBool();
    m_cfg.pauseBaseSecs = m_settings->value(QStringLiteral("import/pauseSeconds"), m_cfg.pauseBaseSecs).toInt();
    if (qEnvironmentVariableIsSet("MVPLAYER_PAUSE_SECS"))
        m_cfg.pauseBaseSecs = qEnvironmentVariableIntValue("MVPLAYER_PAUSE_SECS");
    m_accent = m_settings->value(QStringLiteral("ui/accent"), m_accent).toString();
    m_sidebarFacet = m_settings->value(QStringLiteral("ui/sidebarFacet"), m_sidebarFacet).toString();
    m_mode = m_settings->value(QStringLiteral("ui/mode"), m_mode).toString();
    if (m_mode != QLatin1String("music"))
        m_mode = QStringLiteral("video");
    m_themeMode = m_settings->value(QStringLiteral("ui/theme"), m_themeMode).toString();
    m_flickDeceleration = qBound(200.0, m_settings->value(QStringLiteral("ui/flickDeceleration"), m_flickDeceleration).toDouble(), 50000.0);
    connect(&m_systemTheme, &SystemTheme::darkChanged, this, &AppController::systemDarkChanged);
    m_touchpadGain = qBound(0.5, m_settings->value(QStringLiteral("ui/touchpadGain"), m_touchpadGain).toDouble(), 20.0);
    m_wheelStep = qBound(20.0, m_settings->value(QStringLiteral("ui/wheelStep"), m_wheelStep).toDouble(), 1000.0);
    m_volume = m_settings->value(QStringLiteral("player/volume"), 1.0).toDouble();
    m_muted = m_settings->value(QStringLiteral("player/muted"), false).toBool();

    if (!options.musicDirs.isEmpty()) {
        m_cfg.musicDirs.clear();
        for (const QString &d : options.musicDirs)
            m_cfg.musicDirs << QDir(d).absolutePath();
        // Folders given on the command line set the base for the run.
        m_baseDir = FolderTreeModel::commonAncestor(m_cfg.musicDirs);
    } else {
        m_baseDir = m_settings->value(QStringLiteral("library/baseDir")).toString();
        // Settings from before there was a base: the folder that holds every music folder.
        if (m_baseDir.isEmpty() && !m_cfg.musicDirs.isEmpty()) {
            m_baseDir = FolderTreeModel::commonAncestor(m_cfg.musicDirs);
            m_settings->setValue(QStringLiteral("library/baseDir"), m_baseDir);
        }
        fitDirsToBase();
    }
    syncLibraryDirs();
    if (!options.mvDir.isEmpty())
        m_cfg.mvDir = QDir(options.mvDir).absolutePath();
    m_folderTree.setChecked(m_cfg.musicDirs);
    m_folderTree.setRoot(m_baseDir);

    m_facetTimer.setSingleShot(true);
    m_facetTimer.setInterval(150);
    connect(&m_facetTimer, &QTimer::timeout, this, &AppController::rebuildFacets);
    auto scheduleFacets = [this] { m_facetTimer.start(); };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, scheduleFacets);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, scheduleFacets);
    connect(m_model, &QAbstractItemModel::dataChanged, this, scheduleFacets);
    connect(m_model, &QAbstractItemModel::modelReset, this, &AppController::rebuildFacets);
    // The albums follow the music library: read again shortly after a scan,
    // or after a video comes or goes (the cards count them).
    m_albumTimer.setSingleShot(true);
    m_albumTimer.setInterval(150);
    connect(&m_albumTimer, &QTimer::timeout, this, &AppController::reloadAlbums);
    connect(m_albumModel, &QAbstractItemModel::modelReset, this, &AppController::rebuildFacets);
    connect(&m_covers, &CoverCache::coverReady, m_albumModel, &AlbumModel::setCover);

    openLibrary();
}

AppController::~AppController()
{
    shutdown();
}

void AppController::shutdown()
{
    closeLibrary();
    if (m_settings)
        m_settings->sync();
}

bool AppController::configured() const
{
    // A base directory alone is a music library, with no video folder yet.
    if (!m_cfg.libraryDirs.isEmpty() && QDir(m_cfg.libraryDirs.first()).exists())
        return true;
    for (const QString &d : m_cfg.musicDirs) {
        if (QDir(d).exists())
            return true;
    }
    return false;
}

void AppController::openLibrary()
{
    closeLibrary();
    m_sessionDone = 0;
    m_jobs->clear();

    const QString dataDir = ImportManager::dataDir(m_cfg.mvDir);
    if (!QDir().mkpath(dataDir)) {
        qWarning().noquote() << "cannot create" << dataDir;
        m_model->reset({});
        return;
    }
    m_db = std::make_unique<Database>(QDir(dataDir).filePath(QStringLiteral("library.db")), m_cfg.mvDir);
    QString error;
    if (!m_db->init(&error)) {
        qWarning().noquote() << "cannot open library database:" << error;
        m_db.reset();
        m_model->reset({});
        return;
    }

    // Only show videos whose file is actually there.
    QVector<VideoInfo> videos;
    for (const VideoInfo &v : m_db->allVideos()) {
        if (QFile::exists(v.path))
            videos << v;
    }
    m_model->reset(videos);
    m_shownKey.clear();
    for (const VideoInfo &v : std::as_const(videos))
        updateShown(v);
    emit candidatesChanged();
    m_covers.setDir(QDir(dataDir).filePath(QStringLiteral("covers")));
    reloadAlbums();
    emit lookupAlbumsChanged();

    m_cfg.captureDir = m_keepEverything ? captureFolder() : QString();
    m_manager = std::make_unique<ImportManager>(m_db.get());
    m_manager->setSettings(m_cfg);
    m_manager->setKeepsUndo(true);
    connect(m_manager.get(), &ImportManager::undoChanged, this, &AppController::undoChanged, Qt::QueuedConnection);
    connect(m_manager.get(), &ImportManager::jobChanged, this, [this](const JobStatus &s) {
        m_jobs->update(s);
        if (s.finished && !s.upgrade && s.outcome != QLatin1String("postponed")) {
            ++m_sessionDone;
            refreshCounts();
        }
    });
    connect(m_manager.get(), &ImportManager::videoAdded, this, [this](qint64 id) {
        if (const auto v = m_db->video(id)) {
            m_model->upsert(*v);
            updateShown(*v);
            emit candidatesChanged();
            emit videoImported(m_model->toMap(m_model->rowCount() - 1));
        }
    });
    connect(m_manager.get(), &ImportManager::videoChanged, this, [this](qint64 id) {
        if (const auto v = m_db->video(id)) {
            m_model->upsert(*v);
            updateShown(*v);
            emit candidatesChanged();
        }
    });
    connect(m_manager.get(), &ImportManager::videoRemoved, this, [this](qint64 id) {
        m_model->remove(id);
        m_shownKey.remove(id);
        emit candidatesChanged();
        refreshCounts();
    });
    connect(m_manager.get(), &ImportManager::libraryChanged, this, [this] { m_albumTimer.start(); });
    connect(m_manager.get(), &ImportManager::videoAdded, this, [this] { m_albumTimer.start(); });
    connect(m_manager.get(), &ImportManager::videoRemoved, this, [this] { m_albumTimer.start(); });
    connect(m_manager.get(), &ImportManager::channelListsChanged, this, &AppController::channelListsChanged);
    connect(m_manager.get(), &ImportManager::channelJudged, this,
            [this](int ticket, qint64 videoId, bool accept, qint64, const QString &track, const QString &reason) {
        if (ticket != 0 && ticket == m_channelTicket)
            emit channelJudged(videoId, accept, track, reason);
    });
    connect(m_manager.get(), &ImportManager::channelPreviewDone, this, [this](int ticket) {
        if (ticket == m_channelTicket)
            emit channelPreviewDone();
    });
    connect(m_manager.get(), &ImportManager::judgeDecided, this, [this](qint64, bool alone) {
        if (alone)
            ++m_judgeAlone;
        else
            ++m_judgeLeft;
        m_settings->setValue(QStringLiteral("judge/alone"), m_judgeAlone);
        m_settings->setValue(QStringLiteral("judge/left"), m_judgeLeft);
        emit judgeChanged();
    });
    connect(m_manager.get(), &ImportManager::adviceChanged, this, [this](qint64 videoId) {
        // The card's badge shows the answer for the candidate on show.
        if (const std::optional<VideoInfo> v = m_db ? m_db->video(videoId) : std::nullopt)
            updateShown(*v);
        emit adviceChanged(videoId);
    });
    connect(m_manager.get(), &ImportManager::adviceQueueChanged, this, [this](int left) {
        m_adviceLeft = left;
        emit adviceQueueChanged();
    });
    connect(m_manager.get(), &ImportManager::activityChanged, this, [this] {
        if (!m_manager->busy())
            m_sessionDone = 0;
        refreshCounts();
        emit replaceChanged();
    });
    connect(m_manager.get(), &ImportManager::replaceStageChanged, this, [this](qint64 videoId, const QString &stage) {
        m_replaceVideoId = stage.isEmpty() ? 0 : videoId;
        m_replaceStage = stage;
        emit replaceChanged();
    });
    connect(m_manager.get(), &ImportManager::replaceOptions, this, &AppController::replaceOptionsReady);
    connect(m_manager.get(), &ImportManager::replaceChecked, this, &AppController::replaceCheckDone);

    if (configured())
        m_manager->start();
    refreshCounts();
}

void AppController::closeLibrary()
{
    m_albumTimer.stop();
    m_libraryTracks = 0;
    m_albumModel->reset({});
    if (m_manager) {
        m_manager->stop();
        m_manager.reset();
    }
    m_db.reset();
}

void AppController::refreshCounts()
{
    QVariantMap counts;
    if (m_db) {
        const QHash<QString, int> c = m_db->trackStateCounts();
        int total = 0;
        for (auto it = c.begin(); it != c.end(); ++it) {
            counts.insert(it.key(), it.value());
            total += it.value();
        }
        counts.insert(QStringLiteral("total"), total);
    }
    m_trackCounts = counts;
    const QSet<qint64> untracked = m_manager ? m_manager->untrackedIds() : QSet<qint64>();
    m_untrackedCount = int(untracked.size());
    m_model->setUntracked(untracked);
    emit activityChanged();
}

// The albums of the music library, for music mode's grid; a cover that is
// not made yet is made in the background and arrives through CoverCache.
void AppController::reloadAlbums()
{
    QVector<AlbumInfo> albums;
    int tracks = 0;
    if (m_db) {
        for (const Database::AlbumSummary &s : m_db->albumSummaries()) {
            AlbumInfo a = AlbumInfo::from(s);
            a.cover = m_covers.lookup(a.key, a.firstPath, true);
            tracks += a.tracks;
            albums << a;
        }
    }
    m_libraryTracks = tracks;
    m_albumModel->reset(albums);
}

void AppController::setMode(const QString &mode)
{
    const QString m = mode == QLatin1String("music") ? QStringLiteral("music") : QStringLiteral("video");
    if (m == m_mode)
        return;
    m_mode = m;
    m_settings->setValue(QStringLiteral("ui/mode"), m_mode);
    emit modeChanged();
    rebuildFacets();
}

QVariantList AppController::albumTracks(int row) const
{
    QVariantList out;
    if (!m_db || row < 0 || row >= m_albums->rowCount())
        return out;
    const AlbumInfo &a = m_albumModel->at(m_albums->mapToSource(m_albums->index(row, 0)).row());
    const QUrl cover = a.cover.isEmpty() ? QUrl() : QUrl::fromLocalFile(a.cover);
    for (const TrackInfo &t : m_db->tracksOfAlbum(a.albumArtist, a.album)) {
        QVariantMap m{
            {QStringLiteral("kind"), QStringLiteral("track")},
            {QStringLiteral("trackId"), t.id},
            {QStringLiteral("videoId"), t.videoId},
            {QStringLiteral("videoPath"), QString()},
            {QStringLiteral("ytTitle"), QString()},
            {QStringLiteral("path"), t.path},
            {QStringLiteral("title"), t.title},
            {QStringLiteral("artist"), t.artist},
            {QStringLiteral("albumArtist"), t.albumArtist},
            {QStringLiteral("album"), t.album},
            {QStringLiteral("albumKey"), a.key},
            {QStringLiteral("genre"), t.genre},
            {QStringLiteral("year"), t.year},
            {QStringLiteral("trackNo"), t.trackNo},
            {QStringLiteral("discNo"), t.discNo},
            {QStringLiteral("duration"), t.duration},
            {QStringLiteral("durationText"), formatDuration(t.duration)},
            {QStringLiteral("thumb"), cover},
            {QStringLiteral("codec"), t.codec},
            {QStringLiteral("bitrate"), t.bitrate},
            {QStringLiteral("lossless"), t.lossless},
            {QStringLiteral("sampleRate"), t.sampleRate},
            {QStringLiteral("bits"), t.bitsPerSample},
            {QStringLiteral("lookup"), t.lookup},
            {QStringLiteral("state"), t.state},
            {QStringLiteral("review"), false},
        };
        if (t.videoId > 0) {
            if (const std::optional<VideoInfo> v = m_db->video(t.videoId)) {
                m.insert(QStringLiteral("videoPath"), v->path);
                m.insert(QStringLiteral("ytTitle"), v->ytTitle);
            }
        }
        out << m;
    }
    return out;
}

QVariantList AppController::lookupAlbums() const
{
    QVariantList out;
    if (!m_db)
        return out;
    for (const Database::AlbumKey &k : m_db->lookupAlbums())
        out << QVariantMap{{QStringLiteral("albumArtist"), k.albumArtist}, {QStringLiteral("album"), k.album}};
    return out;
}

int AppController::lookupAlbum(const QString &albumArtist, const QString &album)
{
    if (!m_db)
        return 0;
    int brought = 0;
    for (const TrackInfo &t : m_db->tracksOfAlbum(albumArtist, album))
        brought += t.lookup == 0 ? 1 : 0;
    m_db->addLookupAlbum(albumArtist, album);
    emit lookupAlbumsChanged();
    // The next scan takes its tracks in: fingerprinted, then looked up (A13).
    if (m_manager)
        m_manager->rescan();
    return brought;
}

void AppController::removeLookupAlbum(const QString &albumArtist, const QString &album)
{
    if (!m_db)
        return;
    m_db->removeLookupAlbum(albumArtist, album);
    emit lookupAlbumsChanged();
    // Its tracks outside the video folders are indexed only again, at the
    // next scan; what they have stays with them.
    if (m_manager)
        m_manager->rescan();
}

int AppController::reimportAlbum(const QString &albumArtist, const QString &album)
{
    if (!m_manager || !m_db)
        return 0;
    const int n = m_manager->reimport(m_db->trackIdsOfAlbum(albumArtist, album));
    refreshCounts();
    return n;
}

void AppController::rebuildFacets()
{
    struct Def {
        const char *key;
        const char *title;
    };
    static const Def defs[] = {
        {"albumArtist", "Album Artists"}, {"artist", "Artists"}, {"genre", "Genres"},
        {"album", "Albums"}, {"year", "Years"},
    };
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    // Each section of the grid has its own list: the review queue one card
    // per video, the orphans those of the library's videos.
    QVariantList out;
    for (const Def &def : defs) {
        const QString key = QLatin1String(def.key);
        QHash<QString, int> counts;
        QHash<QString, int> reviewCounts;
        QHash<QString, int> orphanCounts;
        for (const VideoInfo &v : m_model->videos()) {
            QStringList values;
            if (key == QLatin1String("albumArtist"))
                values = splitMulti(v.albumArtist);
            else if (key == QLatin1String("artist"))
                values = splitMulti(v.artist);
            else if (key == QLatin1String("genre"))
                values = splitMulti(v.genre);
            else if (key == QLatin1String("album") && !v.album.isEmpty())
                values << v.album;
            else if (key == QLatin1String("year") && v.year > 0)
                values << QString::number(v.year);
            const bool orphan = m_model->isOrphan(v);
            for (const QString &value : std::as_const(values)) {
                if (v.review)
                    ++reviewCounts[value];
                else
                    ++counts[value];
                if (orphan)
                    ++orphanCounts[value];
            }
        }
        // In music mode the library's list counts albums, not videos.
        if (m_mode == QLatin1String("music")) {
            counts.clear();
            for (const AlbumInfo &a : m_albumModel->albums()) {
                QStringList values;
                if (key == QLatin1String("albumArtist"))
                    values = splitMulti(a.albumArtist);
                else if (key == QLatin1String("artist"))
                    values = splitMulti(a.artists);
                else if (key == QLatin1String("genre"))
                    values = splitMulti(a.genres);
                else if (key == QLatin1String("album") && !a.album.isEmpty())
                    values << a.album;
                else if (key == QLatin1String("year") && a.year > 0)
                    values << QString::number(a.year);
                for (const QString &value : std::as_const(values))
                    ++counts[value];
            }
        }
        auto itemsOf = [&](const QHash<QString, int> &counts) {
            QStringList names = counts.keys();
            std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
                return key == QLatin1String("year") ? a > b : collator.compare(a, b) < 0;
            });
            QVariantList items;
            for (const QString &name : std::as_const(names))
                items << QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("count"), counts.value(name)}};
            return items;
        };
        out << QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("title"), QString::fromLatin1(def.title)},
            {QStringLiteral("items"), QVariantMap{
                {QStringLiteral("library"), itemsOf(counts)},
                {QStringLiteral("review"), itemsOf(reviewCounts)},
                {QStringLiteral("orphans"), itemsOf(orphanCounts)},
            }},
        };
    }
    m_facets = out;
    m_reviewVideos = int(std::count_if(m_model->videos().begin(), m_model->videos().end(),
                                       [](const VideoInfo &v) { return v.review; }));
    m_reviewCount = m_reviewVideos;
    m_orphanCount = int(std::count_if(m_model->videos().begin(), m_model->videos().end(),
                                      [this](const VideoInfo &v) { return m_model->isOrphan(v); }));
    emit facetsChanged();
}

void AppController::saveSettings()
{
    // Folders given on the command line are for that run only.
    if (m_options.musicDirs.isEmpty()) {
        m_settings->setValue(QStringLiteral("library/baseDir"), m_baseDir);
        m_settings->setValue(QStringLiteral("library/musicDirs"), m_cfg.musicDirs);
    }
    if (m_options.mvDir.isEmpty())
        m_settings->setValue(QStringLiteral("library/mvDir"), m_cfg.mvDir);
    m_settings->setValue(QStringLiteral("import/replaceAudio"), m_cfg.replaceAudio);
    m_settings->setValue(QStringLiteral("import/allowUnofficial"), m_cfg.allowUnofficial);
    m_settings->setValue(QStringLiteral("import/skipStillImages"), m_cfg.skipStillImages);
    m_settings->setValue(QStringLiteral("import/ignoreSmallChannels"), m_ignoreSmallChannels);
    m_settings->setValue(QStringLiteral("import/minSubscribers"), m_minSubscribers);
    m_settings->setValue(QStringLiteral("import/judgeMode"), m_cfg.judgeMode);
    m_settings->setValue(QStringLiteral("import/judgeProvider"), m_cfg.judgeProvider);
    if (m_cfg.judgeProvider == QLatin1String("claude"))
        m_settings->setValue(QStringLiteral("import/judgeModel"), m_cfg.judgeModel);
    m_settings->setValue(QStringLiteral("import/judgeThreads"), m_cfg.judgeThreads);
    m_settings->setValue(QStringLiteral("judge/alone"), m_judgeAlone);
    m_settings->setValue(QStringLiteral("judge/left"), m_judgeLeft);
    m_settings->setValue(QStringLiteral("import/accountOnBotCheck"), m_cfg.accountOnBotCheck);
    m_settings->setValue(QStringLiteral("import/keepEverything"), m_keepEverything);
    m_settings->setValue(QStringLiteral("player/volume"), m_volume);
    m_settings->setValue(QStringLiteral("player/muted"), m_muted);
    m_settings->setValue(QStringLiteral("import/subtitleLangs"), m_cfg.subtitleLangs);
    m_settings->setValue(QStringLiteral("player/subtitles"), m_subtitlesOn);
    m_settings->setValue(QStringLiteral("player/subtitleOutline"), m_subtitleOutline);
    m_settings->setValue(QStringLiteral("player/subtitleShadow"), m_subtitleShadow);
    m_settings->setValue(QStringLiteral("player/autoExpand"), m_autoExpand);
}

void AppController::fitDirsToBase()
{
    QStringList kept;
    for (const QString &d : std::as_const(m_cfg.musicDirs)) {
        if (FolderTreeModel::isWithin(d, m_baseDir))
            kept << d;
        else if (FolderTreeModel::isWithin(m_baseDir, d))
            kept << m_baseDir; // the base was inside a ticked folder: all of it stays
    }
    kept.removeDuplicates();
    m_cfg.musicDirs = kept;
}

// The base directory is the music library, read for the player; the
// folders given on the command line are read on their own, as before there
// was a base.
void AppController::syncLibraryDirs()
{
    m_cfg.libraryDirs.clear();
    if (m_options.musicDirs.isEmpty() && !m_baseDir.isEmpty())
        m_cfg.libraryDirs << m_baseDir;
}

void AppController::applyMusicDirs(QStringList dirs)
{
    dirs.removeDuplicates();
    const bool wasOpen = m_manager != nullptr;
    m_cfg.musicDirs = dirs;
    m_options.musicDirs.clear();
    syncLibraryDirs();
    saveSettings();
    m_folderTree.setChecked(m_cfg.musicDirs);
    if (wasOpen) {
        // The library is already open: a rescan takes the folders in, and
        // drops the tracks of those that went.
        m_manager->setSettings(m_cfg);
        m_manager->rescan();
    } else {
        openLibrary();
    }
    emit settingsChanged();
}

void AppController::setBaseDir(const QString &dir)
{
    if (dir.isEmpty())
        return;
    const QString d = QDir(dir).absolutePath();
    if (d.compare(m_baseDir, pathCase) == 0)
        return;
    m_baseDir = d;
    fitDirsToBase();
    m_folderTree.setRoot(m_baseDir);
    // The base is the music library: it is read anew whether or not the
    // ticked folders changed with it.
    applyMusicDirs(m_cfg.musicDirs);
}

void AppController::setFolderChecked(const QString &path, bool on)
{
    if (path.isEmpty())
        return;
    const QString p = QDir(path).absolutePath();
    if (!FolderTreeModel::isWithin(p, m_baseDir))
        return;
    const QStringList next = FolderTreeModel::withChecked(m_cfg.musicDirs, p, on);
    if (next != m_cfg.musicDirs)
        applyMusicDirs(next);
}

void AppController::addMusicDir(const QString &dir)
{
    if (dir.isEmpty())
        return;
    const QString d = QDir(dir).absolutePath();
    if (FolderTreeModel::stateOf(d, m_cfg.musicDirs) == FolderTreeModel::Checked)
        return;
    // A folder outside the base widens it to the folder that holds both.
    if (!FolderTreeModel::isWithin(d, m_baseDir)) {
        QStringList both{d};
        if (!m_baseDir.isEmpty())
            both << m_baseDir;
        m_baseDir = FolderTreeModel::commonAncestor(both);
        m_folderTree.setRoot(m_baseDir);
    }
    applyMusicDirs(FolderTreeModel::withChecked(m_cfg.musicDirs, d, true));
}

void AppController::removeMusicDir(const QString &dir)
{
    const QString d = QDir(dir).absolutePath();
    if (FolderTreeModel::stateOf(d, m_cfg.musicDirs) == FolderTreeModel::Unchecked)
        return;
    applyMusicDirs(FolderTreeModel::withChecked(m_cfg.musicDirs, d, false));
}

void AppController::setMvDir(const QString &dir)
{
    const QString d = QDir(dir).absolutePath();
    if (dir.isEmpty() || d == m_cfg.mvDir)
        return;
    m_cfg.mvDir = d;
    m_options.mvDir.clear();
    saveSettings();
    openLibrary();
    emit settingsChanged();
}

void AppController::setReplaceAudio(bool v)
{
    if (v == m_cfg.replaceAudio)
        return;
    m_cfg.replaceAudio = v;
    saveSettings();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    emit settingsChanged();
}

void AppController::setAllowUnofficial(bool v)
{
    if (v == m_cfg.allowUnofficial)
        return;
    m_cfg.allowUnofficial = v;
    saveSettings();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    emit settingsChanged();
}

QString AppController::setJudgeKey(const QString &key)
{
    const QString error = m_judge->setKey(key);
    if (error.isEmpty())
        m_judge->check();
    return error;
}

void AppController::removeJudgeKey()
{
    m_judge->removeKey();
}

void AppController::checkJudgeKey()
{
    m_judge->check();
}

QString AppController::cookiesPath() const
{
    return QFileInfo(m_settings->fileName()).absoluteDir().filePath(QStringLiteral("cookies.txt"));
}

QString AppController::cookiesAdded() const
{
    if (!hasCookies())
        return {};
    return QLocale().toString(QFileInfo(m_cfg.cookiesFile).lastModified().date(), QLocale::LongFormat);
}

QString AppController::importCookies(const QString &file)
{
    if (!YtDlp::looksLikeCookies(file))
        return tr("That is not a cookies.txt with YouTube cookies in it.");
    const QString target = cookiesPath();
    QFile source(file);
    QSaveFile out(target);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !source.open(QIODevice::ReadOnly)
        || !out.open(QIODevice::WriteOnly))
        return tr("Could not store the cookies.");
    // They are as good as the account's password: for this user only.
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (out.write(source.readAll()) < 0 || !out.commit())
        return tr("Could not store the cookies.");
    // What the pause wanted, where YouTube asked for a sign-in.
    const bool wanted = pauseKind() == QLatin1String("cookies");
    m_cfg.cookiesFile = target;
    if (m_manager)
        m_manager->setSettings(m_cfg);
    setCookiesCheck({}, {});
    emit settingsChanged();
    emit activityChanged();
    if (wanted && m_cfg.accountOnBotCheck)
        resumeImport();
    return {};
}

void AppController::setAccountOnBotCheck(bool v)
{
    if (v == m_cfg.accountOnBotCheck)
        return;
    const bool wanted = v && pauseKind() == QLatin1String("signin");
    m_cfg.accountOnBotCheck = v;
    saveSettings();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    emit settingsChanged();
    emit activityChanged();
    if (wanted)
        resumeImport();
}

void AppController::setCookiesCheck(const QString &state, const QString &status)
{
    ++m_cookiesCheck;
    m_checkingCookies = false;
    m_cookiesState = state;
    m_cookiesStatus = status;
    emit cookiesCheckChanged();
}

void AppController::checkCookies()
{
    if (!hasCookies() || m_checkingCookies)
        return;
    setCookiesCheck({}, {});
    m_checkingCookies = true;
    emit cookiesCheckChanged();
    // Asked of a video of the library, where there is one: music is what
    // the account's better audio is offered for.
    QString id = QStringLiteral("dQw4w9WgXcQ");
    if (m_model->rowCount() > 0)
        id = m_model->at(0).ytId;
    const int check = m_cookiesCheck;
    const ImportSettings cfg = m_cfg;
    QThreadPool::globalInstance()->start([this, id, check, cfg] {
        YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, nullptr);
        double kbps = 0;
        QString error, state, status;
        switch (yt.checkAccount(id, &kbps, &error)) {
        case YtDlp::Account::Premium:
            state = QStringLiteral("premium");
            status = tr("The cookies work: YouTube offers this account audio at %1 kbit/s.").arg(qRound(kbps));
            break;
        case YtDlp::Account::Ordinary:
            state = QStringLiteral("ordinary");
            status = tr("YouTube takes the cookies, but offers no Premium audio with them (%1 kbit/s at best). Is the account a Premium one?").arg(qRound(kbps));
            break;
        case YtDlp::Account::Expired:
            state = QStringLiteral("expired");
            status = tr("The cookies have expired. Export cookies.txt from the browser again — from a private window that is closed afterwards, or the browser replaces them once more.");
            break;
        case YtDlp::Account::Unknown:
            state = QStringLiteral("unknown");
            status = tr("Could not ask YouTube: %1").arg(error);
            break;
        }
        QMetaObject::invokeMethod(this, [this, check, state, status] {
            if (check == m_cookiesCheck)
                setCookiesCheck(state, status);
        }, Qt::QueuedConnection);
    });
}

void AppController::removeCookies()
{
    QFile::remove(cookiesPath());
    m_cfg.cookiesFile.clear();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    setCookiesCheck({}, {});
    emit settingsChanged();
    emit activityChanged();
}

void AppController::approveVideo(qint64 videoId)
{
    if (!m_manager || !m_db)
        return;
    // For the candidate track on show.
    qint64 trackId = 0;
    if (const auto v = m_db->video(videoId); v && v->review) {
        const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(m_db.get(), *v);
        if (!all.isEmpty())
            trackId = all.at(qBound(0, shownCandidate(videoId), int(all.size()) - 1)).track.id;
    }
    m_manager->approveVideo(videoId, trackId);
}

void AppController::undoVerdict()
{
    if (m_manager)
        m_manager->undoVerdict();
}

QVariantMap AppController::channelActions(qint64 videoId) const
{
    QVariantMap out{{QStringLiteral("offered"), false}};
    const std::optional<VideoInfo> v = m_db ? m_db->video(videoId) : std::nullopt;
    if (!v || !v->review)
        return out;
    // (Under the channel's own name: this upload may be listed under several.)
    const Database::ListedChannel channel = m_db->knownChannel(v->ytChannelId);
    out.insert(QStringLiteral("id"), v->ytChannelId);
    out.insert(QStringLiteral("name"), channel.name.isEmpty() ? v->ytChannel : channel.name);
    out.insert(QStringLiteral("handle"), channel.handle);
    // A name will not do: two channels can have the same one.
    if (v->ytChannelId.isEmpty() || !m_db->channelList(v->ytChannelId).isEmpty())
        return out;
    // Not where it is an artist's channel already, for the candidate showing.
    const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(m_db.get(), *v);
    if (all.isEmpty())
        return out;
    const TrackInfo &track = all.at(qBound(0, shownCandidate(videoId), int(all.size()) - 1)).track;
    out.insert(QStringLiteral("offered"),
               !Matcher::isOwnChannel(track, v->ytChannel)
                   && !ArtistChannels::knownArtistChannel(*m_db, Matcher::artistNames(track), v->ytChannelId, v->ytChannel));
    return out;
}

QVariantMap AppController::channelChange(qint64 videoId, bool allow)
{
    QVariantMap out;
    const std::optional<VideoInfo> of = m_db ? m_db->video(videoId) : std::nullopt;
    if (!of || of->ytChannelId.isEmpty() || !m_manager)
        return out;
    const Database::ListedChannel channel = m_db->knownChannel(of->ytChannelId);
    out.insert(QStringLiteral("id"), of->ytChannelId);
    out.insert(QStringLiteral("name"), channel.name.isEmpty() ? of->ytChannel : channel.name);
    out.insert(QStringLiteral("handle"), channel.handle);
    out.insert(QStringLiteral("allow"), allow);
    QVariantList videos;
    for (const VideoInfo &v : m_db->videosOfChannel(of->ytChannelId)) {
        // Allowing changes what waits for a verdict; denying, everything.
        if (allow && !v.review)
            continue;
        QStringList tracks;
        if (v.review) {
            for (const Database::Candidate &c : ImportManager::candidatesInOrder(m_db.get(), v))
                tracks << (c.track.artist.isEmpty() ? c.track.title : QStringLiteral("%1 — %2").arg(c.track.artist, c.track.title));
        } else {
            QSet<qint64> seen;
            for (const TrackInfo &t : m_db->tracksForVideo(v.id)) {
                if (seen.contains(Database::trackKey(t)))
                    continue;
                seen.insert(Database::trackKey(t));
                tracks << (t.artist.isEmpty() ? t.title : QStringLiteral("%1 — %2").arg(t.artist, t.title));
            }
        }
        videos << QVariantMap{
            {QStringLiteral("videoId"), v.id},
            {QStringLiteral("ytTitle"), v.ytTitle},
            {QStringLiteral("thumb"), v.thumb.isEmpty() ? QString() : QUrl::fromLocalFile(v.thumb).toString()},
            {QStringLiteral("review"), v.review},
            {QStringLiteral("tracks"), tracks},
        };
    }
    out.insert(QStringLiteral("videos"), videos);
    m_channelTicket = allow ? m_manager->previewAllowChannel(of->ytChannelId) : 0;
    if (!allow)
        m_manager->cancelChannelPreview();
    return out;
}

void AppController::cancelChannelChange()
{
    m_channelTicket = 0;
    if (m_manager)
        m_manager->cancelChannelPreview();
}

void AppController::applyChannelChange(const QString &channelId, bool allow)
{
    if (!m_manager || !m_db || channelId.isEmpty())
        return;
    m_channelTicket = 0;
    const Database::ListedChannel c = m_db->knownChannel(channelId);
    if (allow)
        m_manager->allowChannel(c.id, c.name, c.handle);
    else
        m_manager->denyChannel(c.id, c.name, c.handle);
}

void AppController::unlistChannel(const QString &channelId)
{
    if (m_manager)
        m_manager->unlistChannel(channelId);
}

QVariantList AppController::listedChannels() const
{
    QVariantList out;
    if (!m_db)
        return out;
    for (const Database::ListedChannel &c : m_db->channelLists()) {
        out << QVariantMap{{QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name}, {QStringLiteral("handle"), c.handle},
                           {QStringLiteral("list"), c.list}};
    }
    return out;
}

QVariantList AppController::candidates(qint64 videoId) const
{
    QVariantList out;
    const std::optional<VideoInfo> v = m_db ? m_db->video(videoId) : std::nullopt;
    if (!v || !v->review)
        return out;
    for (const Database::Candidate &c : ImportManager::candidatesInOrder(m_db.get(), *v)) {
        QVariantList elsewhere;
        for (const VideoInfo &o : m_db->candidacies(c.track)) {
            if (o.id != videoId)
                elsewhere << o.id;
        }
        out << QVariantMap{
            {QStringLiteral("trackId"), c.track.id},
            {QStringLiteral("title"), c.track.title},
            {QStringLiteral("artist"), c.track.artist},
            {QStringLiteral("album"), c.track.album},
            {QStringLiteral("length"), c.track.duration > 0 ? formatDuration(c.track.duration) : QString()},
            {QStringLiteral("stream"), c.stream},
            {QStringLiteral("audioDetail"), c.audioDetail},
            {QStringLiteral("elsewhere"), elsewhere},
        };
    }
    return out;
}

int AppController::shownCandidate(qint64 videoId) const
{
    const std::optional<VideoInfo> v = m_db ? m_db->video(videoId) : std::nullopt;
    const auto key = m_shownKey.constFind(videoId);
    if (!v || !v->review || key == m_shownKey.constEnd())
        return 0;
    const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(m_db.get(), *v);
    for (int i = 0; i < all.size(); ++i) {
        if (Database::trackKey(all[i].track) == *key)
            return i;
    }
    return 0;
}

void AppController::showCandidate(qint64 videoId, int index)
{
    const std::optional<VideoInfo> v = m_db ? m_db->video(videoId) : std::nullopt;
    if (!v || !v->review)
        return;
    const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(m_db.get(), *v);
    if (index < 0 || index >= all.size())
        return;
    m_shownKey.insert(videoId, Database::trackKey(all[index].track));
    updateShown(*v);
    emit candidatesChanged();
}

int AppController::firstAudioTrack(qint64 videoId) const
{
    const std::optional<VideoInfo> v = m_db ? m_db->video(videoId) : std::nullopt;
    if (!v || !v->review)
        return 1;
    const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(m_db.get(), *v);
    if (all.isEmpty())
        return 1;
    // (A candidate without audio of its own in the file would keep YouTube's.)
    const Database::Candidate &c = all.at(qBound(0, shownCandidate(videoId), int(all.size()) - 1));
    return c.stream > 0 ? c.stream : v->ytStream + 1;
}

void AppController::updateShown(const VideoInfo &v)
{
    if (!v.review || !m_db)
        return;
    const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(m_db.get(), v);
    VideoModel::Shown shown;
    shown.count = int(all.size());
    if (!all.isEmpty()) {
        int at = 0;
        const auto key = m_shownKey.constFind(v.id);
        for (int i = 0; key != m_shownKey.constEnd() && i < all.size(); ++i) {
            if (Database::trackKey(all[i].track) == *key)
                at = i;
        }
        shown.index = at + 1;
        shown.title = all[at].track.title;
        shown.artist = all[at].track.artist;
        const qint64 shownKey = Database::trackKey(all[at].track);
        for (const Database::Advice &a : m_db->advice(v.id)) {
            if (a.trackKey == shownKey)
                shown.advice = {{QStringLiteral("asked"), true}, {QStringLiteral("keep"), a.keep}, {QStringLiteral("probability"), a.probability},
                                {QStringLiteral("error"), a.kind == QLatin1String("error")}};
        }
    }
    m_model->setShown(v.id, shown);
}

void AppController::rejectVideo(qint64 videoId)
{
    if (m_manager)
        m_manager->rejectVideo(videoId);
    refreshCounts();
}

void AppController::keepOrphan(qint64 videoId)
{
    if (m_manager)
        m_manager->keepUntracked(videoId);
    refreshCounts();
}

bool AppController::deleteOrphan(qint64 videoId)
{
    const bool deleted = m_manager && m_manager->deleteUntracked(videoId);
    refreshCounts();
    return deleted;
}

bool AppController::checkingQuality() const
{
    return m_manager && m_manager->checkingQuality();
}

void AppController::checkQuality()
{
    if (m_manager && hasCookies())
        m_manager->checkQuality();
}

void AppController::setSubtitleLangs(const QString &langs)
{
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    static const QRegularExpression code(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$"));
    QStringList list;
    for (const QString &l : langs.split(separators, Qt::SkipEmptyParts)) {
        if (code.match(l).hasMatch() && !list.contains(l))
            list << l;
    }
    if (list == m_cfg.subtitleLangs) {
        emit settingsChanged(); // the field shows what was understood
        return;
    }
    m_cfg.subtitleLangs = list;
    saveSettings();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    emit settingsChanged();
}

void AppController::setSubtitlesOn(bool on)
{
    if (on == m_subtitlesOn)
        return;
    m_subtitlesOn = on;
    saveSettings();
    emit settingsChanged();
}

void AppController::setSubtitleOutline(double v)
{
    v = qBound(0.0, std::round(v * 10) / 10, 6.0);
    if (v == m_subtitleOutline)
        return;
    m_subtitleOutline = v;
    saveSettings();
    emit settingsChanged();
}

void AppController::setSubtitleShadow(double v)
{
    v = qBound(0.0, std::round(v * 10) / 10, 6.0);
    if (v == m_subtitleShadow)
        return;
    m_subtitleShadow = v;
    saveSettings();
    emit settingsChanged();
}

void AppController::setAutoExpand(bool v)
{
    if (v == m_autoExpand)
        return;
    m_autoExpand = v;
    saveSettings();
    emit settingsChanged();
}

int AppController::deleteUntracked()
{
    const int n = m_manager ? m_manager->deleteUntracked() : 0;
    refreshCounts();
    return n;
}

bool AppController::fetchingSubtitles() const
{
    return m_manager && m_manager->fetchingSubtitles();
}

void AppController::fetchSubtitles()
{
    if (m_manager)
        m_manager->fetchSubtitles();
}

void AppController::setIgnoreSmallChannels(bool v)
{
    if (v == m_ignoreSmallChannels)
        return;
    m_ignoreSmallChannels = v;
    m_cfg.minSubscribers = m_ignoreSmallChannels ? m_minSubscribers : 0;
    saveSettings();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    emit settingsChanged();
}

void AppController::setMinSubscribers(int n)
{
    n = qBound(1, n, 1000000000);
    if (n == m_minSubscribers)
        return;
    m_minSubscribers = n;
    m_cfg.minSubscribers = m_ignoreSmallChannels ? m_minSubscribers : 0;
    saveSettings();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    emit settingsChanged();
}

void AppController::setSkipStillImages(bool v)
{
    if (v == m_cfg.skipStillImages)
        return;
    m_cfg.skipStillImages = v;
    saveSettings();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    emit settingsChanged();
}

bool AppController::busy() const
{
    return m_manager && m_manager->busy();
}

bool AppController::scanning() const
{
    return m_manager && m_manager->scanning();
}

int AppController::remaining() const
{
    return m_manager ? m_manager->queuedCount() + m_manager->activeCount() : 0;
}

QString AppController::statusText() const
{
    if (!m_manager)
        return {};
    if (m_manager->paused())
        return tr("Paused until %1").arg(QLocale().toString(m_manager->resumeAt().time(), QLocale::ShortFormat));
    const int left = remaining();
    if (left > 0) {
        const int total = left + m_sessionDone;
        return tr("Finding videos · %1 of %2").arg(qMin(total, m_sessionDone + 1)).arg(total);
    }
    if (m_manager->scanning())
        return tr("Scanning library…");
    if (m_manager->fetchingSubtitles()) {
        const int total = m_manager->qualityTotal();
        return tr("Fetching subtitles · %1 of %2").arg(qMin(total, m_manager->qualityChecked() + 1)).arg(total);
    }
    if (m_manager->checkingQuality()) {
        const int total = m_manager->qualityTotal();
        return tr("Checking quality · %1 of %2").arg(qMin(total, m_manager->qualityChecked() + 1)).arg(total);
    }
    return {};
}

void AppController::setAccent(const QString &accent)
{
    if (accent == m_accent || accent.isEmpty())
        return;
    m_accent = accent;
    m_settings->setValue(QStringLiteral("ui/accent"), m_accent);
    emit appearanceChanged();
}

void AppController::setThemeMode(const QString &mode)
{
    if (mode == m_themeMode || mode.isEmpty())
        return;
    m_themeMode = mode;
    m_settings->setValue(QStringLiteral("ui/theme"), m_themeMode);
    emit appearanceChanged();
}

void AppController::setSidebarFacet(const QString &key)
{
    if (key == m_sidebarFacet || key.isEmpty())
        return;
    m_sidebarFacet = key;
    m_settings->setValue(QStringLiteral("ui/sidebarFacet"), m_sidebarFacet);
    emit appearanceChanged();
}

void AppController::setVolume(double v)
{
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(v + 1.0, m_volume + 1.0))
        return;
    m_volume = v;
    m_settings->setValue(QStringLiteral("player/volume"), m_volume);
    emit volumeChanged();
}

void AppController::setMuted(bool m)
{
    if (m == m_muted)
        return;
    m_muted = m;
    m_settings->setValue(QStringLiteral("player/muted"), m_muted);
    emit volumeChanged();
}

bool AppController::importPaused() const
{
    return m_manager && m_manager->paused();
}

QString AppController::pauseReason() const
{
    return m_manager ? m_manager->pauseReason() : QString();
}

QString AppController::pauseKind() const
{
    const QString reason = pauseReason();
    if (!importPaused() || !YtDlp::looksBotCheck(reason))
        return {};
    // No account, or one YouTube no longer takes: by this pause's own
    // answer, or by the last check of the cookies.
    if (!hasCookies() || YtDlp::cookiesExpired(reason) || m_cookiesState == QLatin1String("expired"))
        return QStringLiteral("cookies");
    return m_cfg.accountOnBotCheck ? QString() : QStringLiteral("signin");
}

void AppController::resumeImport()
{
    if (m_manager)
        m_manager->resumeNow();
}

void AppController::rescan()
{
    if (m_manager)
        m_manager->rescan();
}

void AppController::retryUnmatched()
{
    if (m_manager)
        m_manager->retryUnmatched();
    refreshCounts();
}

int AppController::reimportVideo(qint64 videoId)
{
    const int n = m_manager ? m_manager->reimportVideo(videoId) : 0;
    refreshCounts();
    return n;
}

int AppController::reimportFacet(const QString &type, const QString &value)
{
    if (!m_manager || !m_db)
        return 0;
    QVector<qint64> tracks;
    for (qint64 videoId : m_filter->videosWithFacet(type, value)) {
        for (const TrackInfo &t : m_db->tracksForVideo(videoId))
            tracks << t.id;
        for (const Database::Candidate &c : m_db->candidates(videoId))
            tracks << c.track.id;
    }
    const int n = m_manager->reimport(tracks);
    refreshCounts();
    return n;
}

int AppController::reimportPath(const QString &path)
{
    const int n = m_manager ? m_manager->reimportPath(path) : 0;
    refreshCounts();
    return n;
}

bool AppController::replaceSearch(qint64 videoId)
{
    return m_manager && m_manager->replaceSearch(videoId);
}

QString AppController::youtubeId(const QString &text)
{
    const QString t = text.trimmed();
    static const QRegularExpression inUrl(QStringLiteral("(?:[?&]v=|youtu\\.be/|/shorts/|/embed/|/live/)([A-Za-z0-9_-]{11})(?![A-Za-z0-9_-])"));
    static const QRegularExpression bare(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    if (const QRegularExpressionMatch m = inUrl.match(t); m.hasMatch())
        return m.captured(1);
    if (bare.match(t).hasMatch())
        return t;
    return QString();
}

QString AppController::replaceCheck(qint64 videoId, const QString &text)
{
    const QString id = youtubeId(text);
    if (id.isEmpty())
        return tr("That is not a YouTube link or video id.");
    if (const auto v = m_db ? m_db->video(videoId) : std::nullopt; v && v->ytId == id)
        return tr("That is the video it has now.");
    if (!m_manager || !m_manager->replaceCheck(videoId, id))
        return tr("Still busy with the last one; try again in a moment.");
    return QString();
}

bool AppController::replaceWith(qint64 videoId, const QString &ytId)
{
    return m_manager && m_manager->replaceWith(videoId, youtubeId(ytId));
}

QVariantList AppController::unmatchedTracks() const
{
    QVariantList out;
    if (!m_db)
        return out;
    const QVector<TrackInfo> tracks = m_db->trackSummariesInState(
        {QStringLiteral("not_found"), QStringLiteral("failed"), QStringLiteral("skipped")});
    for (const TrackInfo &t : tracks) {
        out << QVariantMap{
            {QStringLiteral("title"), t.title},
            {QStringLiteral("artist"), splitMulti(t.albumArtist).value(0, t.artist)},
            {QStringLiteral("state"), t.state},
            {QStringLiteral("message"), t.message},
        };
    }
    return out;
}

QVector<TrackInfo> AppController::sourceTracks(const VideoInfo &v) const
{
    // Of a video under review: the music file of its candidate on show.
    if (v.review) {
        const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(m_db.get(), v);
        if (all.isEmpty())
            return {};
        return {all.at(qBound(0, shownCandidate(v.id), int(all.size()) - 1)).track};
    }
    QVector<TrackInfo> tracks = m_db->tracksForVideo(v.id, true);
    std::sort(tracks.begin(), tracks.end(), [](const TrackInfo &a, const TrackInfo &b) {
        return a.absent != b.absent ? !a.absent : betterSource(a, b);
    });
    return tracks;
}

QString AppController::trackLength(qint64 videoId) const
{
    const std::optional<VideoInfo> v = m_db ? m_db->video(videoId) : std::nullopt;
    if (!v)
        return {};
    const QVector<TrackInfo> tracks = sourceTracks(*v);
    return tracks.isEmpty() || tracks.first().duration <= 0 ? QString() : formatDuration(tracks.first().duration);
}

QVariantMap AppController::videoSources(qint64 videoId) const
{
    if (!m_db)
        return {};
    const std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v)
        return {};
    const QVector<TrackInfo> tracks = sourceTracks(*v);

    // The tags that say what the recording is come first, as most players
    // list them; long free text last; the rest in between, by name.
    static const QStringList first = {
        QStringLiteral("TITLE"), QStringLiteral("ARTIST"), QStringLiteral("ALBUMARTIST"), QStringLiteral("ALBUM"),
        QStringLiteral("DATE"), QStringLiteral("ORIGINALDATE"), QStringLiteral("TRACKNUMBER"), QStringLiteral("TRACKTOTAL"),
        QStringLiteral("DISCNUMBER"), QStringLiteral("DISCTOTAL"), QStringLiteral("GENRE"), QStringLiteral("COMPOSER"),
        QStringLiteral("LYRICIST"), QStringLiteral("ARRANGER"),
    };
    static const QStringList last = {QStringLiteral("COMMENT"), QStringLiteral("DESCRIPTION"), QStringLiteral("LYRICS"),
                                     QStringLiteral("UNSYNCEDLYRICS")};
    const auto rank = [](const QString &key) {
        const int f = int(first.indexOf(key));
        if (f >= 0)
            return f;
        const int l = int(last.indexOf(key));
        return l >= 0 ? 1000 + l : 500;
    };

    QVariantList files;
    for (const TrackInfo &t : std::as_const(tracks)) {
        QStringList keys = t.tags.keys();
        std::stable_sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) { return rank(a) < rank(b); });
        QVariantList tags;
        for (const QString &key : std::as_const(keys))
            tags << QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("value"), t.tags.value(key).toString()}};
        QStringList format = {t.codec.toUpper()};
        if (t.lossless && t.bitsPerSample > 0)
            format << QStringLiteral("%1-bit").arg(t.bitsPerSample);
        else if (t.bitrate > 0)
            format << QStringLiteral("%1 kbit/s").arg(t.bitrate);
        if (t.sampleRate > 0)
            format << QStringLiteral("%1 kHz").arg(QLocale::c().toString(t.sampleRate / 1000.0, 'g', 4));
        if (t.channels > 0)
            format << (t.channels == 1 ? QStringLiteral("mono") : t.channels == 2 ? QStringLiteral("stereo")
                                                                                   : QStringLiteral("%1 ch").arg(t.channels));
        format.removeAll(QString());
        files << QVariantMap{
            {QStringLiteral("path"), displayPath(t.path)},
            {QStringLiteral("absent"), t.absent},
            {QStringLiteral("length"), t.duration > 0 ? formatDuration(t.duration) : QString()},
            {QStringLiteral("format"), format.join(QStringLiteral(" · "))},
            {QStringLiteral("tags"), tags},
        };
    }
    // When the upload was published: kept with the video, or for one from
    // before that was kept, read off the importer's copy of its page in the
    // cache, where that is still there.
    QString published = v->ytDate;
    if (published.isNull()) {
        QFile page(QDir(ImportManager::cacheDir(m_cfg.mvDir)).filePath(QStringLiteral("videos/%1/audio.info.json").arg(v->ytId)));
        if (page.open(QIODevice::ReadOnly))
            published = YtDlp::publishedOn(QJsonDocument::fromJson(page.readAll()).object());
    }
    const QDate publishedOn = QDate::fromString(published, Qt::ISODate);
    return {
        {QStringLiteral("ytUrl"), QStringLiteral("https://www.youtube.com/watch?v=") + v->ytId},
        {QStringLiteral("ytTitle"), v->ytTitle},
        {QStringLiteral("ytChannel"), v->ytChannel},
        {QStringLiteral("ytDate"), publishedOn.isValid() ? QLocale().toString(publishedOn, QStringLiteral("d MMM yyyy")) : QString()},
        {QStringLiteral("videoLength"), v->duration > 0 ? formatDuration(v->duration) : QString()},
        {QStringLiteral("files"), files},
    };
}

QString AppController::urlToPath(const QUrl &url) const
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

QUrl AppController::pathToUrl(const QString &path) const
{
    return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path);
}

QString AppController::displayPath(const QString &path) const
{
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(path);
#else
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
#endif
}

void AppController::setJudgeMode(const QString &mode)
{
    const QString m = mode == QLatin1String("advice") || mode == QLatin1String("judge") ? mode : QStringLiteral("off");
    if (m == m_cfg.judgeMode || (m != QLatin1String("off") && !judgeReady()))
        return;
    // A lookup under way finishes as it began; the next starts in the new mode (X4).
    m_cfg.judgeMode = m;
    if (m_manager)
        m_manager->setSettings(m_cfg);
    m_settings->setValue(QStringLiteral("import/judgeMode"), m);
    emit settingsChanged();
}

void AppController::setJudgeProvider(const QString &provider)
{
    if ((provider != QLatin1String("claude") && provider != QLatin1String("chatgpt")) || provider == m_cfg.judgeProvider)
        return;
    m_cfg.judgeProvider = provider;
    m_cfg.judgeModel = provider == QLatin1String("chatgpt") ? m_chatGpt->model()
        : m_settings->value(QStringLiteral("import/judgeModel"), QStringLiteral("sonnet")).toString();
    if (provider == QLatin1String("claude") && !Judge::models().contains(m_cfg.judgeModel)) m_cfg.judgeModel = QStringLiteral("sonnet");
    m_settings->setValue(QStringLiteral("import/judgeProvider"), provider);
    if (m_manager) m_manager->setSettings(m_cfg);
    emit settingsChanged();
    emit judgeChanged();
    if (provider == QLatin1String("chatgpt")) m_chatGpt->refreshModels();
}

bool AppController::judgeReady() const
{
    if (m_cfg.judgeProvider == QLatin1String("claude")) return m_judge->hasKey();
    if (!m_chatGpt->signedIn() || m_chatGpt->busy()) return false;
    for (const auto &m : m_chatGpt->models())
        if (m.toMap().value(QStringLiteral("name")).toString() == m_cfg.judgeModel) return true;
    return false;
}

void AppController::setJudgeModel(const QString &model)
{
    if (m_cfg.judgeProvider == QLatin1String("chatgpt")) { m_chatGpt->setModel(model); return; }
    if (!Judge::models().contains(model) || model == m_cfg.judgeModel)
        return;
    m_cfg.judgeModel = model;
    if (m_manager)
        m_manager->setSettings(m_cfg);
    m_settings->setValue(QStringLiteral("import/judgeModel"), model);
    emit settingsChanged();
}

void AppController::setJudgeThreads(int n)
{
    n = qBound(1, n, 8);
    if (n == m_cfg.judgeThreads)
        return;
    m_cfg.judgeThreads = n;
    if (m_manager)
        m_manager->setSettings(m_cfg);
    m_settings->setValue(QStringLiteral("import/judgeThreads"), n);
    emit settingsChanged();
}

QVariantList AppController::judgeModels() const
{
    if (m_cfg.judgeProvider == QLatin1String("chatgpt")) return m_chatGpt->models();
    // What a lookup's verdict cost on average when measured (M).
    static const QHash<QString, QString> costs = {{QStringLiteral("haiku"), QStringLiteral("about 0.1¢ a lookup")},
                                                  {QStringLiteral("sonnet"), QStringLiteral("about 2¢ a lookup")},
                                                  {QStringLiteral("opus"), QStringLiteral("about 3¢ a lookup")}};
    QVariantList out;
    for (const QString &name : Judge::models()) {
        out.append(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("label"), Judge::modelLabel(name)},
                               {QStringLiteral("cost"), costs.value(name)}});
    }
    return out;
}

QVariantMap AppController::judgeAdvice(qint64 videoId, qint64 trackId)
{
    QVariantMap out{{QStringLiteral("asked"), false}};
    const std::optional<TrackInfo> track = m_db->track(trackId);
    if (!track)
        return out;
    const qint64 key = Database::trackKey(*track);
    for (const Database::Advice &a : m_db->advice(videoId)) {
        if (a.trackKey != key)
            continue;
        out = {{QStringLiteral("asked"), true}, {QStringLiteral("keep"), a.keep}, {QStringLiteral("probability"), a.probability},
               {QStringLiteral("kind"), a.kind}, {QStringLiteral("source"), a.source}, {QStringLiteral("version"), a.version},
               {QStringLiteral("whole"), a.whole}, {QStringLiteral("note"), a.note}, {QStringLiteral("mode"), a.mode},
               {QStringLiteral("model"), a.model}, {QStringLiteral("error"), a.kind == QLatin1String("error")}};
    }
    return out;
}

void AppController::askJudge(qint64 videoId, qint64 trackId, bool again)
{
    if (m_manager && judgeReady())
        m_manager->askJudge(videoId, trackId, again);
}

QString AppController::captureFolder() const
{
    return m_cfg.mvDir.isEmpty() ? QString() : QDir(m_cfg.mvDir).filePath(QStringLiteral(".mvplayer/capture"));
}

void AppController::setKeepEverything(bool on)
{
    if (on == m_keepEverything)
        return;
    m_keepEverything = on;
    m_cfg.captureDir = on ? captureFolder() : QString();
    if (m_manager)
        m_manager->setSettings(m_cfg);
    m_settings->setValue(QStringLiteral("import/keepEverything"), on);
    emit settingsChanged();
}

QString AppController::captureSizeText() const
{
    const QString folder = captureFolder();
    if (folder.isEmpty() || !QDir(folder).exists())
        return QStringLiteral("nothing kept yet");
    qint64 bytes = 0;
    int videos = 0;
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        bytes += it.fileInfo().size();
    }
    videos = int(QDir(QDir(folder).filePath(QStringLiteral("video"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
    const double gb = bytes / 1e9;
    return QStringLiteral("%1 for %2 videos").arg(gb >= 1 ? QStringLiteral("%1 GB").arg(gb, 0, 'f', 1) : QStringLiteral("%1 MB").arg(bytes / 1e6, 0, 'f', 0)).arg(videos);
}

void AppController::askJudgeAll()
{
    if (m_manager && judgeReady())
        m_manager->askJudgeAll();
}

// X4: a card whose candidate on show the judge would keep at the gate or
// above, and no other candidate of it as surely, is accepted for that
// candidate; a card where every candidate's answer is "not this one" at
// the gate or above is rejected. Every other card stays.
struct JudgePlan {
    QVector<QPair<qint64, qint64>> accept; // video, track
    QVector<qint64> reject;
};
static JudgePlan planJudged(Database *db, const std::function<int(qint64)> &shownOf)
{
    JudgePlan plan;
    for (const VideoInfo &v : db->videosUnderReview()) {
        const QVector<Database::Candidate> all = ImportManager::candidatesInOrder(db, v);
        if (all.isEmpty())
            continue;
        const QVector<Database::Advice> advice = db->advice(v.id);
        const auto answer = [&](const Database::Candidate &c) -> const Database::Advice * {
            const qint64 key = Database::trackKey(c.track);
            for (const Database::Advice &a : advice) {
                if (a.trackKey == key && a.kind != QLatin1String("error"))
                    return &a;
            }
            return nullptr;
        };
        int keeps = 0, rejects = 0;
        for (const Database::Candidate &c : all) {
            const Database::Advice *a = answer(c);
            if (!a)
                continue;
            if (a->keep && a->probability >= Judge::kGate)
                ++keeps;
            else if (!a->keep && 1 - a->probability >= Judge::kGate)
                ++rejects;
        }
        const Database::Candidate &shown = all.at(qBound(0, shownOf(v.id), int(all.size()) - 1));
        const Database::Advice *s = answer(shown);
        if (s && s->keep && s->probability >= Judge::kGate && keeps == 1)
            plan.accept.append({v.id, shown.track.id});
        else if (rejects == all.size())
            plan.reject.append(v.id);
    }
    return plan;
}

QVariantMap AppController::judgePlan() const
{
    if (!m_db)
        return {{QStringLiteral("accept"), 0}, {QStringLiteral("reject"), 0}};
    const JudgePlan plan = planJudged(m_db.get(), [this](qint64 id) { return shownCandidate(id); });
    return {{QStringLiteral("accept"), int(plan.accept.size())}, {QStringLiteral("reject"), int(plan.reject.size())}};
}

void AppController::acceptJudged()
{
    if (!m_manager || !m_db || m_cfg.judgeMode != QLatin1String("advice"))
        return;
    const JudgePlan plan = planJudged(m_db.get(), [this](qint64 id) { return shownCandidate(id); });
    for (const auto &[videoId, trackId] : plan.accept)
        m_manager->approveVideo(videoId, trackId, true);
    for (qint64 videoId : plan.reject)
        m_manager->rejectVideo(videoId, true);
    refreshCounts();
}
