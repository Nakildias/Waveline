// SPDX-License-Identifier: GPL-2.0-or-later
#include "displayhotplug.h"

#include <QSocketNotifier>

#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

namespace waveline {

DisplayHotplugWatcher::DisplayHotplugWatcher(int settleMs, QObject *parent)
    : QObject(parent) {
    settle_.setSingleShot(true);
    settle_.setInterval(settleMs);
    connect(&settle_, &QTimer::timeout, this, &DisplayHotplugWatcher::settled);
}

DisplayHotplugWatcher::~DisplayHotplugWatcher() {
    if (fd_ >= 0) ::close(fd_);
}

bool DisplayHotplugWatcher::start() {
    if (fd_ >= 0) return true;
    fd_ = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                   NETLINK_KOBJECT_UEVENT);
    if (fd_ < 0) return false;
    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1;  // the kernel's own broadcasts
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &DisplayHotplugWatcher::drain);
    return true;
}

void DisplayHotplugWatcher::drain() {
    // Every uevent on the machine arrives here; almost all are ignored.
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n <= 0) break;
        if (isDisplayHotplugUevent(buf, static_cast<std::size_t>(n)))
            settle_.start();  // restart: answer the burst once it is over
    }
}

} // namespace waveline
