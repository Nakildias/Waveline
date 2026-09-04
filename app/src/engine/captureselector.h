// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Stable capture-device selector. Hardware inputs stay linked for the life of
// the node; switching is an atomic software choice, not a PipeWire rewire.

#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace waveline {

class CaptureSelector {
public:
    static constexpr std::size_t kMaxInputs = 16;

    CaptureSelector();
    ~CaptureSelector();
    CaptureSelector(const CaptureSelector &) = delete;
    CaptureSelector &operator=(const CaptureSelector &) = delete;

    bool start(const std::string &nodeName, const std::string &description,
               std::string &error);
    void stop();

    void select(std::size_t index);
    void selectSilence();
    static std::string inputPort(std::size_t index);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline
