// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Gate, compressor and limiter as a PipeWire filter node.

#pragma once

#include "dynamics.h"

#include <memory>
#include <string>

struct spa_io_position;

namespace waveline {

class DynamicsFilter {
public:
    DynamicsFilter();
    ~DynamicsFilter();
    DynamicsFilter(const DynamicsFilter &) = delete;
    DynamicsFilter &operator=(const DynamicsFilter &) = delete;

    bool start(const std::string &nodeName, const std::string &description,
               int channels, std::string &error);
    void stop();

    void setSettings(const DynamicsSettings &s);
    DynamicsSettings settings() const;

    static void filterProcess(void *userdata, spa_io_position *position);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline
