#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QStringList>

// The folders under the base directory, as a tree the user ticks folders in:
// one row per folder on show, children listed when a folder is opened.
//
// What is ticked is the list of music folders itself (`checked`): a folder is
// checked when it or an ancestor is in the list, partly checked when a
// descendant is, and the list is kept minimal (no folder under another).
// Ticking and unticking go through withChecked(), which keeps it that way.
class FolderTreeModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString root READ root WRITE setRoot NOTIFY rootChanged)
    Q_PROPERTY(QStringList checked READ checked WRITE setChecked NOTIFY checkedChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Role { PathRole = Qt::UserRole + 1, NameRole, DepthRole, ExpandedRole, HasChildrenRole, StateRole };
    enum State { Unchecked = 0, Partial = 1, Checked = 2 };
    Q_ENUM(State)

    explicit FolderTreeModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString root() const { return m_root; }
    void setRoot(const QString &root);
    QStringList checked() const { return m_checked; }
    void setChecked(const QStringList &dirs);

    Q_INVOKABLE void toggleExpanded(int row);
    Q_INVOKABLE void setExpanded(int row, bool expanded);
    // The row of a folder on show, or -1.
    Q_INVOKABLE int rowOf(const QString &path) const;
    // Reads every open folder again, for folders made or removed meanwhile.
    Q_INVOKABLE void refresh();
    // The state a folder would show: Checked, Partial or Unchecked.
    static State stateOf(const QString &path, const QStringList &checked);

    // The music folders after `path` is ticked (`on`) or unticked. Ticking a
    // folder takes the place of every folder under it; unticking one under a
    // ticked ancestor replaces the ancestor by its other branches, read from
    // disk, so that only `path` goes.
    static QStringList withChecked(const QStringList &dirs, const QString &path, bool on);
    // The deepest folder that holds every one of `dirs` (a folder holds itself).
    static QString commonAncestor(const QStringList &dirs);
    static bool isWithin(const QString &path, const QString &ancestor); // or the same

signals:
    void rootChanged();
    void checkedChanged();
    void countChanged();

private:
    struct Node {
        QString path;
        QString name;
        int depth = 0;
        bool expanded = false;
        bool hasChildren = false;
    };
    static QStringList subfolders(const QString &path);
    static bool hasSubfolders(const QString &path);
    QVector<Node> children(const Node &parent) const;
    void rebuild();
    void insertChildren(int row);
    void removeChildren(int row);

    QString m_root;
    QStringList m_checked;
    QSet<QString> m_expanded;
    QVector<Node> m_rows;
};
