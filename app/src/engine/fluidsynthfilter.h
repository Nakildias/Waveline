// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// PipeWire filter: MIDI in, mono audio out, FluidSynth synthesis backend.

#pragma once

#include "fluidsynthbackend.h"

#include <memory>
#include <mutex>
#include <string>

struct spa_io_position;

namespace waveline {

class FluidSynthFilter {
public:
    FluidSynthFilter();
    ~FluidSynthFilter();
    FluidSynthFilter(const FluidSynthFilter &) = delete;
    FluidSynthFilter &operator=(const FluidSynthFilter &) = delete;

    bool start(const std::string &nodeName, const std::string &description,
               std::string &error);
    void stop();

    bool loadSoundfont(const std::string &path, std::string &error);
    void setSoundfontPath(const std::string &path);

    static void filterProcess(void *userdata, spa_io_position *position);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline
