#include "ui/SystemVolume.h"

#include <QDebug>

#include <cmath>

#ifdef MV_HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif

// ---------------------------------------------------------------------------
// Linux: libpulse on its own thread. Every callback runs on that thread and
// hands the device's state to the object's thread through a queued call;
// the sets lock the loop for the length of one request.
// ---------------------------------------------------------------------------
#ifdef MV_HAVE_PULSE
struct SystemVolume::Backend
{
    SystemVolume *owner = nullptr;
    pa_threaded_mainloop *loop = nullptr;
    pa_context *ctx = nullptr;
    // The default sink, as last reported; its channel volumes keep the
    // balance when the level is set.
    QByteArray sinkName;
    uint32_t sinkIndex = PA_INVALID_INDEX;
    pa_cvolume cvolume{};
    bool ready = false;

    static void stateCb(pa_context *c, void *userdata)
    {
        auto *b = static_cast<Backend *>(userdata);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            b->ready = true;
            pa_context_set_subscribe_callback(c, &Backend::eventCb, b);
            if (pa_operation *o = pa_context_subscribe(c, static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER), nullptr, nullptr))
                pa_operation_unref(o);
            b->askServer();
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            b->ready = false;
            b->sinkIndex = PA_INVALID_INDEX;
            b->report(false, 0, false);
            break;
        default:
            break;
        }
    }
    static void eventCb(pa_context *, pa_subscription_event_type_t t, uint32_t idx, void *userdata)
    {
        auto *b = static_cast<Backend *>(userdata);
        const auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        if (facility == PA_SUBSCRIPTION_EVENT_SERVER)
            b->askServer();
        else if (facility == PA_SUBSCRIPTION_EVENT_SINK && idx == b->sinkIndex)
            b->askSink();
    }
    static void serverCb(pa_context *, const pa_server_info *i, void *userdata)
    {
        auto *b = static_cast<Backend *>(userdata);
        if (!i || !i->default_sink_name) {
            b->report(false, 0, false);
            return;
        }
        b->sinkName = i->default_sink_name;
        b->askSink();
    }
    static void sinkCb(pa_context *, const pa_sink_info *i, int eol, void *userdata)
    {
        auto *b = static_cast<Backend *>(userdata);
        if (eol > 0)
            return;
        if (!i) {
            b->report(false, 0, false);
            return;
        }
        b->sinkIndex = i->index;
        b->cvolume = i->volume;
        b->report(true, double(pa_cvolume_max(&i->volume)) / PA_VOLUME_NORM, i->mute != 0);
    }
    void askServer()
    {
        if (pa_operation *o = pa_context_get_server_info(ctx, &Backend::serverCb, this))
            pa_operation_unref(o);
    }
    void askSink()
    {
        if (sinkName.isEmpty())
            return;
        if (pa_operation *o = pa_context_get_sink_info_by_name(ctx, sinkName.constData(), &Backend::sinkCb, this))
            pa_operation_unref(o);
    }
    void report(bool available, double volume, bool muted)
    {
        QMetaObject::invokeMethod(owner, [o = owner, available, volume, muted] { o->deviceChanged(available, volume, muted); }, Qt::QueuedConnection);
    }

    bool open(SystemVolume *o)
    {
        owner = o;
        loop = pa_threaded_mainloop_new();
        if (!loop)
            return false;
        ctx = pa_context_new(pa_threaded_mainloop_get_api(loop), "MV Player");
        if (!ctx)
            return false;
        pa_context_set_state_callback(ctx, &Backend::stateCb, this);
        if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0)
            return false;
        return pa_threaded_mainloop_start(loop) == 0;
    }
    ~Backend()
    {
        if (loop)
            pa_threaded_mainloop_stop(loop);
        if (ctx) {
            pa_context_set_state_callback(ctx, nullptr, nullptr);
            pa_context_set_subscribe_callback(ctx, nullptr, nullptr);
            pa_context_disconnect(ctx);
            pa_context_unref(ctx);
        }
        if (loop)
            pa_threaded_mainloop_free(loop);
    }
    void setVolume(double v)
    {
        pa_threaded_mainloop_lock(loop);
        if (ready && sinkIndex != PA_INVALID_INDEX && cvolume.channels > 0) {
            pa_cvolume cv = cvolume;
            if (pa_cvolume_max(&cv) == PA_VOLUME_MUTED)
                pa_cvolume_set(&cv, cv.channels, pa_volume_t(std::lround(v * PA_VOLUME_NORM)));
            else
                pa_cvolume_scale(&cv, pa_volume_t(std::lround(v * PA_VOLUME_NORM)));
            cvolume = cv;
            if (pa_operation *o = pa_context_set_sink_volume_by_index(ctx, sinkIndex, &cv, nullptr, nullptr))
                pa_operation_unref(o);
        }
        pa_threaded_mainloop_unlock(loop);
    }
    void setMuted(bool m)
    {
        pa_threaded_mainloop_lock(loop);
        if (ready && sinkIndex != PA_INVALID_INDEX) {
            if (pa_operation *o = pa_context_set_sink_mute_by_index(ctx, sinkIndex, m ? 1 : 0, nullptr, nullptr))
                pa_operation_unref(o);
        }
        pa_threaded_mainloop_unlock(loop);
    }
};

// ---------------------------------------------------------------------------
// Windows: the default render endpoint's master volume, with its change
// notifications (which arrive on a worker thread, hence the queued call).
// A change of default device is not followed: the setting toggled, or the
// app started again, takes the new one.
// ---------------------------------------------------------------------------
#elif defined(Q_OS_WIN)
struct SystemVolume::Backend : public IAudioEndpointVolumeCallback
{
    SystemVolume *owner = nullptr;
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioEndpointVolume *endpoint = nullptr;
    bool comHere = false;
    LONG refs = 1;

    // IUnknown: the object outlives the registration, so counting is a formality.
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&refs); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override
    {
        if (data)
            report(true, data->fMasterVolume, data->bMuted != FALSE);
        return S_OK;
    }
    void report(bool available, double volume, bool muted)
    {
        QMetaObject::invokeMethod(owner, [o = owner, available, volume, muted] { o->deviceChanged(available, volume, muted); }, Qt::QueuedConnection);
    }

    bool open(SystemVolume *o)
    {
        owner = o;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comHere = SUCCEEDED(init); // RPC_E_CHANGED_MODE: Qt initialised it already
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))))
            return false;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
            return false;
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&endpoint))))
            return false;
        endpoint->RegisterControlChangeNotify(this);
        float level = 1;
        BOOL mute = FALSE;
        endpoint->GetMasterVolumeLevelScalar(&level);
        endpoint->GetMute(&mute);
        report(true, level, mute != FALSE);
        return true;
    }
    ~Backend()
    {
        if (endpoint) {
            endpoint->UnregisterControlChangeNotify(this);
            endpoint->Release();
        }
        if (device)
            device->Release();
        if (enumerator)
            enumerator->Release();
        if (comHere)
            CoUninitialize();
    }
    void setVolume(double v)
    {
        if (endpoint)
            endpoint->SetMasterVolumeLevelScalar(float(v), nullptr);
    }
    void setMuted(bool m)
    {
        if (endpoint)
            endpoint->SetMute(m ? TRUE : FALSE, nullptr);
    }
};

#else
struct SystemVolume::Backend
{
    bool open(SystemVolume *) { return false; }
    void setVolume(double) {}
    void setMuted(bool) {}
};
#endif

// ---------------------------------------------------------------------------

SystemVolume::SystemVolume(QObject *parent)
    : QObject(parent)
{
    m_retry.setInterval(5000);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_following && !m_available) {
            close();
            open();
        }
    });
}

SystemVolume::~SystemVolume()
{
    close();
}

void SystemVolume::follow(bool on)
{
    if (on == m_following)
        return;
    m_following = on;
    if (on) {
        open();
        m_retry.start();
    } else {
        m_retry.stop();
        close();
        setAvailable(false);
    }
}

void SystemVolume::open()
{
    if (m_backend)
        return;
    m_backend = std::make_unique<Backend>();
    if (!m_backend->open(this)) {
        qInfo() << "System volume: no output device to follow";
        m_backend.reset();
    }
}

void SystemVolume::close()
{
    m_backend.reset();
}

void SystemVolume::setAvailable(bool a)
{
    if (a == m_available)
        return;
    m_available = a;
    emit availableChanged();
}

void SystemVolume::deviceChanged(bool available, double volume, bool muted)
{
    if (!m_following)
        return;
    volume = qBound(0.0, volume, 1.0);
    const bool moved = !qFuzzyCompare(volume + 1.0, m_volume + 1.0) || muted != m_muted;
    m_volume = volume;
    m_muted = muted;
    setAvailable(available);
    if (moved)
        emit changed();
}

void SystemVolume::setVolume(double v)
{
    v = qBound(0.0, v, 1.0);
    if (!m_available || !m_backend)
        return;
    if (qFuzzyCompare(v + 1.0, m_volume + 1.0))
        return;
    m_volume = v;
    m_backend->setVolume(v);
    emit changed();
}

void SystemVolume::setMuted(bool m)
{
    if (!m_available || !m_backend || m == m_muted)
        return;
    m_muted = m;
    m_backend->setMuted(m);
    emit changed();
}
