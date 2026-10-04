#include "core/Matcher.h"

#include "core/Util.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

// Folded text with punctuation collapsed to single spaces and padded with a
// space at each end, so " word " lookups match whole words only.
QString tokens(const QString &s)
{
    const QString f = foldText(s);
    QString out;
    out.reserve(f.size() + 2);
    out += QLatin1Char(' ');
    // Scripts written without spaces run straight into Latin words
    // ("東京Remix"); split where the script changes so "remix" stays a word.
    bool prevWide = false, first = true;
    for (const QChar c : f) {
        if (!c.isLetterOrNumber()) {
            out += QLatin1Char(' ');
            first = true;
            continue;
        }
        const bool wide = c.unicode() >= 0x2E80;
        if (!first && wide != prevWide)
            out += QLatin1Char(' ');
        out += c;
        prevWide = wide;
        first = false;
    }
    out += QLatin1Char(' ');
    return out.simplified().prepend(QLatin1Char(' ')).append(QLatin1Char(' '));
}

bool hasWord(const QString &tok, const QString &word)
{
    return tok.contains(QLatin1Char(' ') + word + QLatin1Char(' '));
}

// Whole-word match for space-delimited scripts, substring match for CJK.
bool hasTerm(const QString &tok, const QString &term)
{
    const bool cjk = std::any_of(term.begin(), term.end(), [](QChar c) { return c.unicode() >= 0x2E80; });
    return cjk ? tok.contains(term) : hasWord(tok, term);
}

// Versions of a song that are not "the music video". A result carrying one of
// these is discarded unless the track itself is that kind of version.
const QStringList &versionTerms()
{
    static const QStringList t = {
        QStringLiteral("cover"), QStringLiteral("covered"), QStringLiteral("piano"), QStringLiteral("guitar"),
        QStringLiteral("drum"), QStringLiteral("drums"), QStringLiteral("remix"), QStringLiteral("nightcore"),
        QStringLiteral("sped"), QStringLiteral("slowed"), QStringLiteral("reverb"), QStringLiteral("8d"),
        QStringLiteral("reaction"), QStringLiteral("reacts"), QStringLiteral("react"), QStringLiteral("reacting"),
        QStringLiteral("karaoke"), QStringLiteral("instrumental"),
        QStringLiteral("inst"), QStringLiteral("off vocal"), QStringLiteral("offvocal"),
        QStringLiteral("acoustic"), QStringLiteral("live"), QStringLiteral("teaser"), QStringLiteral("trailer"),
        QStringLiteral("preview"), QStringLiteral("1 hour"), QStringLiteral("1hour"), QStringLiteral("loop"),
        QStringLiteral("amv"), QStringLiteral("mmd"), QStringLiteral("sync"), QStringLiteral("tutorial"),
        QStringLiteral("mashup"), QStringLiteral("medley"), QStringLiteral("crossfade"), QStringLiteral("xfd"),
        QStringLiteral("full album"), QStringLiteral("dance practice"), QStringLiteral("behind the scenes"),
        QStringLiteral("fancam"), QStringLiteral("concert"), QStringLiteral("shorts"),
        QStringLiteral("歌ってみた"), QStringLiteral("弾いてみた"), QStringLiteral("叩いてみた"),
        QStringLiteral("踊ってみた"), QStringLiteral("カラオケ"), QStringLiteral("ライブ"),
        QStringLiteral("カバー"), QStringLiteral("ピアノ"), QStringLiteral("リアクション"),
        QStringLiteral("耐久"), QStringLiteral("メイキング"), QStringLiteral("予告"),
        QStringLiteral("ティザー"), QStringLiteral("クロスフェード"), QStringLiteral("試聴"),
    };
    return t;
}

// Signs of a fan re-upload.
const QStringList &fanTerms()
{
    static const QStringList t = {
        QStringLiteral("lyrics"), QStringLiteral("lyric"), QStringLiteral("romaji"), QStringLiteral("sub"),
        QStringLiteral("subs"), QStringLiteral("subtitle"), QStringLiteral("subtitles"),
        QStringLiteral("vietsub"), QStringLiteral("engsub"), QStringLiteral("español"),
        QStringLiteral("歌詞"), QStringLiteral("翻译"), QStringLiteral("翻譯"), QStringLiteral("中日"),
        QStringLiteral("中字"), QStringLiteral("字幕"), QStringLiteral("和訳"), QStringLiteral("高音質"),
        // Korean and Thai subtitle uploads: lyrics, subtitles, translation, pronunciation
        QStringLiteral("가사"), QStringLiteral("자막"), QStringLiteral("해석"), QStringLiteral("발음"),
        QStringLiteral("번역"), QStringLiteral("한글"), QStringLiteral("ซับไทย"),
        // fan-made and compiled videos
        QStringLiteral("創作"), QStringLiteral("自制"), QStringLiteral("自製"), QStringLiteral("compiled"),
        QStringLiteral("fanmade"), QStringLiteral("fan made"), QStringLiteral("fan mv"),
    };
    return t;
}

const QStringList &officialTerms()
{
    static const QStringList t = {
        QStringLiteral("mv"), QStringLiteral("m v"), QStringLiteral("music video"),
        QStringLiteral("official video"), QStringLiteral("official music video"), QStringLiteral("official"),
        QStringLiteral("pv"), QStringLiteral("公式"), QStringLiteral("ミュージックビデオ"),
    };
    return t;
}

// Titles like "Song (Instrumental)" describe an audio-only variant.
const QStringList &nonMvTrackTerms()
{
    static const QStringList t = {
        QStringLiteral("instrumental"), QStringLiteral("inst"), QStringLiteral("off vocal"),
        QStringLiteral("offvocal"), QStringLiteral("karaoke"), QStringLiteral("カラオケ"),
        QStringLiteral("backing track"), QStringLiteral("tv size"),
    };
    return t;
}

QStringList artistNames(const TrackInfo &t)
{
    QStringList names = splitMulti(t.albumArtist);
    for (const QString &a : splitMulti(t.artist)) {
        if (!names.contains(a))
            names << a;
    }
    return names;
}

} // namespace

bool Matcher::isNonMvTrack(const TrackInfo &track, QString *why)
{
    const QString tok = tokens(track.title);
    for (const QString &term : nonMvTrackTerms()) {
        if (hasTerm(tok, term)) {
            if (why)
                *why = QStringLiteral("“%1” version").arg(term);
            return true;
        }
    }
    return false;
}

QStringList Matcher::searchQueries(const TrackInfo &track)
{
    // Tried in order until one yields a usable result. The variants matter:
    // YouTube answers some plain "artist title" queries with an empty page
    // yet finds the same video once "music video" is added.
    const QString artist = artistNames(track).value(0);
    const QString base = QStringLiteral("%1 %2").arg(artist, track.title).simplified();
    return {
        base,
        base + QStringLiteral(" MV"),
        base + QStringLiteral(" music video"),
        base + QStringLiteral(" official video"),
    };
}

bool Matcher::sameVersion(const TrackInfo &track, const QString &videoTitle)
{
    const QString video = tokens(videoTitle);
    const QString title = tokens(track.title), album = tokens(track.album);
    for (const QString &term : versionTerms()) {
        if (hasTerm(video, term) != (hasTerm(title, term) || hasTerm(album, term)))
            return false;
    }
    // "(English Ver.)", "(Prayer Ver.)", "Rap version": a named version on
    // one side has to be named on the other.
    const auto named = [](const QString &tok) {
        QSet<QString> out;
        static const QRegularExpression re(QStringLiteral("(\\S+) (?:ver|version)(?= )"));
        auto it = re.globalMatch(tok);
        while (it.hasNext())
            out.insert(it.next().captured(1));
        return out;
    };
    return named(video) == (named(title) | named(album));
}

void Matcher::rank(const TrackInfo &track, QVector<YtCandidate> &candidates)
{
    const QString titleTok = tokens(track.title);
    const QString albumTok = tokens(track.album);
    const QStringList names = artistNames(track);
    const int total = candidates.size();

    for (YtCandidate &c : candidates) {
        const QString ct = tokens(c.title);
        const QString ch = tokens(c.channel);
        c.score = 0;
        c.trusted = false;
        c.rejectReason.clear();

        // Auto-generated "Artist - Topic" uploads are a still image over the audio.
        if (hasWord(ch, QStringLiteral("topic")) && c.channel.trimmed().endsWith(QStringLiteral("- Topic"))) {
            c.rejectReason = QStringLiteral("auto-generated art track");
            continue;
        }
        if (track.duration > 0 && c.duration > 0) {
            const double ratio = c.duration / track.duration;
            if (ratio < 0.3 || ratio > 3.0 || c.duration > track.duration + 900) {
                c.rejectReason = QStringLiteral("duration mismatch");
                continue;
            }
        }
        for (const QString &term : versionTerms()) {
            if (hasTerm(ct, term) && !hasTerm(titleTok, term) && !hasTerm(albumTok, term)) {
                c.rejectReason = QStringLiteral("“%1” version").arg(term);
                break;
            }
        }
        if (!c.rejectReason.isEmpty())
            continue;

        // Title
        const QString bareTitle = titleTok.trimmed();
        if (!bareTitle.isEmpty() && ct.contains(bareTitle)) {
            c.score += 40;
        } else {
            const QStringList words = bareTitle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            int hit = 0;
            for (const QString &w : words)
                hit += hasWord(ct, w) ? 1 : 0;
            if (!words.isEmpty())
                c.score += 30.0 * hit / words.size();
        }

        // Artist
        bool artistInTitle = false, artistIsChannel = false;
        for (const QString &n : names) {
            const QString nt = tokens(n).trimmed();
            if (nt.size() < 2)
                continue;
            artistInTitle |= ct.contains(nt);
            artistIsChannel |= ch.contains(nt);
        }
        if (artistInTitle || artistIsChannel)
            c.score += 20;
        if (artistIsChannel)
            c.score += 15;
        if (c.verified)
            c.score += 25;

        bool official = false;
        for (const QString &term : officialTerms())
            official |= hasTerm(ct, term);
        if (official)
            c.score += 10;

        bool fan = false;
        for (const QString &term : fanTerms())
            fan |= hasTerm(ct, term);
        if (fan)
            c.score -= 25;

        if (hasWord(ct, QStringLiteral("short ver")) || hasWord(ct, QStringLiteral("short version")))
            c.score -= 15;

        if (track.duration > 0 && c.duration > 0) {
            const double diff = qAbs(c.duration - track.duration);
            if (diff <= 6)
                c.score += 15;
            else if (diff <= 30)
                c.score += 8;
            else if (diff > 120)
                c.score -= 5;
        }

        // YouTube's own relevance order is a useful tie-breaker.
        c.score += qMax(0, total - c.rank);

        // The artist's own channel is trusted whatever the title says. A
        // verified channel is not on its own: fan channels with subtitled
        // re-uploads are verified too.
        c.trusted = artistIsChannel || ((c.verified || official) && !fan);
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const YtCandidate &a, const YtCandidate &b) {
        if (a.rejectReason.isEmpty() != b.rejectReason.isEmpty())
            return a.rejectReason.isEmpty();
        return a.score > b.score;
    });
}
