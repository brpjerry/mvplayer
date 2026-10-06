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
        QStringLiteral("カバー"), QStringLiteral("ピアノ"), QStringLiteral("リアクション"), QStringLiteral("反応"), QStringLiteral("歌いました"), QStringLiteral("演奏してみた"), QStringLiteral("歌わせて"),
        // a game being played to the song
        QStringLiteral("beat saber"), QStringLiteral("project diva"), QStringLiteral("full combo"),
        QStringLiteral("perfect combo"), QStringLiteral("expertplus"), QStringLiteral("osu"), QStringLiteral("gameplay"),
        QStringLiteral("ハニプレ"), QStringLiteral("プレイ動画"), QStringLiteral("譜面"), QStringLiteral("フルコンボ"),
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
        QStringLiteral("unofficial"), QStringLiteral("非公式"),
        QStringLiteral("subs"), QStringLiteral("subtitle"), QStringLiteral("subtitles"),
        QStringLiteral("vietsub"), QStringLiteral("engsub"), QStringLiteral("español"),
        QStringLiteral("歌詞"), QStringLiteral("翻译"), QStringLiteral("翻譯"), QStringLiteral("翻訳"), QStringLiteral("中日"),
        QStringLiteral("中字"), QStringLiteral("字幕"), QStringLiteral("和訳"), QStringLiteral("高音質"),
        // Korean and Thai subtitle uploads: lyrics, subtitles, translation, pronunciation
        QStringLiteral("가사"), QStringLiteral("자막"), QStringLiteral("해석"), QStringLiteral("발음"),
        QStringLiteral("번역"), QStringLiteral("한글"), QStringLiteral("ซับไทย"),
        // a picture put to someone's song: "PV made for it", "drew it"
        QStringLiteral("つけてみた"), QStringLiteral("付けてみた"), QStringLiteral("描いてみた"), QStringLiteral("pv 風"), QStringLiteral("手描き"),
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

} // namespace

QStringList Matcher::artistNames(const TrackInfo &t)
{
    QStringList names = splitMulti(t.albumArtist);
    for (const QString &a : splitMulti(t.artist)) {
        if (!names.contains(a))
            names << a;
    }
    return names;
}

bool Matcher::isNonMvTrack(const TrackInfo &track, QString *why)
{
    // Nothing this short has a music video: a jingle, a skit, a few words.
    if (track.duration > 0 && track.duration < 30) {
        if (why)
            *why = QStringLiteral("%1-second track").arg(qRound(track.duration));
        return true;
    }
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

bool Matcher::isTalkTitle(const QString &title)
{
    // Folded: full-width letters and circled numbers become plain ones.
    QString t = title.normalized(QString::NormalizationForm_KC).toCaseFolded().simplified();
    // "～MC01～", "-MC-": the same, dressed up.
    static const QRegularExpression wrapper(QStringLiteral("^[~〜～\\-—–\\s]+|[~〜～\\-—–\\s]+$"));
    t.remove(wrapper);
    static const QString terms = QStringLiteral(
        "mc|talk|トーク|banter|stage banter|speech|interview|インタビュー|commentary|audio commentary|"
        "コメンタリー|voice drama|ボイスドラマ|メンバー紹介|band introductions?|encore call|applause|挨拶|ごあいさつ");
    // The term alone, numbered ("mc06", "mc 2", "talk #3"), with where it
    // was ("mc5 at ...") and with a tag after it ("mc7(live)").
    static const QRegularExpression whole(
        QStringLiteral("^(?:%1)\\s*[-#.:]?\\s*\\d{0,3}(?:\\s*(?:at|in|@|~|-)\\s*\\S.*)?(?:\\s*[(\\[【][^)\\]】]*[)\\]】])?$").arg(terms));
    // ... or as a tag after a title: "(mc)", "[interview]", "-talk-".
    static const QRegularExpression tag(
        QStringLiteral("\\S\\s*[(\\[【~-]\\s*(?:%1)\\s*\\d{0,3}\\s*[)\\]】~-]$").arg(terms));
    return whole.match(t).hasMatch() || tag.match(t).hasMatch();
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

bool Matcher::sameVersion(const TrackInfo &track, const QString &videoTitle, bool artistChannel)
{
    const QString video = tokens(videoTitle);
    const QString title = tokens(track.title), album = tokens(track.album);
    for (const QString &term : versionTerms()) {
        if (hasTerm(video, term) != (hasTerm(title, term) || hasTerm(album, term)))
            return false;
    }
    // "(English Ver.)", "(Prayer Ver.)", "Rap version": a version the video
    // names has to be named by the track, and one the track's title names
    // by the video. A version only the album is named after ("... -3 nuits
    // ver.-") is not every track's: the video need not name it.
    const auto named = [](const QString &tok) {
        QSet<QString> out;
        static const QRegularExpression re(QStringLiteral("(\\S+) (?:ver|version)(?= )"));
        auto it = re.globalMatch(tok);
        while (it.hasNext())
            out.insert(it.next().captured(1));
        // "Music Video Full ver." is the whole of it, not another one;
        // lengths are compared elsewhere.
        out.remove(QStringLiteral("full"));
        return out;
    };
    const QSet<QString> inVideo = named(video), inTitle = named(title);
    if (!(inTitle | named(album)).contains(inVideo) || !inVideo.contains(inTitle))
        return false;

    // "... feat. 初音ミク": sung by someone the track does not have. The
    // producer's own upload of a song is its other singer's version.
    // Likewise "… ／mona（CV：夏川椎菜）", "（Vo：…）": the voice is named.
    static const QRegularExpression feat(QStringLiteral(" (?:feat|ft|featuring|cv|vo|vocal) (\\S+)"));
    const QString whose = title + album + tokens(track.artist) + tokens(track.albumArtist);
    // The synthesised voices go by two names, and a tag has one of them.
    static const QVector<QStringList> voices = {
        {QStringLiteral("初音ミク"), QStringLiteral("hatsune miku"), QStringLiteral("miku")},
        {QStringLiteral("鏡音リン"), QStringLiteral("kagamine rin")},
        {QStringLiteral("鏡音レン"), QStringLiteral("kagamine len")},
        {QStringLiteral("巡音ルカ"), QStringLiteral("megurine luka"), QStringLiteral("luka")},
        {QStringLiteral("gumi"), QStringLiteral("グミ"), QStringLiteral("megpoid")},
        {QStringLiteral("可不"), QStringLiteral("kafu")},
        {QStringLiteral("重音テト"), QStringLiteral("kasane teto"), QStringLiteral("teto")},
        {QStringLiteral("裏命"), QStringLiteral("rime")},
        {QStringLiteral("星界"), QStringLiteral("sekai")},
        {QStringLiteral("狐子"), QStringLiteral("coko")},
        {QStringLiteral("羽累"), QStringLiteral("haru")},
        {QStringLiteral("結月ゆかり"), QStringLiteral("yuzuki yukari"), QStringLiteral("yukari")},
        {QStringLiteral("歌愛ユキ"), QStringLiteral("kaai yuki")},
        {QStringLiteral("ずんだもん"), QStringLiteral("zundamon")},
        {QStringLiteral("flower"), QStringLiteral("v flower"), QStringLiteral("フラワ")},
    };
    const auto knownAs = [&whose](const QString &singer) {
        for (const QStringList &names : voices) {
            const bool is = std::any_of(names.begin(), names.end(), [&singer](const QString &n) {
                return n == singer || n.startsWith(singer + QLatin1Char(' '));
            });
            if (is && std::any_of(names.begin(), names.end(), [&whose](const QString &n) { return whose.contains(n); }))
                return true;
        }
        return false;
    };
    // On the artist's own channel a synthesised voice is simply who sings
    // the artist's songs, credited or not in the tags. A human singer
    // credited there is still another version (the producer's channel).
    const auto synthesised = [](const QString &singer) {
        for (const QStringList &names : voices) {
            if (std::any_of(names.begin(), names.end(), [&singer](const QString &n) { return n == singer || n.startsWith(singer + QLatin1Char(' ')); }))
                return true;
        }
        return false;
    };
    auto it = feat.globalMatch(video);
    while (it.hasNext()) {
        const QString singer = it.next().captured(1);
        if (singer.size() >= 2 && !whose.contains(singer) && !knownAs(singer) && !(artistChannel && synthesised(singer)))
            return false;
    }
    return true;
}

bool Matcher::isOwnChannel(const TrackInfo &track, const QString &channel)
{
    QString rest = tokens(channel);
    bool named = false;
    // "Producer feat. Singer": the producer's channel is the artist's.
    static const QRegularExpression parts(QStringLiteral("[()（）\\[\\]【】/／&＆、,;]+|\\s*\\b(?:feat|ft|featuring)\\.?\\s*"));
    for (const QString &n : Matcher::artistNames(track)) {
        QStringList forms = n.split(parts, Qt::SkipEmptyParts);
        forms.prepend(n);
        for (const QString &form : std::as_const(forms)) {
            const QString nt = tokens(form).trimmed();
            if (nt.size() >= 2 && rest.contains(nt)) {
                named = true;
                rest.replace(nt, QStringLiteral(" "));
            }
        }
    }
    if (!named)
        return false;
    // Nothing beside the name but what any artist's channel is called.
    static const QRegularExpression generic(QStringLiteral(
        " (?:official|channel|music|youtube|vevo|tv|records|公式|チャンネル|オフィシャル|[a-z]|\\d+)(?= )"));
    rest = rest.simplified().prepend(QLatin1Char(' ')).append(QLatin1Char(' '));
    while (rest.contains(generic))
        rest.replace(generic, QString());
    return rest.trimmed().isEmpty();
}

bool Matcher::namesArtist(const TrackInfo &track, const QString &videoTitle, const QString &channel)
{
    const QString ct = tokens(videoTitle), ch = tokens(channel);
    // "Producer feat. Singer": the singer is who has to be named. The
    // producer's uploads all carry the producer's name, whoever sings.
    static const QRegularExpression feat(QStringLiteral(" (?:feat|ft|featuring) (.+)$"));
    const QRegularExpressionMatch m = feat.match(tokens(track.artist));
    if (m.hasMatch()) {
        const QStringList singers = m.captured(1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        return std::any_of(singers.begin(), singers.end(), [&](const QString &w) {
            return w.size() >= 2 && (ct.contains(w) || ch.contains(w));
        });
    }
    // A tag may hold a name twice over or several names: "Kizuna AI
    // (キズナアイ)", "CANI CLUB/カニ研究会", "A & B". Any one of them will do.
    static const QRegularExpression parts(QStringLiteral("[()（）\\[\\]【】/／&＆、,;]+"));
    for (const QString &n : Matcher::artistNames(track)) {
        QStringList forms = n.split(parts, Qt::SkipEmptyParts);
        forms.prepend(n);
        for (const QString &form : std::as_const(forms)) {
            const QString nt = tokens(form).trimmed();
            if (nt.size() >= 2 && (ct.contains(nt) || ch.contains(nt)))
                return true;
        }
    }
    return false;
}

void Matcher::rank(const TrackInfo &track, QVector<YtCandidate> &candidates,
                   const std::function<bool(const YtCandidate &)> &knownChannel)
{
    const QString titleTok = tokens(track.title);
    const QString albumTok = tokens(track.album);
    const int total = candidates.size();

    // YouTube's own name for the artist: its auto-generated "Artist - Topic"
    // upload of the song is among the results, in whatever script the
    // artist goes by there. It serves as another name of the artist for
    // this lookup — the tag may be romanised and the channel not.
    TrackInfo known = track;
    const QString bareTitle = titleTok.trimmed();
    for (const YtCandidate &c : std::as_const(candidates)) {
        const QString ch = c.channel.trimmed();
        if (ch.endsWith(QStringLiteral("- Topic")) && !bareTitle.isEmpty() && tokens(c.title).contains(bareTitle)) {
            const QString alias = ch.left(ch.size() - 7).trimmed();
            if (!alias.isEmpty() && !Matcher::artistNames(known).contains(alias))
                known.artist += QStringLiteral("; ") + alias;
        }
    }
    const QStringList names = Matcher::artistNames(known);

    for (YtCandidate &c : candidates) {
        const QString ct = tokens(c.title);
        const QString ch = tokens(c.channel);
        c.score = 0;
        c.trusted = false;
        c.ownChannel = false;
        c.rejectReason.clear();

        // A rhythm-game replay: mod strings and scores in the title.
        static const QRegularExpression replay(QStringLiteral(
            "\\b(?:(?:hd|hr|dt|fl|ez|nc|nf|ht|sd|pf){2,}|\\d+pp|\\d+(?:\\.\\d+)?% ?fc)\\b"));
        if (replay.match(ct).hasMatch()) {
            c.rejectReason = QStringLiteral("game replay");
            continue;
        }
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
        // On the artist's own channel the title is not held against the
        // upload: the artist's cover of a song is a cover by its title, the
        // video of a track that is that cover; a music video followed by an
        // album crossfade says "crossfade". The audio decides there, and
        // what it cannot settle goes to review.
        c.ownChannel = isOwnChannel(known, c.channel) || (knownChannel && knownChannel(c));
        if (!c.ownChannel) {
            for (const QString &term : versionTerms()) {
                if (hasTerm(ct, term) && !hasTerm(titleTok, term) && !hasTerm(albumTok, term)) {
                    c.rejectReason = QStringLiteral("“%1” version").arg(term);
                    break;
                }
            }
        }
        if (!c.rejectReason.isEmpty())
            continue;

        // Title
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
        const bool channelIsOnlyArtist = c.ownChannel;
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
        // A channel that merely has the artist's name among other words
        // gets that trust only for titles that do not look like a fan's.
        // A label's channel ("Sony Music (Japan)", unverified) is examined
        // too when the title names the artist: it cannot be accepted
        // outright, only offered for review.
        static const QStringList labelTerms = {
            QStringLiteral("music"), QStringLiteral("records"), QStringLiteral("record"), QStringLiteral("entertainment"),
            QStringLiteral("label"), QStringLiteral("レコード"), QStringLiteral("ミュージック"), QStringLiteral("エンタテインメント"),
        };
        const bool label = artistInTitle && std::any_of(labelTerms.begin(), labelTerms.end(),
                                                        [&ch](const QString &t) { return hasTerm(ch, t); });
        c.trusted = channelIsOnlyArtist || (artistIsChannel && !fan) || ((c.verified || official || label) && !fan);
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const YtCandidate &a, const YtCandidate &b) {
        if (a.rejectReason.isEmpty() != b.rejectReason.isEmpty())
            return a.rejectReason.isEmpty();
        return a.score > b.score;
    });
}
