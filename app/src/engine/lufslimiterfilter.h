// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// LUFS loudness limiter as a PipeWire filter node.

#pragma once

#include "lufslimiter.h"

#include <memory>
#include <string>

struct spa_io_position;

namespace waveline {

class LufsLimiterFilter {
public:
    LufsLimiterFilter();
    ~LufsLimiterFilter();
    LufsLimiterFilter(const LufsLimiterFilter &) = delete;
    LufsLimiterFilter &operator=(const LufsLimiterFilter &) = delete;

    bool start(const std::string &nodeName, const std::string &description,
               std::string &error);
    void stop();

    void setSettings(const LufsLimiterSettings &s);
    LufsLimiterSettings settings() const;

    static void filterProcess(void *userdata, spa_io_position *position);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline
