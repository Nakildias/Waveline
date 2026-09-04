// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// What the tuner knows about instruments, and the pitch arithmetic that goes
// with it.
//
// Strings are MIDI note numbers rather than names or frequencies. Names are a
// presentation detail (and ambiguous: F# and Gb are the same string), and
// frequencies depend on the reference pitch, which is adjustable -- so neither
// belongs in the table.

#pragma once

#include <QList>
#include <QString>

struct TuningPreset {
    QString id;
    QString name;      // "Drop D"
    QString spelling;  // "D A D G B E", shown beside the name
    // Lowest-numbered string first, in the order the instrument's strings are
    // conventionally counted -- which is not always ascending pitch: a
    // ukulele's fourth string is above its third, and a banjo's fifth is above
    // everything.
    QList<int> notes;
};

struct InstrumentPreset {
    QString id;
    QString name;
    QList<TuningPreset> tunings;
};

// Built once and shared. Chromatic is last and has a single tuning with no
// strings, which is what puts the window into "name whatever note it hears"
// mode rather than snapping to anything.
const QList<InstrumentPreset> &instrumentPresets();

// "E2", "A♯3". Octaves are scientific pitch notation, so middle C is C4.
QString noteName(int midiNote);
// Just the letter: "E", "A♯". What the string buttons carry, because six
// buttons reading E2 A2 D3 G3 B3 E4 are harder to scan than E A D G B E.
QString noteLetter(int midiNote);

// Concert pitch is a parameter: baroque ensembles play at 415 Hz, some
// orchestras tune to 442, and a tuner that assumes 440 is useless to both.
double noteFrequency(double midiNote, double referenceA4);
// The inverse, as a fractional MIDI number -- 69.5 is halfway between A4 and
// A♯4. Returns 0 for a non-positive frequency.
double frequencyToMidi(double hz, double referenceA4);
