#pragma once

#include "core/Types.h"

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QCollator>
#include <QSortFilterProxyModel>

// Every video in the MV library.
class VideoModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role {
        VideoIdRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        AlbumArtistRole,
        AlbumRole,
        GenreRole,
        YearRole,
        DurationRole,
        DurationTextRole,
        ThumbRole,
        PathRole,
        QualityRole,
        AudioSourceRole,
        AudioDetailRole,
        AddedAtRole,
        YtIdRole,
        YtTitleRole,
        YtChannelRole,
        ReviewRole,
        ReviewStartRole,
        ReviewEndRole,
        ReviewOptionRole,  // which of its group's options this is, from 1
        ReviewOptionsRole, // how many options the group has
        SameTitleRole,     // of a video under review: a library video with its title {album, ytTitle}
        OrphanRole,        // a library video that no track in the music folders has
    };

    explicit VideoModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reset(const QVector<VideoInfo> &videos);
    void upsert(const VideoInfo &video);
    void remove(qint64 videoId);

    const QVector<VideoInfo> &videos() const { return m_videos; }
    const VideoInfo &at(int row) const { return m_videos.at(row); }
    const QString &searchText(int row) const { return m_search.at(row); }
    QVariantMap toMap(int row) const;
    // The videos waiting as options alongside `v` (itself included), by id.
    QVector<qint64> reviewOptions(const VideoInfo &v) const;
    // The accepted video that has the title of `v`, a video under review:
    // the song may have its video already. Null when there is none.
    const VideoInfo *sameTitle(const VideoInfo &v) const;
    // The videos no track has (Database::untrackedVideoIds). Those of them
    // in the library, not under review, are orphans.
    void setUntracked(const QSet<qint64> &ids);
    bool isOrphan(const VideoInfo &v) const { return !v.review && m_untracked.contains(v.id); }

private:
    static QString buildSearchText(const VideoInfo &v);
    void reviewOptionsChanged();

    // The library is thousands of videos and the review view hundreds of
    // cards; what a card asks per row must not walk the library.
    void reindex();
    static QString foldedTitle(const QString &title);
    void reviewRowsChanged(const QVector<int> &roles);

    QVector<VideoInfo> m_videos;
    QVector<QString> m_search; // folded text of every tag, parallel to m_videos
    QHash<qint64, QVector<qint64>> m_groups;  // review group -> its options, sorted
    QHash<QString, QVector<qint64>> m_titles; // folded title -> accepted videos with it
    QSet<qint64> m_untracked;
};

// The grid's view of one section — the library, the videos that wait for
// review, or the library's orphans — through one sidebar facet and the
// search box.
class VideoFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString section READ section NOTIFY sectionChanged)
    Q_PROPERTY(QString facetType READ facetType NOTIFY facetChanged)
    Q_PROPERTY(QString facetValue READ facetValue NOTIFY facetChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit VideoFilterModel(VideoModel *source, QObject *parent = nullptr);

    QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);
    QString section() const { return m_section; }
    QString facetType() const { return m_facetType; }
    QString facetValue() const { return m_facetValue; }
    QString sortMode() const { return m_sortMode; }
    void setSortMode(const QString &mode);
    int count() const { return rowCount(); }

    // type: all | recent | albumArtist | artist | genre | album | year. It
    // narrows every section alike.
    Q_INVOKABLE void setFacet(const QString &type, const QString &value = QString());
    // section: library | review, the videos that wait for the user's
    // verdict, which are in no other section | orphans, the library's
    // videos whose tracks are gone. The facet stays when some videos of the
    // section have it; otherwise the section shows whole, not empty.
    Q_INVOKABLE void showSection(const QString &section);
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QVariantList snapshot() const;
    Q_INVOKABLE int rowOfVideo(qint64 videoId) const;
    // Of several videos that wait as options for the same track, the review
    // view shows one. Steps to the next (+1) or previous (-1) option and
    // returns the video now shown.
    Q_INVOKABLE qint64 stepReview(qint64 videoId, int delta);
    // The videos of the section on show that a sidebar row stands for:
    // every one with that tag, whatever the search box says.
    QVector<qint64> videosWithFacet(const QString &type, const QString &value) const;

signals:
    void searchTextChanged();
    void sectionChanged();
    void facetChanged();
    void sortModeChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    void beginFilterEdit();
    void endFilterEdit();
    bool inSection(const VideoInfo &v, const QString &section) const;
    bool matchesFacet(const VideoInfo &v) const { return matchesFacet(v, m_facetType, m_facetValue); }
    static bool matchesFacet(const VideoInfo &v, const QString &type, const QString &value);

    VideoModel *m_source;
    QString m_searchText;
    QStringList m_terms;
    QString m_section = QStringLiteral("library");
    QString m_facetType = QStringLiteral("all");
    QString m_facetValue;
    QString m_sortMode = QStringLiteral("title");
    QHash<qint64, qint64> m_reviewShown; // review group -> the option on show
    QCollator m_collator;
};
