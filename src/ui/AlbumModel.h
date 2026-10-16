#pragma once

#include "core/Database.h"

#include <QAbstractListModel>
#include <QCollator>
#include <QHash>
#include <QSortFilterProxyModel>

// One album of the music library, as the music player's grid shows it.
struct AlbumInfo {
    QString key;         // Database::albumKey(albumArtist, album)
    QString albumArtist;
    QString album;       // empty: its files carry no album tag
    QString artists;     // every artist of its files, "; "-joined
    QString genres;
    int year = 0;
    int tracks = 0;
    int videos = 0;      // tracks that have a video
    int lookedUp = 0;    // tracks that are looked up (TrackInfo::lookup)
    double duration = 0;
    qint64 newest = 0;   // mtime of its newest file
    QString firstPath;   // its first file, where the cover comes from
    QString cover;       // the cover's file (CoverCache); empty: none, or not made yet

    static AlbumInfo from(const Database::AlbumSummary &s);
};

// Every album of the music library.
class AlbumModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        AlbumArtistRole,
        AlbumRole,
        ArtistRole,       // "artist": every artist of its files
        GenreRole,
        YearRole,
        TrackCountRole,
        VideoCountRole,
        LookupCountRole,
        DurationRole,
        DurationTextRole,
        NewestRole,
        CoverRole,        // a file URL, or empty
        FirstPathRole,
    };

    explicit AlbumModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reset(const QVector<AlbumInfo> &albums);
    // An album's cover was made or found: its file.
    void setCover(const QString &key, const QString &path);

    const QVector<AlbumInfo> &albums() const { return m_albums; }
    const AlbumInfo &at(int row) const { return m_albums.at(row); }
    const QString &searchText(int row) const { return m_search.at(row); }
    QVariantMap toMap(int row) const;
    int rowOfKey(const QString &key) const { return m_rows.value(key, -1); }

private:
    static QString buildSearchText(const AlbumInfo &a);

    QVector<AlbumInfo> m_albums;
    QVector<QString> m_search; // folded text of every tag, parallel to m_albums
    QHash<QString, int> m_rows; // key -> row
};

// The grid's view of the albums through one sidebar facet and the search
// box, in one of the orders the sort menu offers.
class AlbumFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString facetType READ facetType NOTIFY facetChanged)
    Q_PROPERTY(QString facetValue READ facetValue NOTIFY facetChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(bool sortDescending READ sortDescending WRITE setSortDescending NOTIFY sortDescendingChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit AlbumFilterModel(AlbumModel *source, QObject *parent = nullptr);

    QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);
    QString facetType() const { return m_facetType; }
    QString facetValue() const { return m_facetValue; }
    // sortMode: title (the album's) | artist (the album artist, then the
    // year) | year (Chronological - Audio). Choosing a mode puts its
    // direction the natural way round (names A to Z, years newest first);
    // sortDescending then flips the mode's own key.
    QString sortMode() const { return m_sortMode; }
    void setSortMode(const QString &mode);
    bool sortDescending() const { return m_descending; }
    void setSortDescending(bool descending);
    static bool defaultDescending(const QString &mode) { return mode == QLatin1String("year"); }
    int count() const { return rowCount(); }

    // type: all | recent (newest files first) | albumArtist | artist | genre | year
    Q_INVOKABLE void setFacet(const QString &type, const QString &value = QString());
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QVariantList snapshot() const;
    Q_INVOKABLE int rowOfKey(const QString &key) const;
    static bool matchesFacet(const AlbumInfo &a, const QString &type, const QString &value);

signals:
    void searchTextChanged();
    void facetChanged();
    void sortModeChanged();
    void sortDescendingChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    void beginFilterEdit();
    void endFilterEdit();

    AlbumModel *m_source;
    QString m_searchText;
    QStringList m_terms;
    QString m_facetType = QStringLiteral("all");
    QString m_facetValue;
    QString m_sortMode = QStringLiteral("title");
    bool m_descending = false;
    QCollator m_collator;
};
