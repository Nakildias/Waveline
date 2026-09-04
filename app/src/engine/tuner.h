// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Pitch detection for the instrument tuner.
//
// Lives in the daemon for the same reason everything else audio does: the GUI
// links neither PipeWire nor the device layer, so it cannot open a capture
// stream of its own. The window asks for a source, polls a reading, and does
// the note arithmetic itself.
//
// Two kinds of source, because a MIDI instrument is not a microphone. Audio
// runs YIN over a capture tap; MIDI reads note-on and pitch bend straight off
// the wire, where the pitch is already exact and the only interesting offset
// is whatever the bender is doing.

#pragma once

#include <memory>
#include <string>

namespace waveline {

struct TunerReading {
    // 0 when nothing usable is being heard -- too quiet, or no periodicity.
    double frequencyHz = 0.0;
    // 0..1. Below ~0.5 the estimate is noise and should not move a needle.
    double confidence = 0.0;
    // RMS of the analysis window, 0..1. Drives the "is anything arriving at
    // all" indicator, which is the first question when a tuner reads nothing.
    double level = 0.0;
    // MIDI sources only: the note being held, and the bend wheel in cents.
    // -1 when no note is down.
    int midiNote = -1;
    double bendCents = 0.0;
};

class Tuner {
public:
    Tuner();
    ~Tuner();
    Tuner(const Tuner &) = delete;
    Tuner &operator=(const Tuner &) = delete;

    // kind is "audio" -- source is a PipeWire node name -- or "midi", where
    // source is a "node|port" match as MidiDevices() reports them. Restarting
    // on an already running tuner is fine and is how the source is changed.
    bool start(const std::string &kind, const std::string &source, std::string &error);
    void stop();

    bool active() const;
    std::string sourceKind() const;
    std::string source() const;

    // A MIDI tap cannot connect itself. Every ALSA MIDI device on the system
    // arrives on one bridge node, so pointing a stream at that node picks an
    // arbitrary one of its ports -- which makes choosing a device in the UI
    // mean nothing. The tap therefore publishes a filter with a named port and
    // leaves the linking to the caller, which has the graph engine to do it
    // with. Audio sources need none of this: they are nodes of their own and
    // target.object resolves them exactly.
    static const char *midiNodeName();
    static const char *midiPortName();

    TunerReading reading() const;

    // Plays the note a string should be at, into `targetSink` -- the Monitor
    // mix, so it arrives where everything else the user hears does. A tuner
    // tells you which way to turn the peg; hearing the note tells you what you
    // are aiming at, which is the part a display cannot do.
    //
    // Only while the tuner is running: it borrows the tuner's PipeWire loop
    // rather than opening a connection of its own for a two-second tone.
    void playReference(double hz, int ms, const std::string &targetSink);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline
