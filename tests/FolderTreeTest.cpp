#include "ui/FolderTreeModel.h"

#include <QtTest>
#include <QDir>
#include <QTemporaryDir>

// The folder tree behind Settings → Library: which rows a base shows, and
// what ticking and unticking a folder does to the list of music folders.
class FolderTreeTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_base;
    QString p(const QString &rel) const { return m_base + QLatin1Char('/') + rel; }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_base = QDir(m_tmp.path()).absolutePath() + QStringLiteral("/Music");
        for (const char *d : {"A/Album1", "A/Album2", "B", "C/Live/Disc1", "C/Studio"})
            QVERIFY(QDir().mkpath(p(QLatin1String(d))));
        // A hidden folder is not shown, a file is not a folder.
        QVERIFY(QDir().mkpath(p(QStringLiteral(".hidden"))));
        QFile f(p(QStringLiteral("B/track.flac")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
    }

    void within()
    {
        QVERIFY(FolderTreeModel::isWithin(QStringLiteral("/a/b"), QStringLiteral("/a")));
        QVERIFY(FolderTreeModel::isWithin(QStringLiteral("/a"), QStringLiteral("/a")));
        QVERIFY(FolderTreeModel::isWithin(QStringLiteral("/a/b"), QStringLiteral("/")));
        QVERIFY(!FolderTreeModel::isWithin(QStringLiteral("/ab"), QStringLiteral("/a")));
        QVERIFY(!FolderTreeModel::isWithin(QStringLiteral("/a"), QStringLiteral("/a/b")));
        QVERIFY(!FolderTreeModel::isWithin(QStringLiteral("/a"), QString()));
    }

    void ancestor()
    {
        QCOMPARE(FolderTreeModel::commonAncestor({QStringLiteral("/m/A"), QStringLiteral("/m/B/x")}), QStringLiteral("/m"));
        QCOMPARE(FolderTreeModel::commonAncestor({QStringLiteral("/m/A")}), QStringLiteral("/m/A"));
        QCOMPARE(FolderTreeModel::commonAncestor({QStringLiteral("/m/A"), QStringLiteral("/m/A/x")}), QStringLiteral("/m/A"));
        QCOMPARE(FolderTreeModel::commonAncestor({QStringLiteral("/m"), QStringLiteral("/n")}), QStringLiteral("/"));
        QCOMPARE(FolderTreeModel::commonAncestor({}), QString());
    }

    void states()
    {
        const QStringList checked{p(QStringLiteral("A")), p(QStringLiteral("C/Live"))};
        QCOMPARE(FolderTreeModel::stateOf(p(QStringLiteral("A")), checked), FolderTreeModel::Checked);
        QCOMPARE(FolderTreeModel::stateOf(p(QStringLiteral("A/Album1")), checked), FolderTreeModel::Checked);
        QCOMPARE(FolderTreeModel::stateOf(p(QStringLiteral("C")), checked), FolderTreeModel::Partial);
        QCOMPARE(FolderTreeModel::stateOf(m_base, checked), FolderTreeModel::Partial);
        QCOMPARE(FolderTreeModel::stateOf(p(QStringLiteral("B")), checked), FolderTreeModel::Unchecked);
        QCOMPARE(FolderTreeModel::stateOf(p(QStringLiteral("C/Studio")), checked), FolderTreeModel::Unchecked);
    }

    void ticking()
    {
        // Ticking a folder stands in for everything under it.
        QStringList dirs = FolderTreeModel::withChecked({}, p(QStringLiteral("A/Album1")), true);
        QCOMPARE(dirs, QStringList{p(QStringLiteral("A/Album1"))});
        dirs = FolderTreeModel::withChecked(dirs, p(QStringLiteral("A")), true);
        QCOMPARE(dirs, QStringList{p(QStringLiteral("A"))});
        // Under a ticked folder there is nothing to add.
        QCOMPARE(FolderTreeModel::withChecked(dirs, p(QStringLiteral("A/Album2")), true), dirs);
        // Ticking the base is the whole library in one.
        dirs = FolderTreeModel::withChecked(dirs, m_base, true);
        QCOMPARE(dirs, QStringList{m_base});
    }

    void unticking()
    {
        // Unticking a folder under a ticked ancestor keeps the ancestor's
        // other branches, down to the folder's own siblings.
        QStringList dirs = FolderTreeModel::withChecked({m_base}, p(QStringLiteral("C/Live/Disc1")), false);
        QCOMPARE(dirs, (QStringList{p(QStringLiteral("A")), p(QStringLiteral("B")), p(QStringLiteral("C/Studio"))}));
        QCOMPARE(FolderTreeModel::stateOf(p(QStringLiteral("C/Live")), dirs), FolderTreeModel::Unchecked);
        QCOMPARE(FolderTreeModel::stateOf(p(QStringLiteral("C")), dirs), FolderTreeModel::Partial);
        // Unticking a ticked folder itself just takes it out.
        dirs = FolderTreeModel::withChecked(dirs, p(QStringLiteral("B")), false);
        QCOMPARE(dirs, (QStringList{p(QStringLiteral("A")), p(QStringLiteral("C/Studio"))}));
        // Unticking what is not ticked changes nothing.
        QCOMPARE(FolderTreeModel::withChecked(dirs, p(QStringLiteral("B")), false), dirs);
        // Unticking the base clears all.
        QCOMPARE(FolderTreeModel::withChecked(dirs, m_base, false), QStringList{});
    }

    void rows()
    {
        FolderTreeModel model;
        model.setChecked({p(QStringLiteral("C/Live"))});
        model.setRoot(m_base);
        // The base, open, with its folders; the ticked one's ancestors are open too.
        QStringList names;
        for (int i = 0; i < model.rowCount(); ++i)
            names << QStringLiteral("%1:%2").arg(model.data(model.index(i), FolderTreeModel::DepthRole).toInt()).arg(model.data(model.index(i), FolderTreeModel::NameRole).toString());
        QCOMPARE(names, (QStringList{QStringLiteral("0:") + m_base, QStringLiteral("1:A"), QStringLiteral("1:B"), QStringLiteral("1:C"), QStringLiteral("2:Live"), QStringLiteral("2:Studio")}));
        QCOMPARE(model.data(model.index(2), FolderTreeModel::HasChildrenRole).toBool(), false); // B holds a file only
        QCOMPARE(model.data(model.index(3), FolderTreeModel::StateRole).toInt(), int(FolderTreeModel::Partial));
        QCOMPARE(model.data(model.index(4), FolderTreeModel::StateRole).toInt(), int(FolderTreeModel::Checked));

        // Opening and closing a folder.
        model.toggleExpanded(1);
        QCOMPARE(model.rowCount(), 8);
        QCOMPARE(model.data(model.index(2), FolderTreeModel::NameRole).toString(), QStringLiteral("Album1"));
        model.toggleExpanded(1);
        QCOMPARE(model.rowCount(), 6);
        // Closing C hides Live and Studio; opening it again brings them back.
        model.toggleExpanded(3);
        QCOMPARE(model.rowCount(), 4);
        model.toggleExpanded(3);
        QCOMPARE(model.rowCount(), 6);

        // A tick from elsewhere shows in the rows, and a folder ticked out
        // of sight is brought on show.
        model.setChecked({p(QStringLiteral("A/Album2"))});
        QVERIFY(model.rowOf(p(QStringLiteral("A/Album2"))) > 0);
        QCOMPARE(model.data(model.index(model.rowOf(p(QStringLiteral("C")))), FolderTreeModel::StateRole).toInt(), int(FolderTreeModel::Unchecked));

        // A new folder appears on refresh.
        QVERIFY(QDir().mkpath(p(QStringLiteral("D"))));
        model.refresh();
        QVERIFY(model.rowOf(p(QStringLiteral("D"))) > 0);

        // No base: nothing.
        model.setRoot(QString());
        QCOMPARE(model.rowCount(), 0);
    }
};

QTEST_GUILESS_MAIN(FolderTreeTest)
#include "FolderTreeTest.moc"
