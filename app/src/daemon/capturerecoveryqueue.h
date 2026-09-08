// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>
#include <QSet>

namespace waveline {

// Control-thread state for a debounced quiet/rebuild cycle. A new request
// must retain disconnected devices, but must not repeat completed rebuilds.
class CaptureRecoveryQueue {
public:
    bool request(const QStringList &ids) {
        if (ids.isEmpty()) return false;
        for (const auto &id : remaining_) pending_.insert(id);
        remaining_.clear();
        for (const auto &id : ids)
            if (!id.isEmpty()) pending_.insert(id);
        return !pending_.isEmpty();
    }

    const QStringList &beginQuiet() {
        remaining_ = pending_.values();
        remaining_.sort();
        pending_.clear();
        return remaining_;
    }

    bool empty() const { return remaining_.isEmpty(); }
    QString takeNext() { return remaining_.takeFirst(); }

private:
    QSet<QString> pending_;
    QStringList remaining_;
};

} // namespace waveline
