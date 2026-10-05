#include "core/Subtitles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QVector>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

namespace {

// A "pen" of the format: how a run of text looks.
struct Pen {
    bool bold = false, italic = false, underline = false;
    QString color;        // "RRGGBB", empty: the default
    int opacity = 255;    // of the text, 0..255
    QString edgeColor;
    int edge = 0;         // 0 none, 1 hard shadow, 2 bevel, 3 glow, 4 soft shadow
    int size = 100;       // percent, on YouTube's own scale
    bool plain() const
    {
        return !bold && !italic && !underline && color.isEmpty() && opacity >= 250 && edgeColor.isEmpty() && edge == 0
            && size == 100;
    }
};

// Where a line sits: an anchor point (0 top left .. 8 bottom right) at a
// position in percent of the picture.
struct Place {
    int anchor = 7;
    int x = 50, y = 100;
    bool set = false;
};

struct Run {
    int pen = -1;
    QString text;
};

struct Line {
    qint64 start = 0, end = 0; // ms
    int pen = -1;
    int place = -1;
    QVector<Run> runs;
};

QString rgb(const QString &hash)
{
    QString c = hash.trimmed();
    if (c.startsWith(QLatin1Char('#')))
        c.remove(0, 1);
    return c.size() == 6 ? c.toUpper() : QString();
}

QString srtTime(qint64 ms)
{
    return QStringLiteral("%1:%2:%3,%4")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString assTime(qint64 ms)
{
    return QStringLiteral("%1:%2:%3.%4")
        .arg(ms / 3600000)
        .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000 / 10, 2, 10, QLatin1Char('0'));
}

// ASS writes colours as BBGGRR.
QString assColor(const QString &rrggbb)
{
    return rrggbb.mid(4, 2) + rrggbb.mid(2, 2) + rrggbb.mid(0, 2);
}

constexpr int kResX = 1280, kResY = 720, kFontSize = 40;

QString assTags(const Pen &p)
{
    QString t;
    if (p.bold)
        t += QStringLiteral("\\b1");
    if (p.italic)
        t += QStringLiteral("\\i1");
    if (p.underline)
        t += QStringLiteral("\\u1");
    if (!p.color.isEmpty())
        t += QStringLiteral("\\c&H%1&").arg(assColor(p.color));
    if (p.opacity < 250)
        t += QStringLiteral("\\1a&H%1&").arg(QStringLiteral("%1").arg(255 - p.opacity, 2, 16, QLatin1Char('0')).toUpper());
    if (p.size != 100) // YouTube scales sizes down: 200 is half as large again, not double
        t += QStringLiteral("\\fs%1").arg(qRound(kFontSize * (1.0 + (p.size - 100) / 400.0)));
    if (!p.edgeColor.isEmpty())
        t += QStringLiteral("\\3c&H%1&\\4c&H%1&").arg(assColor(p.edgeColor));
    if (p.edge == 1 || p.edge == 4)
        t += QStringLiteral("\\bord0\\shad2");
    else if (p.edge == 3)
        t += QStringLiteral("\\bord2\\blur2");
    return t;
}

QString assText(QString s)
{
    s.replace(QLatin1Char('{'), QStringLiteral("("));
    s.replace(QLatin1Char('}'), QStringLiteral(")"));
    s.replace(QLatin1Char('\n'), QStringLiteral("\\N"));
    return s;
}

} // namespace

namespace Subtitles {

QString convertSrv3(const QString &srv3File, const QString &outBase, QString *error)
{
    QFile in(srv3File);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1").arg(srv3File);
        return {};
    }
    QHash<int, Pen> pens;
    QHash<int, Place> places;
    QVector<Line> lines;

    QXmlStreamReader xml(&in);
    Line *open = nullptr;
    int runPen = -1;
    bool inRun = false;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            const auto name = xml.name();
            const QXmlStreamAttributes a = xml.attributes();
            if (name == QLatin1String("pen")) {
                Pen p;
                p.bold = a.value(QLatin1String("b")) == QLatin1String("1");
                p.italic = a.value(QLatin1String("i")) == QLatin1String("1");
                p.underline = a.value(QLatin1String("u")) == QLatin1String("1");
                p.color = rgb(a.value(QLatin1String("fc")).toString());
                if (p.color == QLatin1String("FFFFFF"))
                    p.color.clear();
                if (a.hasAttribute(QLatin1String("fo")))
                    p.opacity = a.value(QLatin1String("fo")).toInt();
                p.edgeColor = rgb(a.value(QLatin1String("ec")).toString());
                p.edge = a.value(QLatin1String("et")).toInt();
                if (a.hasAttribute(QLatin1String("sz")))
                    p.size = a.value(QLatin1String("sz")).toInt();
                pens.insert(a.value(QLatin1String("id")).toInt(), p);
            } else if (name == QLatin1String("wp")) {
                Place w;
                w.set = true;
                if (a.hasAttribute(QLatin1String("ap")))
                    w.anchor = std::clamp(a.value(QLatin1String("ap")).toInt(), 0, 8);
                w.x = a.hasAttribute(QLatin1String("ah")) ? a.value(QLatin1String("ah")).toInt() : 50;
                w.y = a.hasAttribute(QLatin1String("av")) ? a.value(QLatin1String("av")).toInt() : 100;
                places.insert(a.value(QLatin1String("id")).toInt(), w);
            } else if (name == QLatin1String("p")) {
                Line l;
                l.start = a.value(QLatin1String("t")).toLongLong();
                l.end = l.start + a.value(QLatin1String("d")).toLongLong();
                l.pen = a.hasAttribute(QLatin1String("p")) ? a.value(QLatin1String("p")).toInt() : -1;
                l.place = a.hasAttribute(QLatin1String("wp")) ? a.value(QLatin1String("wp")).toInt() : -1;
                lines.append(l);
                open = &lines.last();
            } else if (name == QLatin1String("s") && open) {
                inRun = true;
                runPen = a.hasAttribute(QLatin1String("p")) ? a.value(QLatin1String("p")).toInt() : open->pen;
            }
            break;
        }
        case QXmlStreamReader::Characters:
            if (open && !xml.text().isEmpty())
                open->runs.append({inRun ? runPen : open->pen, xml.text().toString()});
            break;
        case QXmlStreamReader::EndElement:
            if (xml.name() == QLatin1String("s"))
                inRun = false;
            else if (xml.name() == QLatin1String("p"))
                open = nullptr;
            break;
        default:
            break;
        }
    }
    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("not YouTube's subtitle format: %1").arg(xml.errorString());
        return {};
    }

    // Lines of nothing but spaces are how the format clears the screen.
    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const Line &l) {
        QString all;
        for (const Run &r : l.runs)
            all += r.text;
        return all.trimmed().isEmpty() || l.end <= l.start;
    }), lines.end());
    if (lines.isEmpty())
        return {};
    std::stable_sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) { return a.start < b.start; });

    // Styled: any text that is not in the default look, or not at the bottom.
    bool styled = false;
    for (const Line &l : std::as_const(lines)) {
        const Place w = places.value(l.place);
        if (w.set && !(w.anchor == 7 && w.y >= 85 && std::abs(w.x - 50) <= 5))
            styled = true;
        for (const Run &r : l.runs) {
            if (r.pen >= 0 && pens.contains(r.pen) && !pens.value(r.pen).plain())
                styled = true;
        }
    }

    QByteArray out;
    if (!styled) {
        int n = 0;
        for (const Line &l : std::as_const(lines)) {
            QString text;
            for (const Run &r : l.runs)
                text += r.text;
            out += QStringLiteral("%1\n%2 --> %3\n%4\n\n").arg(++n).arg(srtTime(l.start), srtTime(l.end), text.trimmed()).toUtf8();
        }
    } else {
        out += QStringLiteral("[Script Info]\nScriptType: v4.00+\nPlayResX: %1\nPlayResY: %2\nWrapStyle: 0\n"
                              "ScaledBorderAndShadow: yes\n\n[V4+ Styles]\n"
                              "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
                              "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
                              "Alignment, MarginL, MarginR, MarginV, Encoding\n"
                              "Style: Default,sans-serif,%3,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,"
                              "2,40,40,34,1\n\n[Events]\n"
                              "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n")
                   .arg(kResX).arg(kResY).arg(kFontSize).toUtf8();
        // The format's anchors run top left to bottom right; ASS numbers
        // its alignments like a keypad.
        static const int align[9] = {7, 8, 9, 4, 5, 6, 1, 2, 3};
        for (const Line &l : std::as_const(lines)) {
            QString text;
            const Place w = places.value(l.place);
            if (w.set) {
                // YouTube keeps a margin of 2% around the picture.
                text += QStringLiteral("{\\an%1\\pos(%2,%3)}")
                            .arg(align[w.anchor])
                            .arg(qRound(kResX * (0.02 + 0.96 * w.x / 100.0)))
                            .arg(qRound(kResY * (0.02 + 0.96 * w.y / 100.0)));
            }
            for (const Run &r : l.runs) {
                const QString tags = r.pen >= 0 ? assTags(pens.value(r.pen)) : QString();
                text += QStringLiteral("{\\r%1}").arg(tags) + assText(r.text);
            }
            out += QStringLiteral("Dialogue: 0,%1,%2,Default,,0,0,0,,%3\n").arg(assTime(l.start), assTime(l.end), text).toUtf8();
        }
    }

    const QString file = outBase + (styled ? QStringLiteral(".ass") : QStringLiteral(".srt"));
    QSaveFile save(file);
    if (!save.open(QIODevice::WriteOnly) || save.write(out) < 0 || !save.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(file);
        return {};
    }
    // One file per language: not the other kind from an earlier time.
    QFile::remove(outBase + (styled ? QStringLiteral(".srt") : QStringLiteral(".ass")));
    return file;
}

QString pickLanguage(const QStringList &available, const QString &wanted)
{
    QStringList sorted = available;
    sorted.sort();
    for (const QString &l : std::as_const(sorted)) {
        if (l.compare(wanted, Qt::CaseInsensitive) == 0)
            return l;
    }
    // A regional or labelled variant: "en-GB", "en-AAj-uoGhMZA".
    for (const QString &l : std::as_const(sorted)) {
        if (l.startsWith(wanted + QLatin1Char('-'), Qt::CaseInsensitive))
            return l;
    }
    return {};
}

QString sidecarBase(const QString &videoPath, const QString &language)
{
    const QFileInfo fi(videoPath);
    return fi.absoluteDir().filePath(fi.completeBaseName()) + QLatin1Char('.') + language;
}

QStringList sidecars(const QString &videoPath)
{
    const QFileInfo fi(videoPath);
    const QString stem = fi.completeBaseName() + QLatin1Char('.');
    QStringList out;
    // Not a name filter: titles hold brackets, which those read as sets.
    const QFileInfoList entries = fi.absoluteDir().entryInfoList(QDir::Files);
    for (const QFileInfo &e : entries) {
        const QString name = e.fileName();
        if (name.startsWith(stem) && (name.endsWith(QLatin1String(".srt")) || name.endsWith(QLatin1String(".ass"))))
            out << e.absoluteFilePath();
    }
    return out;
}

bool hasSidecar(const QString &videoPath, const QString &language)
{
    const QString base = sidecarBase(videoPath, language);
    return QFile::exists(base + QStringLiteral(".srt")) || QFile::exists(base + QStringLiteral(".ass"));
}

} // namespace Subtitles
