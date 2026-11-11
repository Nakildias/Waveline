// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Display connect/disconnect, as the kernel announces it.
//
// Turning a monitor off and on, or unplugging and replugging one, leaves a
// running USB microphone capturing robotic, chopped audio -- and nothing in
// the PipeWire graph marks the moment. Measured on an RX 9070 with a Wave:3:
// DP-3 dropped and returned, the GPU's HDMI/DP sink stayed suspended, the
// graph driver (the onboard analog output) never changed, no node came or
// went, and the capture stayed broken until a manual Rebuild. So the event
// has to come from outside the graph: the DRM connector hotplug uevent the
// kernel broadcasts on every display change.
//
// This listens on the kernel's own uevent multicast group. Unprivileged,
// no udev dependency, and nothing to install.

#include <QObject>
#include <QTimer>

#include <cstddef>
#include <cstring>

class QSocketNotifier;

namespace waveline {

// True for a kernel uevent that reports a display being connected or
// disconnected: SUBSYSTEM=drm, ACTION=change, HOTPLUG=1. Property-only
// notifications (PROPERTY=, e.g. HDCP state) are not display changes and are
// rejected. `msg` is the raw datagram: "action@devpath" followed by
// NUL-separated KEY=VALUE pairs.
inline bool isDisplayHotplugUevent(const char *msg, std::size_t len) {
    // Kernel datagrams start "action@devpath"; udev's rebroadcasts start
    // "libudev" and never reach the kernel group, but reject them anyway.
    if (!msg || len == 0) return false;
    const std::size_t head = strnlen(msg, len);
    if (!std::memchr(msg, '@', head)) return false;
    bool drm = false, change = false, hotplug = false, property = false;
    for (std::size_t i = head + 1; i < len;) {
        const char *kv = msg + i;
        const std::size_t n = strnlen(kv, len - i);
        auto is = [&](const char *s) { return n == std::strlen(s) && !std::memcmp(kv, s, n); };
        if (is("SUBSYSTEM=drm")) drm = true;
        else if (is("ACTION=change")) change = true;
        else if (is("HOTPLUG=1")) hotplug = true;
        else if (n >= 9 && !std::memcmp(kv, "PROPERTY=", 9)) property = true;
        i += n + 1;
    }
    return drm && change && hotplug && !property;
}

class DisplayHotplugWatcher : public QObject {
    Q_OBJECT
public:
    // `settleMs`: how long the displays have to stay quiet before settled()
    // fires. A monitor waking usually produces a short burst of events
    // (disconnect, connect, link retrain), and that burst should cost the
    // microphone one recovery, not several.
    explicit DisplayHotplugWatcher(int settleMs, QObject *parent = nullptr);
    ~DisplayHotplugWatcher() override;

    // False when the socket could not be opened; the daemon runs without it.
    bool start();

signals:
    void settled();

private:
    void drain();

    int fd_ = -1;
    QSocketNotifier *notifier_ = nullptr;
    QTimer settle_;
};

} // namespace waveline
