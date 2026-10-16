#include "ui/AlbumModel.h"

#include "core/Util.h"

#include <QUrl>

#include <algorithm>

AlbumInfo AlbumInfo::from(const Database::AlbumSummary &s)
{
    AlbumInfo a;
    a.key = Database::albumKey(s.albumArtist, s.album);
    a.albumArtist = s.albumArtist;
    a.album = s.album;
    a.artists = s.artists;
    a.genres = s.genres;
    a.year = s.year;
    a.tracks = s.tracks;
    a.videos = s.videos;
    a.lookedUp = s.lookedUp;
    a.duration = s.duration;
    a.newest = s.newest;
    a.firstPath = s.firstPath;
    return a;
}

AlbumModel::AlbumModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AlbumModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_albums.size();
}

QVariant AlbumModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_albums.size())
        return {};
    const AlbumInfo &a = m_albums.at(index.row());
    switch (role) {
    case KeyRole: return a.key;
    case AlbumArtistRole: return a.albumArtist;
    case Qt::DisplayRole:
    case AlbumRole: return a.album;
    case ArtistRole: return a.artists;
    case GenreRole: return a.genres;
    case YearRole: return a.year;
    case TrackCountRole: return a.tracks;
    case VideoCountRole: return a.videos;
    case LookupCountRole: return a.lookedUp;
    case DurationRole: return a.duration;
    case DurationTextRole: return formatDuration(a.duration);
    case NewestRole: return a.newest;
    case CoverRole: return a.cover.isEmpty() ? QUrl() : QUrl::fromLocalFile(a.cover);
    case FirstPathRole: return a.firstPath;
    }
    return {};
}

QHash<int, QByteArray> AlbumModel::roleNames() const
{
    return {
        {KeyRole, "key"},
        {AlbumArtistRole, "albumArtist"},
        {AlbumRole, "album"},
        {ArtistRole, "artist"},
        {GenreRole, "genre"},
        {YearRole, "year"},
        {TrackCountRole, "trackCount"},
        {VideoCountRole, "videoCount"},
        {LookupCountRole, "lookupCount"},
        {DurationRole, "duration"},
        {DurationTextRole, "durationText"},
        {NewestRole, "newest"},
        {CoverRole, "cover"},
        {FirstPathRole, "firstPath"},
    };
}

QString AlbumModel::buildSearchText(const AlbumInfo &a)
{
    QStringList parts = {a.album, a.albumArtist, a.artists, a.genres};
    if (a.year > 0)
        parts << QString::number(a.year);
    return foldText(parts.join(QLatin1Char('\n')));
}

void AlbumModel::reset(const QVector<AlbumInfo> &albums)
{
    beginResetModel();
    m_albums = albums;
    m_search.clear();
    m_search.reserve(albums.size());
    m_rows.clear();
    for (int i = 0; i < albums.size(); ++i) {
        m_search << buildSearchText(albums.at(i));
        m_rows.insert(albums.at(i).key, i);
    }
    endResetModel();
}

void AlbumModel::setCover(const QString &key, const QString &path)
{
    const int row = m_rows.value(key, -1);
    if (row < 0 || m_albums.at(row).cover == path)
        return;
    m_albums[row].cover = path;
    emit dataChanged(index(row, 0), index(row, 0), {CoverRole});
}

QVariantMap AlbumModel::toMap(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_albums.size())
        return m;
    const QModelIndex idx = index(row, 0);
    const auto names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(idx, it.key()));
    return m;
}

// ---------------------------------------------------------------------------

AlbumFilterModel::AlbumFilterModel(AlbumModel *source, QObject *parent)
    : QSortFilterProxyModel(parent)
    , m_source(source)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    setSourceModel(source);
    setDynamicSortFilter(true);
    sort(0);
    connect(this, &QAbstractItemModel::rowsInserted, this, &AlbumFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &AlbumFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &AlbumFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &AlbumFilterModel::countChanged);
}

// Qt 6.10 replaced invalidateFilter() with an explicit begin/end pair.
void AlbumFilterModel::beginFilterEdit()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
}

void AlbumFilterModel::endFilterEdit()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

void AlbumFilterModel::setSearchText(const QString &text)
{
    if (text == m_searchText)
        return;
    beginFilterEdit();
    m_searchText = text;
    m_terms = foldText(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    endFilterEdit();
    emit searchTextChanged();
    emit countChanged();
}

void AlbumFilterModel::setFacet(const QString &type, const QString &value)
{
    if (type == m_facetType && value == m_facetValue)
        return;
    const bool resort = (type == QLatin1String("recent")) != (m_facetType == QLatin1String("recent"));
    beginFilterEdit();
    m_facetType = type;
    m_facetValue = value;
    endFilterEdit();
    if (resort)
        invalidate();
    emit facetChanged();
    emit countChanged();
}

void AlbumFilterModel::setSortMode(const QString &mode)
{
    if (mode == m_sortMode)
        return;
    const bool descending = defaultDescending(mode);
    const bool flipped = descending != m_descending;
    m_sortMode = mode;
    m_descending = descending;
    invalidate();
    emit sortModeChanged();
    if (flipped)
        emit sortDescendingChanged();
}

void AlbumFilterModel::setSortDescending(bool descending)
{
    if (descending == m_descending)
        return;
    m_descending = descending;
    invalidate();
    emit sortDescendingChanged();
}

bool AlbumFilterModel::matchesFacet(const AlbumInfo &a, const QString &type, const QString &value)
{
    if (type == QLatin1String("albumArtist"))
        return splitMulti(a.albumArtist).contains(value);
    if (type == QLatin1String("artist"))
        return splitMulti(a.artists).contains(value);
    if (type == QLatin1String("genre"))
        return splitMulti(a.genres).contains(value);
    if (type == QLatin1String("album"))
        return a.album == value;
    if (type == QLatin1String("year"))
        return QString::number(a.year) == value;
    return true;
}

bool AlbumFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &) const
{
    const AlbumInfo &a = m_source->at(sourceRow);
    if (!matchesFacet(a, m_facetType, m_facetValue))
        return false;
    const QString &text = m_source->searchText(sourceRow);
    for (const QString &term : m_terms) {
        if (!text.contains(term))
            return false;
    }
    return true;
}

bool AlbumFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const AlbumInfo &a = m_source->at(left.row());
    const AlbumInfo &b = m_source->at(right.row());
    const bool recent = m_facetType == QLatin1String("recent");
    const QString mode = recent ? QStringLiteral("modified") : m_sortMode;
    const bool descending = recent || m_descending;

    // The mode's own key, in the chosen direction.
    int c = 0;
    if (mode == QLatin1String("modified"))
        c = a.newest < b.newest ? -1 : a.newest > b.newest ? 1 : 0;
    else if (mode == QLatin1String("year"))
        c = a.year < b.year ? -1 : a.year > b.year ? 1 : 0;
    else if (mode == QLatin1String("artist"))
        c = m_collator.compare(a.albumArtist, b.albumArtist);
    else
        c = m_collator.compare(a.album, b.album);
    if (c != 0)
        return descending ? c > 0 : c < 0;

    // Ties, the natural way round whatever the direction: an artist's
    // albums by year, then by title.
    if (mode == QLatin1String("artist") && a.year != b.year)
        return a.year < b.year;
    int t = m_collator.compare(a.album, b.album);
    if (t != 0)
        return t < 0;
    t = m_collator.compare(a.albumArtist, b.albumArtist);
    if (t != 0)
        return t < 0;
    return a.key < b.key;
}

QVariantMap AlbumFilterModel::get(int row) const
{
    if (row < 0 || row >= rowCount())
        return {};
    return m_source->toMap(mapToSource(index(row, 0)).row());
}

QVariantList AlbumFilterModel::snapshot() const
{
    QVariantList list;
    const int n = rowCount();
    list.reserve(n);
    for (int i = 0; i < n; ++i)
        list << m_source->toMap(mapToSource(index(i, 0)).row());
    return list;
}

int AlbumFilterModel::rowOfKey(const QString &key) const
{
    const int source = m_source->rowOfKey(key);
    if (source < 0)
        return -1;
    const QModelIndex idx = mapFromSource(m_source->index(source, 0));
    return idx.isValid() ? idx.row() : -1;
}
