#include "ui/AppController.h"

#include <QCollator>
#include <QDir>
#include <QMap>
#include <QStandardPaths>

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
{
    if (options.configFile.isEmpty())
        m_settings = std::make_unique<QSettings>(QStringLiteral("mvplayer"), QStringLiteral("mvplayer"));
    else
        m_settings = std::make_unique<QSettings>(options.configFile, QSettings::IniFormat);

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
    m_cfg.concurrency = m_settings->value(QStringLiteral("import/concurrency"), 2).toInt();
    m_cfg.ytdlpArgs = m_settings->value(QStringLiteral("import/ytdlpArgs")).toStringList();
    m_accent = m_settings->value(QStringLiteral("ui/accent"), m_accent).toString();
    m_sidebarFacet = m_settings->value(QStringLiteral("ui/sidebarFacet"), m_sidebarFacet).toString();
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
    }
    if (!options.mvDir.isEmpty())
        m_cfg.mvDir = QDir(options.mvDir).absolutePath();

    m_facetTimer.setSingleShot(true);
    m_facetTimer.setInterval(150);
    connect(&m_facetTimer, &QTimer::timeout, this, &AppController::rebuildFacets);
    auto scheduleFacets = [this] { m_facetTimer.start(); };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, scheduleFacets);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, scheduleFacets);
    connect(m_model, &QAbstractItemModel::dataChanged, this, scheduleFacets);
    connect(m_model, &QAbstractItemModel::modelReset, this, &AppController::rebuildFacets);

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
    m_db = std::make_unique<Database>(QDir(dataDir).filePath(QStringLiteral("library.db")));
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

    m_manager = std::make_unique<ImportManager>(m_db.get());
    m_manager->setSettings(m_cfg);
    connect(m_manager.get(), &ImportManager::jobChanged, this, [this](const JobStatus &s) {
        m_jobs->update(s);
        if (s.finished) {
            ++m_sessionDone;
            refreshCounts();
        }
    });
    connect(m_manager.get(), &ImportManager::videoAdded, this, [this](qint64 id) {
        if (const auto v = m_db->video(id)) {
            m_model->upsert(*v);
            emit videoImported(m_model->toMap(m_model->rowCount() - 1));
        }
    });
    connect(m_manager.get(), &ImportManager::videoChanged, this, [this](qint64 id) {
        if (const auto v = m_db->video(id))
            m_model->upsert(*v);
    });
    connect(m_manager.get(), &ImportManager::videoRemoved, this, [this](qint64 id) {
        m_model->remove(id);
        refreshCounts();
    });
    connect(m_manager.get(), &ImportManager::activityChanged, this, [this] {
        if (!m_manager->busy())
            m_sessionDone = 0;
        refreshCounts();
    });

    if (configured())
        m_manager->start();
    refreshCounts();
}

void AppController::closeLibrary()
{
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
    emit activityChanged();
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

    QVariantList out;
    for (const Def &def : defs) {
        const QString key = QLatin1String(def.key);
        QHash<QString, int> counts;
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
            for (const QString &value : std::as_const(values))
                ++counts[value];
        }
        QStringList names = counts.keys();
        std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
            return key == QLatin1String("year") ? a > b : collator.compare(a, b) < 0;
        });
        QVariantList items;
        for (const QString &name : std::as_const(names))
            items << QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("count"), counts.value(name)}};
        out << QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("title"), QString::fromLatin1(def.title)},
            {QStringLiteral("items"), items},
        };
    }
    m_facets = out;
    emit facetsChanged();
}

void AppController::saveSettings()
{
    // Folders given on the command line are for that run only.
    if (m_options.musicDirs.isEmpty())
        m_settings->setValue(QStringLiteral("library/musicDirs"), m_cfg.musicDirs);
    if (m_options.mvDir.isEmpty())
        m_settings->setValue(QStringLiteral("library/mvDir"), m_cfg.mvDir);
    m_settings->setValue(QStringLiteral("import/replaceAudio"), m_cfg.replaceAudio);
    m_settings->setValue(QStringLiteral("import/allowUnofficial"), m_cfg.allowUnofficial);
    m_settings->setValue(QStringLiteral("import/skipStillImages"), m_cfg.skipStillImages);
    m_settings->setValue(QStringLiteral("player/volume"), m_volume);
    m_settings->setValue(QStringLiteral("player/muted"), m_muted);
}

void AppController::addMusicDir(const QString &dir)
{
    if (dir.isEmpty())
        return;
    const QString d = QDir(dir).absolutePath();
    if (m_cfg.musicDirs.contains(d))
        return;
    m_cfg.musicDirs << d;
    m_options.musicDirs.clear();
    saveSettings();
    if (m_manager && m_manager->settings().musicDirs.size() > 0) {
        // The library is already open: just scan the new folder in.
        m_manager->setSettings(m_cfg);
        m_manager->rescan();
    } else {
        openLibrary();
    }
    emit settingsChanged();
}

void AppController::removeMusicDir(const QString &dir)
{
    if (!m_cfg.musicDirs.removeAll(dir))
        return;
    m_options.musicDirs.clear();
    saveSettings();
    if (m_manager) {
        m_manager->setSettings(m_cfg);
        m_manager->rescan(); // drops that folder's tracks
    }
    emit settingsChanged();
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
    const int left = remaining();
    if (left > 0) {
        const int total = left + m_sessionDone;
        return tr("Finding videos · %1 of %2").arg(qMin(total, m_sessionDone + 1)).arg(total);
    }
    if (m_manager->scanning())
        return tr("Scanning library…");
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

QVariantList AppController::unmatchedTracks() const
{
    QVariantList out;
    if (!m_db)
        return out;
    const QVector<TrackInfo> tracks = m_db->tracksInState(
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
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}
