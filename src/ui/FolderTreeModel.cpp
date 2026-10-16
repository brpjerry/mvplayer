#include "ui/FolderTreeModel.h"

#include "core/Util.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <algorithm>

namespace {

QString clean(const QString &path)
{
    return QDir::cleanPath(path);
}

// "/a/b" -> "/a"; "/" -> "/" (its own parent, so walks up stop there).
QString parentOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return path;
    if (slash == 0)
        return QStringLiteral("/");
    const QString parent = path.left(slash);
    // "C:" is the drive "C:/".
    return parent.endsWith(QLatin1Char(':')) ? parent + QLatin1Char('/') : parent;
}

QString nameOf(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? path : name;
}

bool samePath(const QString &a, const QString &b)
{
    return a.compare(b, pathCase) == 0;
}

void sortUnique(QStringList &dirs)
{
    std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) { return a.compare(b, pathCase) < 0; });
    dirs.erase(std::unique(dirs.begin(), dirs.end(), samePath), dirs.end());
}

} // namespace

FolderTreeModel::FolderTreeModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FolderTreeModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant FolderTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Node &n = m_rows.at(index.row());
    switch (role) {
    case PathRole: return n.path;
    case NameRole: return n.name;
    case DepthRole: return n.depth;
    case ExpandedRole: return n.expanded;
    case HasChildrenRole: return n.hasChildren;
    case StateRole: return int(stateOf(n.path, m_checked));
    }
    return {};
}

QHash<int, QByteArray> FolderTreeModel::roleNames() const
{
    return {
        {PathRole, "path"}, {NameRole, "name"}, {DepthRole, "depth"},
        {ExpandedRole, "expanded"}, {HasChildrenRole, "hasChildren"}, {StateRole, "checkState"},
    };
}

bool FolderTreeModel::isWithin(const QString &path, const QString &ancestor)
{
    if (ancestor.isEmpty())
        return false;
    if (samePath(path, ancestor))
        return true;
    const QString prefix = ancestor.endsWith(QLatin1Char('/')) ? ancestor : ancestor + QLatin1Char('/');
    return path.startsWith(prefix, pathCase);
}

FolderTreeModel::State FolderTreeModel::stateOf(const QString &path, const QStringList &checked)
{
    for (const QString &d : checked) {
        if (isWithin(path, d))
            return Checked;
    }
    for (const QString &d : checked) {
        if (isWithin(d, path))
            return Partial;
    }
    return Unchecked;
}

QString FolderTreeModel::commonAncestor(const QStringList &dirs)
{
    if (dirs.isEmpty())
        return {};
    QStringList common = clean(dirs.first()).split(QLatin1Char('/'));
    for (const QString &d : dirs.mid(1)) {
        const QStringList parts = clean(d).split(QLatin1Char('/'));
        int n = 0;
        while (n < common.size() && n < parts.size() && samePath(common.at(n), parts.at(n)))
            ++n;
        common = common.mid(0, n);
    }
    if (common.isEmpty())
        return {}; // different drives
    QString path = common.join(QLatin1Char('/'));
    if (path.isEmpty() || path.endsWith(QLatin1Char(':')))
        path += QLatin1Char('/');
    return path;
}

QStringList FolderTreeModel::subfolders(const QString &path)
{
    QStringList names = QDir(path).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&collator](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    const QString prefix = path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/');
    QStringList out;
    out.reserve(names.size());
    for (const QString &n : names)
        out << prefix + n;
    return out;
}

bool FolderTreeModel::hasSubfolders(const QString &path)
{
    QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot);
    return it.hasNext();
}

QStringList FolderTreeModel::withChecked(const QStringList &dirs, const QString &rawPath, bool on)
{
    const QString path = clean(rawPath);
    QStringList out;
    QString ancestor;
    for (const QString &d : dirs) {
        if (isWithin(d, path))
            continue; // the folder itself and everything under it go either way
        if (isWithin(path, d))
            ancestor = d;
        out << d;
    }
    if (on) {
        if (ancestor.isEmpty())
            out << path;
    } else if (!ancestor.isEmpty()) {
        // The ancestor goes; what it held apart from `path` stays ticked.
        out.removeAll(ancestor);
        for (QString cur = path; !samePath(cur, ancestor);) {
            const QString parent = parentOf(cur);
            if (samePath(parent, cur))
                break;
            for (const QString &sib : subfolders(parent)) {
                if (!samePath(sib, cur))
                    out << sib;
            }
            cur = parent;
        }
    }
    sortUnique(out);
    return out;
}

void FolderTreeModel::setRoot(const QString &rawRoot)
{
    const QString root = rawRoot.isEmpty() ? QString() : clean(rawRoot);
    if (samePath(root, m_root))
        return;
    m_root = root;
    m_expanded.clear();
    if (!m_root.isEmpty())
        m_expanded.insert(m_root);
    // The ticked folders are on show from the start.
    for (const QString &d : m_checked) {
        for (QString p = parentOf(d); isWithin(p, m_root); p = parentOf(p)) {
            m_expanded.insert(p);
            if (samePath(p, m_root) || samePath(parentOf(p), p))
                break;
        }
    }
    rebuild();
    emit rootChanged();
}

void FolderTreeModel::setChecked(const QStringList &dirs)
{
    QStringList next;
    for (const QString &d : dirs)
        next << clean(d);
    if (next == m_checked)
        return;
    m_checked = next;
    // A folder ticked out of sight (from the command line) is brought on show.
    bool opened = false;
    for (const QString &d : m_checked) {
        if (!isWithin(d, m_root) || rowOf(d) >= 0 || samePath(d, m_root))
            continue;
        for (QString p = parentOf(d); isWithin(p, m_root); p = parentOf(p)) {
            opened |= !m_expanded.contains(p);
            m_expanded.insert(p);
            if (samePath(p, m_root) || samePath(parentOf(p), p))
                break;
        }
    }
    if (opened)
        rebuild();
    else if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1), {StateRole});
    emit checkedChanged();
}

int FolderTreeModel::rowOf(const QString &path) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (samePath(m_rows.at(i).path, path))
            return i;
    }
    return -1;
}

QVector<FolderTreeModel::Node> FolderTreeModel::children(const Node &parent) const
{
    QVector<Node> out;
    for (const QString &p : subfolders(parent.path)) {
        Node n;
        n.path = p;
        n.name = nameOf(p);
        n.depth = parent.depth + 1;
        n.expanded = m_expanded.contains(p);
        n.hasChildren = hasSubfolders(p);
        out << n;
        if (n.expanded)
            out << children(n);
    }
    return out;
}

void FolderTreeModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    if (!m_root.isEmpty() && QDir(m_root).exists()) {
        Node top;
        top.path = m_root;
        top.name = m_root;
        top.depth = 0;
        top.expanded = m_expanded.contains(m_root);
        top.hasChildren = hasSubfolders(m_root);
        m_rows << top;
        if (top.expanded)
            m_rows << children(top);
    }
    endResetModel();
    emit countChanged();
}

void FolderTreeModel::refresh()
{
    rebuild();
}

void FolderTreeModel::toggleExpanded(int row)
{
    if (row >= 0 && row < m_rows.size())
        setExpanded(row, !m_rows.at(row).expanded);
}

void FolderTreeModel::setExpanded(int row, bool expanded)
{
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).expanded == expanded)
        return;
    if (expanded) {
        m_expanded.insert(m_rows.at(row).path);
        insertChildren(row);
    } else {
        m_expanded.remove(m_rows.at(row).path);
        removeChildren(row);
    }
}

void FolderTreeModel::insertChildren(int row)
{
    Node &parent = m_rows[row];
    parent.expanded = true;
    const QVector<Node> kids = children(parent);
    parent.hasChildren = !kids.isEmpty();
    emit dataChanged(index(row), index(row), {ExpandedRole, HasChildrenRole});
    if (kids.isEmpty())
        return;
    beginInsertRows({}, row + 1, row + kids.size());
    for (int i = 0; i < kids.size(); ++i)
        m_rows.insert(row + 1 + i, kids.at(i));
    endInsertRows();
    emit countChanged();
}

void FolderTreeModel::removeChildren(int row)
{
    Node &parent = m_rows[row];
    parent.expanded = false;
    emit dataChanged(index(row), index(row), {ExpandedRole});
    int last = row;
    while (last + 1 < m_rows.size() && m_rows.at(last + 1).depth > parent.depth)
        ++last;
    if (last == row)
        return;
    beginRemoveRows({}, row + 1, last);
    m_rows.remove(row + 1, last - row);
    endRemoveRows();
    emit countChanged();
}
