#pragma once

#include <QObject>
#include <QTimer>

#include <memory>

// The operating system's output volume: the default output device's level
// and mute, read as they change (keys, a mixer, another app) and set from
// the volume bar when Settings → Playback maps the bar to it.
//
// Linux: the PulseAudio client API, which PipeWire serves as well; the
// default sink, followed across a change of default. Windows: the Core
// Audio endpoint volume of the default render device. Elsewhere, or
// without a sound server, nothing is available and the bar keeps to the
// player's own volume.
//
// Nothing is touched until follow() is called, so a run that never asks
// for it never opens a connection to the sound server.
class SystemVolume : public QObject
{
    Q_OBJECT

public:
    explicit SystemVolume(QObject *parent = nullptr);
    ~SystemVolume() override;

    // Opens (or closes) the connection. Available only once the device is
    // known; a connection that is lost is tried again every few seconds.
    void follow(bool on);
    bool following() const { return m_following; }

    bool available() const { return m_available; }
    double volume() const { return m_volume; } // 0..1 of the device's full level
    bool muted() const { return m_muted; }

    void setVolume(double v);
    void setMuted(bool m);

signals:
    void availableChanged();
    void changed(); // volume or mute, from here or from outside

private:
    struct Backend;

    void open();
    void close();
    void setAvailable(bool a);
    // Called on this object's thread with the device's state.
    void deviceChanged(bool available, double volume, bool muted);

    std::unique_ptr<Backend> m_backend;
    QTimer m_retry;
    bool m_following = false;
    bool m_available = false;
    double m_volume = 1.0;
    bool m_muted = false;
};
