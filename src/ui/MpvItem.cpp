#include "ui/MpvItem.h"

#include "ui/AccentColor.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMutex>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QStandardPaths>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <atomic>
#include <clocale>
#include <cmath>

// State shared by the item (GUI thread) and its renderer (render thread).
// The renderer owns the render context; whoever lets go last destroys mpv.
struct MpvCore {
    mpv_handle *mpv = nullptr;
    mpv_render_context *rc = nullptr;
    QMutex mutex;
    MpvItem *item = nullptr; // guarded by mutex; null once the item is gone
    std::atomic<bool> sampleRequested{false}; // GUI thread asks, renderer answers

    ~MpvCore()
    {
        if (mpv)
            mpv_terminate_destroy(mpv);
    }

    static void onWakeup(void *ctx)
    {
        auto *core = static_cast<MpvCore *>(ctx);
        QMutexLocker lock(&core->mutex);
        if (core->item)
            QMetaObject::invokeMethod(core->item, &MpvItem::drainEvents, Qt::QueuedConnection);
    }

    static void onRenderUpdate(void *ctx)
    {
        auto *core = static_cast<MpvCore *>(ctx);
        QMutexLocker lock(&core->mutex);
        if (core->item)
            QMetaObject::invokeMethod(core->item, &QQuickItem::update, Qt::QueuedConnection);
    }

    void deliverPixels(const QByteArray &rgba)
    {
        QMutexLocker lock(&mutex);
        if (MpvItem *target = item)
            QMetaObject::invokeMethod(target, [target, rgba] { target->onFramePixels(rgba); }, Qt::QueuedConnection);
    }
};

namespace {

bool g_silent = false;

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(QByteArray(name))) : nullptr;
}

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer(std::shared_ptr<MpvCore> core)
        : m_core(std::move(core))
    {
    }

    ~MpvRenderer() override
    {
        // Runs on the render thread with the GL context current.
        if (m_pbo) {
            if (QOpenGLContext *ctx = QOpenGLContext::currentContext())
                ctx->extraFunctions()->glDeleteBuffers(1, &m_pbo);
        }
        if (m_core->rc) {
            mpv_render_context_set_update_callback(m_core->rc, nullptr, nullptr);
            mpv_render_context_free(m_core->rc);
            m_core->rc = nullptr;
        }
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        if (!m_core->rc)
            createContext();
        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void render() override
    {
        if (!m_core->rc)
            return;
        QOpenGLFramebufferObject *fbo = framebufferObject();
        mpv_opengl_fbo target{int(fbo->handle()), fbo->width(), fbo->height(), 0};
        int flipY = 0;
        // Never wait inside the scene graph's render pass: the UI must keep
        // its own frame pacing regardless of the video's.
        int block = 0;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &target},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(m_core->rc, params);
        sampleFrame(fbo);
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    // Reads a thumbnail-sized copy of the frame back for colour analysis.
    // The read goes through a pixel buffer and is collected a few frames
    // later, so the render thread never waits for the GPU.
    void sampleFrame(QOpenGLFramebufferObject *fbo)
    {
        constexpr int w = 16, h = 9, bytes = w * h * 4;
        QOpenGLExtraFunctions *f = QOpenGLContext::currentContext()->extraFunctions();

        if (m_readPending && ++m_readAge >= 2) {
            f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
            if (const void *data = f->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT)) {
                const QByteArray rgba(static_cast<const char *>(data), bytes);
                f->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                m_core->deliverPixels(rgba);
            }
            f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            m_readPending = false;
        }

        if (m_readPending || !m_core->sampleRequested.exchange(false))
            return;

        // Shrink in steps so each output pixel averages a real area.
        if (!m_small[0]) {
            const QSize sizes[3] = {QSize(256, 144), QSize(64, 36), QSize(w, h)};
            for (int i = 0; i < 3; ++i)
                m_small[i] = std::make_unique<QOpenGLFramebufferObject>(sizes[i]);
            f->glGenBuffers(1, &m_pbo);
            f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
            f->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
            f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        QOpenGLFramebufferObject *src = fbo;
        for (auto &dst : m_small) {
            QOpenGLFramebufferObject::blitFramebuffer(dst.get(), QRect(QPoint(0, 0), dst->size()), src,
                                                      QRect(QPoint(0, 0), src->size()), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            src = dst.get();
        }
        m_small[2]->bind();
        f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
        f->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        fbo->bind();
        m_readPending = true;
        m_readAge = 0;
    }

    void createContext()
    {
        mpv_opengl_init_params gl{getProcAddress, nullptr};
        QList<mpv_render_param> params;
        params.append({MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)});
        params.append({MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl});
        // Native display handles let mpv hand decoded frames to GL without a copy.
#if QT_CONFIG(wayland)
        if (auto *w = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
            params.append({MPV_RENDER_PARAM_WL_DISPLAY, w->display()});
#endif
#if QT_CONFIG(xcb)
        if (auto *x = qGuiApp->nativeInterface<QNativeInterface::QX11Application>())
            params.append({MPV_RENDER_PARAM_X11_DISPLAY, x->display()});
#endif
        params.append({MPV_RENDER_PARAM_INVALID, nullptr});

        const int rc = mpv_render_context_create(&m_core->rc, m_core->mpv, params.data());
        if (rc < 0) {
            qWarning("mpv: cannot create render context: %s", mpv_error_string(rc));
            m_core->rc = nullptr;
            return;
        }
        mpv_render_context_set_update_callback(m_core->rc, MpvCore::onRenderUpdate, m_core.get());
    }

    std::shared_ptr<MpvCore> m_core;
    std::unique_ptr<QOpenGLFramebufferObject> m_small[3];
    GLuint m_pbo = 0;
    bool m_readPending = false;
    int m_readAge = 0;
};

void setOption(mpv_handle *mpv, const char *name, const char *value)
{
    const int rc = mpv_set_option_string(mpv, name, value);
    if (rc < 0)
        qWarning("mpv: option %s=%s rejected: %s", name, value, mpv_error_string(rc));
}

} // namespace

MpvItem::MpvItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
    , m_core(std::make_shared<MpvCore>())
{
    // libmpv requires the C numeric locale.
    std::setlocale(LC_NUMERIC, "C");

    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        qWarning("mpv: cannot create player");
        return;
    }
    m_core->mpv = mpv;
    m_core->item = this;

    setOption(mpv, "vo", "libmpv");
    const QByteArray hwdec = qgetenv("MVPLAYER_HWDEC");
    setOption(mpv, "hwdec", hwdec.isEmpty() ? "auto-safe" : hwdec.constData());
    // Decode well ahead of display. Mapping a zero-copy hardware frame waits
    // for the GPU to finish decoding it; with a queue of finished frames that
    // wait never lands in the UI's render pass (it cost 20-30 ms at 4K).
    setOption(mpv, "vd-queue-enable", "yes");
    setOption(mpv, "hwdec-extra-frames", "12");
    setOption(mpv, "terminal", "no");
    setOption(mpv, "config", "no");
    setOption(mpv, "osc", "no");
    setOption(mpv, "osd-level", "0");
    setOption(mpv, "input-default-bindings", "no");
    setOption(mpv, "input-vo-keyboard", "no");
    setOption(mpv, "ytdl", "no");
    // Hold the last frame at the end of a file instead of going black; the
    // UI decides what plays next (see eof-reached below).
    setOption(mpv, "keep-open", "yes");
    setOption(mpv, "idle", "yes");
    setOption(mpv, "audio-display", "no");
    setOption(mpv, "audio-client-name", "MV Player");
    setOption(mpv, "video-timing-offset", "0");
    // YouTube's encodes band badly in dark gradients. A little stronger than
    // mpv's defaults, with less grain added.
    setOption(mpv, "deband", "yes");
    setOption(mpv, "deband-iterations", "2");
    setOption(mpv, "deband-threshold", "48");
    setOption(mpv, "deband-range", "20");
    setOption(mpv, "deband-grain", "16");
    setOption(mpv, "volume-max", "100");
    // Subtitles lie beside the video as "<name>.<language>.srt". Plain ones
    // get this look; those that carry their own styling (.ass) keep it.
    setOption(mpv, "sub-auto", "exact");
    setOption(mpv, "subs-fallback", "yes");
    setOption(mpv, "subs-with-matching-audio", "yes");
    setOption(mpv, "sub-font", "sans-serif");
    setOption(mpv, "sub-font-size", "42");
    setOption(mpv, "sub-color", "#FFFFFFFF");
    setOption(mpv, "sub-outline-color", "#E6000000");
    setOption(mpv, "sub-outline-size", "2.2");
    setOption(mpv, "sub-shadow-offset", "0");
    setOption(mpv, "sub-blur", "0.3");
    setOption(mpv, "sub-margin-y", "40");
    setOption(mpv, "sub-visibility", m_subtitlesVisible ? "yes" : "no");
    if (!m_subtitleLangs.isEmpty())
        setOption(mpv, "slang", m_subtitleLangs.toUtf8().constData());
    if (g_silent)
        setOption(mpv, "ao", "null");
    // Compiled video shaders are reused across runs, so opening a video does
    // not stall the render thread on shader compilation every time.
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cacheDir.isEmpty() && QDir().mkpath(cacheDir)) {
        setOption(mpv, "gpu-shader-cache", "yes");
        setOption(mpv, "gpu-shader-cache-dir", (cacheDir + QStringLiteral("/shaders")).toUtf8().constData());
    }
    // Extra options for experiments: MVPLAYER_MPV_OPTS="name=value,name=value"
    for (const QByteArray &kv : qgetenv("MVPLAYER_MPV_OPTS").split(',')) {
        const int eq = kv.indexOf('=');
        if (eq > 0)
            setOption(mpv, kv.left(eq).constData(), kv.mid(eq + 1).constData());
    }
    mpv_request_log_messages(mpv, "warn");

    const int rc = mpv_initialize(mpv);
    if (rc < 0) {
        qWarning("mpv: initialisation failed: %s", mpv_error_string(rc));
        return;
    }

    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "aid", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "track-list/count", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "dheight", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    mpv_set_wakeup_callback(mpv, MpvCore::onWakeup, m_core.get());

    // One frame a second follows the mood of a video closely enough; the
    // readback is a 16x9 image, so this costs next to nothing.
    m_sampleTimer.setInterval(1000);
    connect(&m_sampleTimer, &QTimer::timeout, this, &MpvItem::requestSample);
    connect(this, &MpvItem::activeChanged, this, &MpvItem::updateSampling);
    connect(this, &MpvItem::pausedChanged, this, &MpvItem::updateSampling);
    connect(this, &MpvItem::firstFrame, this, [this] {
        // Look at a new video soon after it appears rather than a full interval later.
        if (m_sampleTimer.isActive())
            QTimer::singleShot(250, this, &MpvItem::requestSample);
    });
}

void MpvItem::setSilent(bool silent)
{
    g_silent = silent;
}

MpvItem::~MpvItem()
{
    if (m_core->mpv) {
        mpv_set_wakeup_callback(m_core->mpv, nullptr, nullptr);
        const char *cmd[] = {"stop", nullptr};
        mpv_command_async(m_core->mpv, 0, cmd);
    }
    QMutexLocker lock(&m_core->mutex);
    m_core->item = nullptr;
}

QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const
{
    return new MpvRenderer(m_core);
}

void MpvItem::load(const QString &path)
{
    if (!m_core->mpv)
        return;
    const QByteArray p = path.toUtf8();
    const char *cmd[] = {"loadfile", p.constData(), nullptr};
    mpv_command_async(m_core->mpv, 0, cmd);
    setPaused(false);
    m_awaitingFirstFrame = true;
    m_restartColor = true;
    if (m_position != 0) {
        m_position = 0;
        emit positionChanged();
    }
    if (!m_active) {
        m_active = true;
        emit activeChanged();
    }
}

void MpvItem::stop()
{
    if (!m_core->mpv)
        return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command_async(m_core->mpv, 0, cmd);
    m_awaitingFirstFrame = false;
    if (m_active) {
        m_active = false;
        emit activeChanged();
    }
    m_position = 0;
    m_duration = 0;
    emit positionChanged();
    emit durationChanged();
    if (m_frameColorValid) {
        m_frameColorValid = false;
        emit frameColorChanged();
    }
}

void MpvItem::seek(double seconds, bool exact)
{
    if (!m_core->mpv || !m_active)
        return;
    const QByteArray t = QByteArray::number(qMax(0.0, seconds), 'f', 3);
    const char *cmd[] = {"seek", t.constData(), exact ? "absolute+exact" : "absolute+keyframes", nullptr};
    mpv_command_async(m_core->mpv, 0, cmd);
}

void MpvItem::seekRelative(double seconds)
{
    seek(qBound(0.0, m_position + seconds, qMax(0.0, m_duration - 0.5)), true);
}

void MpvItem::togglePause()
{
    setPaused(!m_paused);
}

void MpvItem::setPaused(bool paused)
{
    if (!m_core->mpv)
        return;
    int flag = paused ? 1 : 0;
    mpv_set_property_async(m_core->mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvItem::setVolume(double v)
{
    if (!m_core->mpv)
        return;
    double pct = qBound(0.0, v, 1.0) * 100.0;
    mpv_set_property_async(m_core->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &pct);
}

void MpvItem::setSubtitlesVisible(bool v)
{
    if (v == m_subtitlesVisible)
        return;
    m_subtitlesVisible = v;
    if (m_core && m_core->mpv) {
        int flag = v ? 1 : 0;
        mpv_set_property_async(m_core->mpv, 0, "sub-visibility", MPV_FORMAT_FLAG, &flag);
    }
    emit subtitlesVisibleChanged();
}

void MpvItem::setSubtitleLangs(const QString &langs)
{
    if (langs == m_subtitleLangs)
        return;
    m_subtitleLangs = langs;
    // Takes effect with the next file.
    if (m_core && m_core->mpv)
        mpv_set_property_string(m_core->mpv, "slang", langs.toUtf8().constData());
    emit subtitleLangsChanged();
}

void MpvItem::setMuted(bool m)
{
    if (!m_core->mpv)
        return;
    int flag = m ? 1 : 0;
    mpv_set_property_async(m_core->mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

void MpvItem::setAudioTrack(int id)
{
    if (!m_core->mpv)
        return;
    qint64 v = id;
    mpv_set_property_async(m_core->mpv, 0, "aid", MPV_FORMAT_INT64, &v);
}

void MpvItem::setColorSampling(bool on)
{
    if (on == m_colorSampling)
        return;
    m_colorSampling = on;
    emit colorSamplingChanged();
    updateSampling();
    if (on)
        requestSample();
}

void MpvItem::updateSampling()
{
    const bool run = m_colorSampling && m_active && !m_paused;
    if (run == m_sampleTimer.isActive())
        return;
    if (run)
        m_sampleTimer.start();
    else
        m_sampleTimer.stop();
}

void MpvItem::requestSample()
{
    if (m_colorSampling && m_active)
        m_core->sampleRequested = true;
}

void MpvItem::onFramePixels(const QByteArray &rgba)
{
    if (!m_colorSampling || !m_active)
        return;
    // Blend with the previous samples so quick cuts do not make the UI
    // flicker, but lean on the newest one so it still keeps up with the video.
    const QColor sample = AccentColor::fromFrame(rgba);
    const double keep = m_restartColor ? 0.0 : 0.3;
    m_avgR = keep * m_avgR + (1 - keep) * sample.redF();
    m_avgG = keep * m_avgG + (1 - keep) * sample.greenF();
    m_avgB = keep * m_avgB + (1 - keep) * sample.blueF();
    m_restartColor = false;

    const QColor c = QColor::fromRgbF(float(m_avgR), float(m_avgG), float(m_avgB));
    const int diff = qAbs(c.red() - m_frameColor.red()) + qAbs(c.green() - m_frameColor.green())
        + qAbs(c.blue() - m_frameColor.blue());
    if (m_frameColorValid && diff < 10)
        return;
    m_frameColor = c;
    m_frameColorValid = true;
    emit frameColorChanged();
}

void MpvItem::drainEvents()
{
    if (!m_core->mpv)
        return;
    for (;;) {
        const mpv_event *e = mpv_wait_event(m_core->mpv, 0);
        if (e->event_id == MPV_EVENT_NONE)
            break;
        switch (e->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            handleProperty(static_cast<const mpv_event_property *>(e->data));
            break;
        case MPV_EVENT_FILE_LOADED:
            refreshTracks();
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            if (m_awaitingFirstFrame) {
                m_awaitingFirstFrame = false;
                emit firstFrame();
            } else if (m_sampleTimer.isActive()) {
                // After a seek the picture is somewhere else entirely.
                QTimer::singleShot(150, this, &MpvItem::requestSample);
            }
            break;
        case MPV_EVENT_END_FILE: {
            const auto *ef = static_cast<const mpv_event_end_file *>(e->data);
            if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                // Clear `active` first: handlers typically load the next file.
                m_active = false;
                emit activeChanged();
                emit loadFailed(QString::fromUtf8(mpv_error_string(ef->error)));
            }
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            const auto *msg = static_cast<const mpv_event_log_message *>(e->data);
            qWarning("mpv [%s] %s: %s", msg->level, msg->prefix, QByteArray(msg->text).trimmed().constData());
            break;
        }
        default:
            break;
        }
    }
}

void MpvItem::handleProperty(const mpv_event_property *prop)
{
    const QByteArray name(prop->name);
    const bool has = prop->data != nullptr;
    if (name == "time-pos") {
        if (has && prop->format == MPV_FORMAT_DOUBLE) {
            m_position = *static_cast<double *>(prop->data);
            emit positionChanged();
        }
    } else if (name == "duration") {
        m_duration = has && prop->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(prop->data) : 0;
        emit durationChanged();
    } else if (name == "pause") {
        if (has && prop->format == MPV_FORMAT_FLAG) {
            m_paused = *static_cast<int *>(prop->data) != 0;
            emit pausedChanged();
        }
    } else if (name == "volume") {
        if (has && prop->format == MPV_FORMAT_DOUBLE) {
            m_volume = *static_cast<double *>(prop->data) / 100.0;
            emit volumeChanged();
        }
    } else if (name == "mute") {
        if (has && prop->format == MPV_FORMAT_FLAG) {
            m_muted = *static_cast<int *>(prop->data) != 0;
            emit mutedChanged();
        }
    } else if (name == "aid") {
        m_audioTrack = has && prop->format == MPV_FORMAT_INT64 ? int(*static_cast<qint64 *>(prop->data)) : 0;
        emit audioTrackChanged();
        refreshTracks();
    } else if (name == "track-list/count") {
        refreshTracks();
    } else if (name == "dwidth" || name == "dheight") {
        const int v = has && prop->format == MPV_FORMAT_INT64 ? int(*static_cast<qint64 *>(prop->data)) : 0;
        if (name == "dwidth")
            m_videoSize.setWidth(v);
        else
            m_videoSize.setHeight(v);
        emit videoSizeChanged();
    } else if (name == "eof-reached") {
        if (has && prop->format == MPV_FORMAT_FLAG && *static_cast<int *>(prop->data) != 0 && m_active)
            emit endReached();
    } else if (name == "hwdec-current") {
        m_hwdec = has && prop->format == MPV_FORMAT_STRING ? QString::fromUtf8(*static_cast<char **>(prop->data)) : QString();
        emit hwdecChanged();
    }
}

void MpvItem::refreshTracks()
{
    QVariantList tracks;
    bool subs = false;
    mpv_node node;
    if (mpv_get_property(m_core->mpv, "track-list", MPV_FORMAT_NODE, &node) >= 0) {
        if (node.format == MPV_FORMAT_NODE_ARRAY) {
            for (int i = 0; i < node.u.list->num; ++i) {
                const mpv_node &t = node.u.list->values[i];
                if (t.format != MPV_FORMAT_NODE_MAP)
                    continue;
                QVariantMap m;
                QString type;
                for (int k = 0; k < t.u.list->num; ++k) {
                    const QByteArray key(t.u.list->keys[k]);
                    const mpv_node &v = t.u.list->values[k];
                    if (key == "type" && v.format == MPV_FORMAT_STRING)
                        type = QString::fromUtf8(v.u.string);
                    else if (key == "id" && v.format == MPV_FORMAT_INT64)
                        m.insert(QStringLiteral("id"), int(v.u.int64));
                    else if (key == "title" && v.format == MPV_FORMAT_STRING)
                        m.insert(QStringLiteral("title"), QString::fromUtf8(v.u.string));
                    else if (key == "codec" && v.format == MPV_FORMAT_STRING)
                        m.insert(QStringLiteral("codec"), QString::fromUtf8(v.u.string));
                    else if (key == "selected" && v.format == MPV_FORMAT_FLAG)
                        m.insert(QStringLiteral("selected"), v.u.flag != 0);
                }
                if (type == QLatin1String("audio"))
                    tracks.append(m);
                else if (type == QLatin1String("sub"))
                    subs = true;
            }
        }
        mpv_free_node_contents(&node);
    }
    if (tracks != m_audioTracks) {
        m_audioTracks = tracks;
        emit audioTracksChanged();
    }
    if (subs != m_hasSubtitles) {
        m_hasSubtitles = subs;
        emit hasSubtitlesChanged();
    }
}
