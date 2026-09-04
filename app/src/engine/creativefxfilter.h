// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Reverb, overdrive and delay as a PipeWire filter node.

#pragma once

#include "creativefx.h"

#include <memory>
#include <string>

struct spa_io_position;

namespace waveline {

class CreativeFxFilter {
public:
    CreativeFxFilter();
    ~CreativeFxFilter();
    CreativeFxFilter(const CreativeFxFilter &) = delete;
    CreativeFxFilter &operator=(const CreativeFxFilter &) = delete;

    bool start(const std::string &nodeName, const std::string &description,
               int channels, std::string &error);
    void stop();

    void setSettings(const CreativeFxSettings &s);
    CreativeFxSettings settings() const;

    static void filterProcess(void *userdata, spa_io_position *position);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline
