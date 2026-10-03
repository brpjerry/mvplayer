#include "ui/VideoModel.h"

#include "core/Util.h"

#include <QUrl>

namespace {

QString qualityLabel(const VideoInfo &v)
{
    const int h = qMin(v.width, v.height) > 0 ? qMin(v.height, v.width * 9 / 16 + 8) : 0;
    const int lines = qMax(h, v.height);
    QString label;
    if (v.width >= 7600 || lines >= 4300)
        label = QStringLiteral("8K");
    else if (v.width >= 3800 || lines >= 2100)
        label = QStringLiteral("4K");
    else if (v.width >= 2500 || lines >= 1400)
        label = QStringLiteral("1440p");
    else if (v.width >= 1900 || lines >= 1060)
        label = QStringLiteral("1080p");
    else if (v.width >= 1260 || lines >= 700)
        label = QStringLiteral("720p");
    else if (lines > 0)
        label = QStringLiteral("%1p").arg(lines);
    if (!label.isEmpty() && v.fps > 49 && !label.endsWith(QLatin1Char('K')))
        label += QString::number(qRound(v.fps));
    return label;
}

} // namespace

VideoModel::VideoModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int VideoModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_videos.size();
}

QVariant VideoModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_videos.size())
        return {};
    const VideoInfo &v = m_videos.at(index.row());
    switch (role) {
    case VideoIdRole: return v.id;
    case Qt::DisplayRole:
    case TitleRole: return v.title;
    case ArtistRole: return v.artist;
    case AlbumArtistRole: return v.albumArtist;
    case AlbumRole: return v.album;
    case GenreRole: return v.genre;
    case YearRole: return v.year;
    case DurationRole: return v.duration;
    case DurationTextRole: return formatDuration(v.duration);
    case ThumbRole: return v.thumb.isEmpty() ? QUrl() : QUrl::fromLocalFile(v.thumb);
    case PathRole: return v.path;
    case QualityRole: return qualityLabel(v);
    case AudioSourceRole: return v.audioSource;
    case AudioDetailRole: return v.audioDetail;
    case AddedAtRole: return v.addedAt;
    case YtIdRole: return v.ytId;
    }
    return {};
}

QHash<int, QByteArray> VideoModel::roleNames() const
{
    return {
        {VideoIdRole, "videoId"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {AlbumArtistRole, "albumArtist"},
        {AlbumRole, "album"},
        {GenreRole, "genre"},
        {YearRole, "year"},
        {DurationRole, "duration"},
        {DurationTextRole, "durationText"},
        {ThumbRole, "thumb"},
        {PathRole, "path"},
        {QualityRole, "quality"},
        {AudioSourceRole, "audioSource"},
        {AudioDetailRole, "audioDetail"},
        {AddedAtRole, "addedAt"},
        {YtIdRole, "ytId"},
    };
}

QVariantMap VideoModel::toMap(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_videos.size())
        return m;
    const QModelIndex idx = index(row, 0);
    const auto names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(idx, it.key()));
    return m;
}

QString VideoModel::buildSearchText(const VideoInfo &v)
{
    QStringList parts = {v.title, v.artist, v.albumArtist, v.album, v.genre, v.ytTitle, v.ytChannel};
    if (v.year > 0)
        parts << QString::number(v.year);
    for (auto it = v.tags.begin(); it != v.tags.end(); ++it)
        parts << it.value().toString();
    return foldText(parts.join(QLatin1Char('\n')));
}

void VideoModel::reset(const QVector<VideoInfo> &videos)
{
    beginResetModel();
    m_videos = videos;
    m_search.clear();
    m_search.reserve(videos.size());
    for (const VideoInfo &v : videos)
        m_search << buildSearchText(v);
    endResetModel();
}

void VideoModel::upsert(const VideoInfo &video)
{
    for (int i = 0; i < m_videos.size(); ++i) {
        if (m_videos[i].id == video.id) {
            m_videos[i] = video;
            m_search[i] = buildSearchText(video);
            emit dataChanged(index(i, 0), index(i, 0));
            return;
        }
    }
    beginInsertRows(QModelIndex(), m_videos.size(), m_videos.size());
    m_videos << video;
    m_search << buildSearchText(video);
    endInsertRows();
}

void VideoModel::remove(qint64 videoId)
{
    for (int i = 0; i < m_videos.size(); ++i) {
        if (m_videos[i].id != videoId)
            continue;
        beginRemoveRows(QModelIndex(), i, i);
        m_videos.removeAt(i);
        m_search.removeAt(i);
        endRemoveRows();
        return;
    }
}

// ---------------------------------------------------------------------------

VideoFilterModel::VideoFilterModel(VideoModel *source, QObject *parent)
    : QSortFilterProxyModel(parent)
    , m_source(source)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    setSourceModel(source);
    setDynamicSortFilter(true);
    sort(0);
    connect(this, &QAbstractItemModel::rowsInserted, this, &VideoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &VideoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &VideoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &VideoFilterModel::countChanged);
}

// Qt 6.10 replaced invalidateFilter() with an explicit begin/end pair.
void VideoFilterModel::beginFilterEdit()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
}

void VideoFilterModel::endFilterEdit()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

void VideoFilterModel::setSearchText(const QString &text)
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

void VideoFilterModel::setFacet(const QString &type, const QString &value)
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

void VideoFilterModel::setSortMode(const QString &mode)
{
    if (mode == m_sortMode)
        return;
    m_sortMode = mode;
    invalidate();
    emit sortModeChanged();
}

bool VideoFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &) const
{
    const VideoInfo &v = m_source->at(sourceRow);
    if (m_facetType == QLatin1String("albumArtist")) {
        if (!splitMulti(v.albumArtist).contains(m_facetValue))
            return false;
    } else if (m_facetType == QLatin1String("artist")) {
        if (!splitMulti(v.artist).contains(m_facetValue))
            return false;
    } else if (m_facetType == QLatin1String("genre")) {
        if (!splitMulti(v.genre).contains(m_facetValue))
            return false;
    } else if (m_facetType == QLatin1String("album")) {
        if (v.album != m_facetValue)
            return false;
    } else if (m_facetType == QLatin1String("year")) {
        if (QString::number(v.year) != m_facetValue)
            return false;
    }
    const QString &text = m_source->searchText(sourceRow);
    for (const QString &term : m_terms) {
        if (!text.contains(term))
            return false;
    }
    return true;
}

bool VideoFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const VideoInfo &a = m_source->at(left.row());
    const VideoInfo &b = m_source->at(right.row());
    const QString mode = m_facetType == QLatin1String("recent") ? QStringLiteral("added") : m_sortMode;

    if (mode == QLatin1String("added")) {
        if (a.addedAt != b.addedAt)
            return a.addedAt > b.addedAt;
        return a.id > b.id;
    }
    if (mode == QLatin1String("year") && a.year != b.year)
        return a.year > b.year;
    if (mode == QLatin1String("artist")) {
        const int c = m_collator.compare(a.albumArtist, b.albumArtist);
        if (c != 0)
            return c < 0;
        if (a.year != b.year)
            return a.year < b.year;
    }
    const int c = m_collator.compare(a.title, b.title);
    if (c != 0)
        return c < 0;
    return a.id < b.id;
}

QVariantMap VideoFilterModel::get(int row) const
{
    if (row < 0 || row >= rowCount())
        return {};
    return m_source->toMap(mapToSource(index(row, 0)).row());
}

QVariantList VideoFilterModel::snapshot() const
{
    QVariantList list;
    const int n = rowCount();
    list.reserve(n);
    for (int i = 0; i < n; ++i)
        list << m_source->toMap(mapToSource(index(i, 0)).row());
    return list;
}

int VideoFilterModel::rowOfVideo(qint64 videoId) const
{
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        if (m_source->at(mapToSource(index(i, 0)).row()).id == videoId)
            return i;
    }
    return -1;
}
