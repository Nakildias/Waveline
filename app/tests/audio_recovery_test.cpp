// SPDX-License-Identifier: GPL-2.0-or-later
#include "daemon/capturerecoveryqueue.h"
#include "engine/frameadapter.h"

#include <cstdlib>
#include <iostream>
#include <vector>

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
    std::cout << "Audio buffering and capture recovery regression checks passed\n";
}
