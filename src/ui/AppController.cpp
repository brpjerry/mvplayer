#include "ui/AppController.h"

#include "core/Util.h"
#include "core/YtDlp.h"

#include <QCollator>

#include <cmath>
#include <QDir>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QFileInfo>
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
    m_cfg.pauseBaseSecs = m_settings->value(QStringLiteral("import/pauseSeconds"), m_cfg.pauseBaseSecs).toInt();
    if (qEnvironmentVariableIsSet("MVPLAYER_PAUSE_SECS"))
        m_cfg.pauseBaseSecs = qEnvironmentVariableIntValue("MVPLAYER_PAUSE_SECS");
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

    m_manager = std::make_unique<ImportManager>(m_db.get());
    m_manager->setSettings(m_cfg);
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
    // per group of options, the orphans those of the library's videos.
    QVariantList out;
    for (const Def &def : defs) {
        const QString key = QLatin1String(def.key);
        QHash<QString, int> counts;
        QHash<QString, QSet<qint64>> reviewGroups;
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
                    reviewGroups[value].insert(v.reviewGroup > 0 ? v.reviewGroup : v.id);
                else
                    ++counts[value];
                if (orphan)
                    ++orphanCounts[value];
            }
        }
        QHash<QString, int> reviewCounts;
        for (auto it = reviewGroups.cbegin(); it != reviewGroups.cend(); ++it)
            reviewCounts.insert(it.key(), int(it.value().size()));
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
    // Several options for the same track are one thing to review.
    QSet<qint64> groups;
    for (const VideoInfo &v : m_model->videos()) {
        if (v.review)
            groups.insert(v.reviewGroup > 0 ? v.reviewGroup : v.id);
    }
    m_reviewCount = int(groups.size());
    m_reviewVideos = int(std::count_if(m_model->videos().begin(), m_model->videos().end(),
                                       [](const VideoInfo &v) { return v.review; }));
    m_orphanCount = int(std::count_if(m_model->videos().begin(), m_model->videos().end(),
                                      [this](const VideoInfo &v) { return m_model->isOrphan(v); }));
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
    m_settings->setValue(QStringLiteral("import/subtitleLangs"), m_cfg.subtitleLangs);
    m_settings->setValue(QStringLiteral("player/subtitles"), m_subtitlesOn);
    m_settings->setValue(QStringLiteral("player/subtitleOutline"), m_subtitleOutline);
    m_settings->setValue(QStringLiteral("player/subtitleShadow"), m_subtitleShadow);
    m_settings->setValue(QStringLiteral("player/autoExpand"), m_autoExpand);
}

void AppController::addMusicDir(const QString &dir)
{
    if (dir.isEmpty())
        return;
    const QString d = QDir(dir).absolutePath();
    if (m_cfg.musicDirs.contains(d, pathCase))
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
    m_cfg.cookiesFile = target;
    if (m_manager)
        m_manager->setSettings(m_cfg);
    setCookiesCheck({}, {});
    emit settingsChanged();
    return {};
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
}

void AppController::approveVideo(qint64 videoId)
{
    if (m_manager)
        m_manager->approveVideo(videoId);
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

QVariantMap AppController::videoSources(qint64 videoId) const
{
    if (!m_db)
        return {};
    const std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v)
        return {};
    // Several options under review wait for the same tracks, which point at
    // one of them.
    QVector<TrackInfo> tracks;
    if (v->review) {
        for (const VideoInfo &option : m_db->reviewOptions(v->reviewGroup > 0 ? v->reviewGroup : v->id))
            tracks += m_db->tracksForVideo(option.id, true);
    } else {
        tracks = m_db->tracksForVideo(videoId, true);
    }
    std::sort(tracks.begin(), tracks.end(), [](const TrackInfo &a, const TrackInfo &b) {
        return a.absent != b.absent ? !a.absent : betterSource(a, b);
    });

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
        format << formatDuration(t.duration);
        format.removeAll(QString());
        files << QVariantMap{
            {QStringLiteral("path"), displayPath(t.path)},
            {QStringLiteral("absent"), t.absent},
            {QStringLiteral("format"), format.join(QStringLiteral(" · "))},
            {QStringLiteral("tags"), tags},
        };
    }
    return {
        {QStringLiteral("ytUrl"), QStringLiteral("https://www.youtube.com/watch?v=") + v->ytId},
        {QStringLiteral("ytTitle"), v->ytTitle},
        {QStringLiteral("ytChannel"), v->ytChannel},
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
