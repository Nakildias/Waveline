// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Simple linear gain as a PipeWire filter (mic send levels, etc.).

#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace waveline {

class GainFilter {
public:
    GainFilter();
    ~GainFilter();
    GainFilter(const GainFilter &) = delete;
    GainFilter &operator=(const GainFilter &) = delete;

    bool start(const std::string &nodeName, const std::string &description,
               int channels, std::string &error);
    void stop();

    void setGain(float linear);
    float gain() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline
