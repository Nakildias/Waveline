// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace waveline {

// Adapts arbitrary graph quanta to a fixed-frame processor. Allocate only
// when configuring on the control thread. Exactly one frame of buffering,
// including when the quantum changes; no dropped or repeated samples.
class FrameAdapter {
public:
    void configure(std::size_t frame) {
        input_.assign(frame, 0.0f);
        output_.assign(frame, 0.0f);
        offset_ = 0;
    }

    void reset() {
        offset_ = 0;
        std::fill(output_.begin(), output_.end(), 0.0f);
    }

    template<class Process>
    void process(const float *in, float *out, std::size_t count, Process &&frame) {
        if (input_.empty()) {
            std::copy_n(in, count, out);
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            input_[offset_] = in[i];
            out[i] = output_[offset_];
            if (++offset_ == input_.size()) {
                frame(input_.data(), output_.data());
                offset_ = 0;
            }
        }
    }

private:
    std::vector<float> input_;
    std::vector<float> output_;
    std::size_t offset_ = 0;
};

} // namespace waveline
