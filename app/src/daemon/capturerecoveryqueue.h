// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>
#include <QSet>
#include <QHash>

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
            if (!id.isEmpty()) {
                if (!pending_.contains(id)) attempts_.remove(id);
                pending_.insert(id);
            }
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

    // Keep failed devices in the current batch, without interrupting peers
    // already waiting. Three total attempts; a later explicit request resets it.
    bool retry(const QString &id) {
        if (++attempts_[id] >= 3) { attempts_.remove(id); return false; }
        if (!remaining_.contains(id)) remaining_.append(id);
        return true;
    }
    void complete(const QString &id) { attempts_.remove(id); }

private:
    QSet<QString> pending_;
    QStringList remaining_;
    QHash<QString, int> attempts_;
};

} // namespace waveline
