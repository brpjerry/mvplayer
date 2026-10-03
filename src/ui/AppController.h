#pragma once

#include "core/ImportManager.h"
#include "ui/SystemTheme.h"
#include "ui/VideoModel.h"

#include <QAbstractListModel>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <memory>

// Import jobs shown in the activity panel: running ones plus recent results.
class JobModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { TitleRole = Qt::UserRole + 1, ArtistRole, StageRole, ProgressRole, FinishedRole, OutcomeRole, DetailRole };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void update(const JobStatus &s);
    void clear();

private:
    QVector<JobStatus> m_jobs;
};

struct AppOptions {
    QString configFile; // empty: the user's normal settings
    QStringList musicDirs; // command line overrides, not persisted
    QString mvDir;
    bool mute = false;  // force silence for this run (testing); never saved
};

// The object QML talks to: settings, library models and import activity.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(VideoFilterModel *videos READ videos CONSTANT)
    Q_PROPERTY(JobModel *jobs READ jobs CONSTANT)
    Q_PROPERTY(QVariantList facets READ facets NOTIFY facetsChanged)
    Q_PROPERTY(int videoCount READ videoCount NOTIFY facetsChanged)

    Q_PROPERTY(QStringList musicDirs READ musicDirs NOTIFY settingsChanged)
    Q_PROPERTY(QString mvDir READ mvDir WRITE setMvDir NOTIFY settingsChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY settingsChanged)
    Q_PROPERTY(bool replaceAudio READ replaceAudio WRITE setReplaceAudio NOTIFY settingsChanged)
    Q_PROPERTY(bool allowUnofficial READ allowUnofficial WRITE setAllowUnofficial NOTIFY settingsChanged)
    Q_PROPERTY(bool skipStillImages READ skipStillImages WRITE setSkipStillImages NOTIFY settingsChanged)

    Q_PROPERTY(bool busy READ busy NOTIFY activityChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY activityChanged)
    Q_PROPERTY(int remaining READ remaining NOTIFY activityChanged)
    Q_PROPERTY(int sessionDone READ sessionDone NOTIFY activityChanged)
    Q_PROPERTY(QVariantMap trackCounts READ trackCounts NOTIFY activityChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY activityChanged)
    // YouTube is refusing requests; the queue waits and resumes by itself.
    Q_PROPERTY(bool importPaused READ importPaused NOTIFY activityChanged)
    Q_PROPERTY(QString pauseReason READ pauseReason NOTIFY activityChanged)

    // "auto" (follow the playing video) or a colour such as "#8b7dff"
    Q_PROPERTY(QString accent READ accent WRITE setAccent NOTIFY appearanceChanged)
    // "auto" (follow the operating system), "dark" or "light"
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY appearanceChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)
    // Which tag the sidebar lists: albumArtist | artist | genre | album | year
    Q_PROPERTY(QString sidebarFacet READ sidebarFacet WRITE setSidebarFacet NOTIFY appearanceChanged)

    // Scroll feel; set in the config file (ui/touchpadGain, ui/wheelStep,
    // ui/flickDeceleration).
    Q_PROPERTY(double flickDeceleration READ flickDeceleration CONSTANT)
    Q_PROPERTY(double touchpadGain READ touchpadGain CONSTANT)
    Q_PROPERTY(double wheelStep READ wheelStep CONSTANT)

    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY volumeChanged)

public:
    explicit AppController(const AppOptions &options, QObject *parent = nullptr);
    ~AppController() override;

    VideoFilterModel *videos() const { return m_filter; }
    JobModel *jobs() const { return m_jobs; }
    QVariantList facets() const { return m_facets; }
    int videoCount() const { return m_model->rowCount(); }

    QStringList musicDirs() const { return m_cfg.musicDirs; }
    Q_INVOKABLE void addMusicDir(const QString &dir);
    Q_INVOKABLE void removeMusicDir(const QString &dir);
    QString mvDir() const { return m_cfg.mvDir; }
    void setMvDir(const QString &dir);
    bool configured() const;
    bool replaceAudio() const { return m_cfg.replaceAudio; }
    void setReplaceAudio(bool v);
    bool allowUnofficial() const { return m_cfg.allowUnofficial; }
    void setAllowUnofficial(bool v);
    bool skipStillImages() const { return m_cfg.skipStillImages; }
    void setSkipStillImages(bool v);

    bool busy() const;
    bool scanning() const;
    int remaining() const;
    int sessionDone() const { return m_sessionDone; }
    QVariantMap trackCounts() const { return m_trackCounts; }
    QString statusText() const;
    bool importPaused() const;
    QString pauseReason() const;
    Q_INVOKABLE void resumeImport();

    QString accent() const { return m_accent; }
    void setAccent(const QString &accent);
    QString themeMode() const { return m_themeMode; }
    void setThemeMode(const QString &mode);
    bool systemDark() const { return m_systemTheme.dark(); }
    QString sidebarFacet() const { return m_sidebarFacet; }
    void setSidebarFacet(const QString &key);

    double flickDeceleration() const { return m_flickDeceleration; }
    double touchpadGain() const { return m_touchpadGain; }
    double wheelStep() const { return m_wheelStep; }

    double volume() const { return m_volume; }
    void setVolume(double v);
    bool muted() const { return m_muted || m_options.mute; }
    void setMuted(bool m);

    Q_INVOKABLE void rescan();
    Q_INVOKABLE void retryUnmatched();
    Q_INVOKABLE QVariantList unmatchedTracks() const;
    Q_INVOKABLE QString urlToPath(const QUrl &url) const;
    Q_INVOKABLE QUrl pathToUrl(const QString &path) const;
    Q_INVOKABLE QString displayPath(const QString &path) const;

    void shutdown();

signals:
    void facetsChanged();
    void settingsChanged();
    void activityChanged();
    void volumeChanged();
    void appearanceChanged();
    void systemDarkChanged();
    void videoImported(const QVariantMap &video);

private:
    void openLibrary();
    void closeLibrary();
    void rebuildFacets();
    void refreshCounts();
    void saveSettings();

    AppOptions m_options;
    std::unique_ptr<QSettings> m_settings;
    ImportSettings m_cfg;

    std::unique_ptr<Database> m_db;
    std::unique_ptr<ImportManager> m_manager;
    VideoModel *m_model;
    VideoFilterModel *m_filter;
    JobModel *m_jobs;

    QVariantList m_facets;
    QTimer m_facetTimer;
    QVariantMap m_trackCounts;
    int m_sessionDone = 0;
    QString m_accent = QStringLiteral("auto");
    QString m_sidebarFacet = QStringLiteral("albumArtist");
    SystemTheme m_systemTheme;
    QString m_themeMode = QStringLiteral("auto");
    double m_flickDeceleration = 4800; // px/s²: how quickly a flick glides to a stop
    double m_touchpadGain = 4.0;
    double m_wheelStep = 170;
    double m_volume = 1.0;
    bool m_muted = false;
};
