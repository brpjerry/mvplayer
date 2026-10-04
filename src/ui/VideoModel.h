#pragma once

#include "core/Types.h"

#include <QAbstractListModel>
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
        ReviewRole,
        ReviewStartRole,
        ReviewEndRole,
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

private:
    static QString buildSearchText(const VideoInfo &v);

    QVector<VideoInfo> m_videos;
    QVector<QString> m_search; // folded text of every tag, parallel to m_videos
};

// The grid's view of the library: one sidebar facet + the search box.
class VideoFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString facetType READ facetType NOTIFY facetChanged)
    Q_PROPERTY(QString facetValue READ facetValue NOTIFY facetChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit VideoFilterModel(VideoModel *source, QObject *parent = nullptr);

    QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);
    QString facetType() const { return m_facetType; }
    QString facetValue() const { return m_facetValue; }
    QString sortMode() const { return m_sortMode; }
    void setSortMode(const QString &mode);
    int count() const { return rowCount(); }

    // type: all | recent | albumArtist | artist | genre | album | year, or
    // review: the videos that wait for the user's verdict, which are in no
    // other view.
    Q_INVOKABLE void setFacet(const QString &type, const QString &value = QString());
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QVariantList snapshot() const;
    Q_INVOKABLE int rowOfVideo(qint64 videoId) const;

signals:
    void searchTextChanged();
    void facetChanged();
    void sortModeChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    void beginFilterEdit();
    void endFilterEdit();

    VideoModel *m_source;
    QString m_searchText;
    QStringList m_terms;
    QString m_facetType = QStringLiteral("all");
    QString m_facetValue;
    QString m_sortMode = QStringLiteral("title");
    QCollator m_collator;
};
