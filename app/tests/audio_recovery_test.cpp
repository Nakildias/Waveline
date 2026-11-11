// SPDX-License-Identifier: GPL-2.0-or-later
#include "daemon/capturerecoveryqueue.h"
#include "daemon/captureclockrecovery.h"
#include "daemon/captureoverloadrecovery.h"
#include "daemon/displayhotplug.h"

#include <algorithm>
#include <string>
#include "engine/frameadapter.h"
#include "engine/realtimesettings.h"

#include <cstdlib>
#include <iostream>
#include <vector>
#include <array>
#include <atomic>
#include <thread>

static void check(bool ok, const char *message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

static void checkFrames(std::size_t frame, const std::vector<std::size_t> &quanta) {
    waveline::FrameAdapter adapter;
    adapter.configure(frame);
    std::size_t position = 0;
    for (int cycle = 0; cycle < 200; ++cycle) {
        const auto n = quanta[cycle % quanta.size()];
        std::vector<float> samples(n);
        for (std::size_t i = 0; i < n; ++i) samples[i] = float(position + i + 1);
        // Exercise aliased PipeWire input/output as well as frame boundaries.
        adapter.process(samples.data(), samples.data(), n,
                        [frame](const float *in, float *out) {
            for (std::size_t i = 0; i < frame; ++i) out[i] = in[i] * 0.5f;
        });
        for (std::size_t i = 0; i < n; ++i) {
            const auto at = position + i;
            const float expected = at < frame ? 0.0f : float(at - frame + 1) * 0.5f;
            check(samples[i] == expected, "quantum transition lost, repeated, or misaligned audio");
        }
        position += n;
    }
    // Disconnect/bypass must not replay the previous stream.
    adapter.reset();
    std::vector<float> zeros(frame * 3, 0.0f);
    adapter.process(zeros.data(), zeros.data(), zeros.size(),
                    [frame](const float *in, float *out) {
        std::copy_n(in, frame, out);
    });
    for (float sample : zeros) check(sample == 0, "stale audio after reset");
}

int main() {
    // Exercise the production handoff under concurrent writes. Every snapshot
    // must be coherent, and the final published edit must eventually arrive.
    struct Settings { std::array<unsigned, 64> values{}; };
    waveline::RealtimeSettings<Settings> mailbox;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned i = 1; i <= 100000; ++i) {
            Settings s;
            s.values.fill(i);
            mailbox.set(s);
        }
        done.store(true, std::memory_order_release);
    });
    Settings current;
    unsigned previous = 0;
    do {
        if (mailbox.consume(current)) {
            for (auto value : current.values)
                check(value == current.values[0], "torn real-time settings snapshot");
            check(current.values[0] >= previous, "settings moved backwards");
            previous = current.values[0];
        }
    } while (!done.load(std::memory_order_acquire));
    writer.join();
    mailbox.consume(current);
    check(current.values[0] == 100000, "latest settings edit was lost");
    check(mailbox.get().values[0] == 100000, "control-side settings are stale");

    waveline::CaptureOverloadRecovery health;
    check(!health.observe(0, 1, 0, 1), "startup triggered overload recovery");
    check(!health.observe(16000, 1, 1, 2), "single xrun triggered recovery");
    check(!health.observe(22000, 1, 1, 3), "isolated xrun triggered recovery");
    check(!health.observe(30000, 1, 4, 4), "recovery during overload");
    check(!health.observe(34000, 1, 5, 5), "recovery during continued overload");
    check(!health.observe(38999, 1, 5, 6), "recovered before quiet interval");
    check(!health.observe(39000, 1, 5, 6), "stalled callbacks mistaken for healthy clock");
    check(health.observe(39000, 1, 5, 7), "settled overload was not recovered");
    check(!health.observe(40000, 2, 0, 1), "rebuild counter reset triggered recovery");
    check(!health.observe(56000, 2, 4, 2), "recovery during cooldown overload");
    check(!health.observe(62000, 2, 4, 3), "cooldown ignored");
    check(!health.observe(100000, 2, 4, 4), "old burst replayed after cooldown");
    check(!health.observe(101000, 2, 8, 5), "recovery before load ended");
    check(health.observe(106000, 2, 8, 6), "second bounded recovery lost");
    check(!health.observe(180000, 2, 12, 7), "recovery during third overload");
    check(!health.observe(186000, 2, 12, 8), "ten-minute recovery budget exceeded");
    check(!health.observe(610000, 2, 16, 9), "recovery during new overload");
    check(health.observe(616000, 2, 16, 10), "recovery budget did not reset");
    check(!health.observe(700000, 2, 20, 11, true), "overlapped manual recovery");
    check(!health.observe(716000, 2, 20, 12), "manual recovery replayed old overload");
    {
        using C = waveline::CaptureClockRecovery;
        C clock;
        check(!clock.observe(0, 1, 0, 1), "clock: startup triggered recovery");
        check(!clock.observe(3000, 1, 1, 2), "clock: change inside startup grace answered");
        check(!clock.observe(6000, 1, 1, 3), "clock: absorbed startup change replayed");
        // Display unplugged: driver flaps a few times, then settles.
        check(!clock.observe(10000, 1, 2, 4), "clock: recovered while driver moving");
        check(!clock.observe(11000, 1, 4, 5), "clock: recovered during flapping");
        check(!clock.observe(12000, 1, 4, 6), "clock: recovered before settle interval");
        check(!clock.observe(13000, 1, 4, 6), "clock: stalled graph mistaken for settled");
        check(clock.observe(13000, 1, 4, 7), "clock: settled driver change not recovered");
        check(!clock.observe(14000, 1, 4, 8), "clock: one change recovered twice");
        // Recovery runs; the rebuild renegotiates and recreates the selector.
        check(!clock.observe(15000, 1, 5, 9, true), "clock: change during recovery answered");
        check(!clock.observe(18000, 2, 0, 1), "clock: new selector triggered recovery");
        check(!clock.observe(20000, 2, 1, 2), "clock: rebuild's own renegotiation looped");
        check(!clock.observe(25000, 2, 1, 3), "clock: absorbed change replayed");
        // Replug, well after the grace window.
        check(!clock.observe(40000, 2, 2, 4), "clock: recovered on the change itself");
        check(clock.observe(43000, 2, 2, 5), "clock: replug not recovered");
        // A graph that never settles is bounded.
        C storm;
        storm.observe(0, 1, 0, 1);
        unsigned n = 0;
        std::uint64_t changes = 0, cycles = 1;
        for (std::int64_t t = 10000; t < 300000; t += 1000) {
            if ((t / 1000) % 4 == 0) ++changes;
            if (storm.observe(t, 1, changes, ++cycles)) ++n;
        }
        check(n == C::kBudget, "clock: recovery budget not enforced");
    }

    {
        auto ev = [](std::string s) {
            std::replace(s.begin(), s.end(), '|', '\0');
            return waveline::isDisplayHotplugUevent(s.data(), s.size());
        };
        const char *dp = "change@/devices/pci0000:00/0000:03:00.0/drm/card1|ACTION=change|"
                         "DEVPATH=/devices/pci0000:00/0000:03:00.0/drm/card1|SUBSYSTEM=drm|"
                         "HOTPLUG=1|CONNECTOR=466|DEVNAME=dri/card1|SEQNUM=9001";
        check(ev(dp), "uevent: DP connector hotplug missed");
        check(!ev(std::string(dp) + "|PROPERTY=77"), "uevent: HDCP property change taken for hotplug");
        check(!ev("change@/devices/x/sound/card4|ACTION=change|SUBSYSTEM=sound"),
              "uevent: sound change taken for display");
        check(!ev("add@/devices/x/drm/card1|ACTION=add|SUBSYSTEM=drm|HOTPLUG=1"),
              "uevent: GPU add taken for hotplug");
        check(!ev("libudev|ACTION=change|SUBSYSTEM=drm|HOTPLUG=1"), "uevent: udev rebroadcast accepted");
        check(!ev(""), "uevent: empty datagram accepted");
        std::string cut(dp);
        check(!waveline::isDisplayHotplugUevent(cut.data(), 20), "uevent: truncated datagram accepted");
    }

    for (std::size_t frame : {480, 960, 8192}) {
        for (std::size_t quantum : {128, 256, 512, 1024, 4096, 8192})
            checkFrames(frame, {quantum});
        checkFrames(frame, {128, 512, 8192, 256, 4096, 1, 960, 1024});
    }

    waveline::CaptureRecoveryQueue queue;
    check(queue.request({"mic", "tone", "mic"}), "request rejected");
    check(queue.beginQuiet().size() == 2, "duplicate recovery");
    // Replug arrives while both devices are disconnected in the quiet phase.
    queue.request({"webcam"});
    check(queue.beginQuiet().size() == 3, "quiet devices lost on new request");
    check(queue.takeNext() == "mic", "unexpected first device");
    // A further replug between rebuilds must preserve remaining devices, but
    // must not recreate the already successful microphone a second time.
    queue.request({"tone"});
    check(queue.beginQuiet() == QStringList({"tone", "webcam"}),
          "completed recovery repeated or unfinished recovery lost");
    check(queue.takeNext() == "tone", "tone recovery lost");
    check(queue.takeNext() == "webcam", "webcam recovery lost");
    check(queue.empty(), "queue did not finish");
    check(!queue.request({}), "empty request accepted");
    queue.request({"mic"});
    check(queue.beginQuiet() == QStringList({"mic"}), "later explicit rebuild lost");
    check(queue.takeNext() == "mic", "manual job missing");
    check(queue.retry("mic"), "first retry rejected");
    check(queue.takeNext() == "mic", "failed input was dropped");
    check(queue.retry("mic"), "second retry rejected");
    check(queue.takeNext() == "mic", "second retry missing");
    check(!queue.retry("mic"), "recovery retries are unbounded");
    check(queue.empty(), "exhausted job left queued");
    queue.request({"mic", "tone"});
    queue.beginQuiet();
    check(queue.takeNext() == "mic", "new recovery missing");
    check(queue.retry("mic"), "new explicit recovery retained failed budget");
    check(queue.takeNext() == "tone", "failed input starved peer");
    queue.complete("tone");
    queue.request({"webcam"});
    check(queue.beginQuiet().contains("mic"), "overlapping event lost retry");
    check(queue.takeNext() == "mic", "retained retry order wrong");
    check(queue.retry("mic"), "remaining retry lost");
    check(queue.takeNext() == "webcam", "new peer starved by retry");
    queue.complete("webcam");
    check(queue.takeNext() == "mic", "last retry missing");
    check(!queue.retry("mic"), "overlapping event reset failed retry budget");
    std::cout << "Audio buffering and capture recovery regression checks passed\n";
}
