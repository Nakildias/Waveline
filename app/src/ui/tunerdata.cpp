// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "tunerdata.h"

#include <QCoreApplication>

#include <cmath>

namespace {

TuningPreset tuning(const char *id, const QString &name, const QString &spelling,
                    const QList<int> &notes) {
    return TuningPreset{QString::fromLatin1(id), name, spelling, notes};
}

QList<InstrumentPreset> buildPresets() {
    // Translated at first use rather than at static-init time, which is why
    // this is a function and not a file-scope table.
    const auto t = [](const char *s) {
        return QCoreApplication::translate("TunerData", s);
    };

    QList<InstrumentPreset> out;

    out.push_back({QStringLiteral("guitar"), t("Guitar"), {
        tuning("standard",  t("Standard"),         QStringLiteral("E A D G B E"),
               {40, 45, 50, 55, 59, 64}),
        tuning("drop-d",    t("Drop D"),           QStringLiteral("D A D G B E"),
               {38, 45, 50, 55, 59, 64}),
        tuning("half-down", t("Half step down"),   QStringLiteral("E♭ A♭ D♭ G♭ B♭ E♭"),
               {39, 44, 49, 54, 58, 63}),
        tuning("full-down", t("Whole step down"),  QStringLiteral("D G C F A D"),
               {38, 43, 48, 53, 57, 62}),
        tuning("drop-cs",   t("Drop C♯"),     QStringLiteral("C♯ G♯ C♯ F♯ B♭ E♭"),
               {37, 44, 49, 54, 58, 63}),
        tuning("drop-c",    t("Drop C"),           QStringLiteral("C G C F A D"),
               {36, 43, 48, 53, 57, 62}),
        tuning("drop-b",    t("Drop B"),           QStringLiteral("B F♯ B E G♯ C♯"),
               {35, 42, 47, 52, 56, 61}),
        tuning("dadgad",    t("DADGAD"),           QStringLiteral("D A D G A D"),
               {38, 45, 50, 55, 57, 62}),
        tuning("open-g",    t("Open G"),           QStringLiteral("D G D G B D"),
               {38, 43, 50, 55, 59, 62}),
        tuning("open-d",    t("Open D"),           QStringLiteral("D A D F♯ A D"),
               {38, 45, 50, 54, 57, 62}),
        tuning("open-e",    t("Open E"),           QStringLiteral("E B E G♯ B E"),
               {40, 47, 52, 56, 59, 64}),
        tuning("open-c",    t("Open C"),           QStringLiteral("C G C G C E"),
               {36, 43, 48, 55, 60, 64}),
        tuning("open-a",    t("Open A"),           QStringLiteral("E A E A C♯ E"),
               {40, 45, 52, 57, 61, 64}),
    }});

    out.push_back({QStringLiteral("guitar-7"), t("Guitar (7-string)"), {
        tuning("standard", t("Standard"), QStringLiteral("B E A D G B E"),
               {35, 40, 45, 50, 55, 59, 64}),
        tuning("drop-a",   t("Drop A"),   QStringLiteral("A E A D G B E"),
               {33, 40, 45, 50, 55, 59, 64}),
    }});

    out.push_back({QStringLiteral("guitar-8"), t("Guitar (8-string)"), {
        tuning("standard", t("Standard"), QStringLiteral("F♯ B E A D G B E"),
               {30, 35, 40, 45, 50, 55, 59, 64}),
        tuning("drop-e",   t("Drop E"),   QStringLiteral("E B E A D G B E"),
               {28, 35, 40, 45, 50, 55, 59, 64}),
    }});

    out.push_back({QStringLiteral("bass"), t("Bass"), {
        tuning("standard",  t("Standard"),        QStringLiteral("E A D G"),
               {28, 33, 38, 43}),
        tuning("drop-d",    t("Drop D"),          QStringLiteral("D A D G"),
               {26, 33, 38, 43}),
        tuning("half-down", t("Half step down"),  QStringLiteral("E♭ A♭ D♭ G♭"),
               {27, 32, 37, 42}),
        tuning("full-down", t("Whole step down"), QStringLiteral("D G C F"),
               {26, 31, 36, 41}),
        tuning("drop-c",    t("Drop C"),          QStringLiteral("C G C F"),
               {24, 31, 36, 41}),
    }});

    out.push_back({QStringLiteral("bass-5"), t("Bass (5-string)"), {
        tuning("standard", t("Standard"), QStringLiteral("B E A D G"),
               {23, 28, 33, 38, 43}),
        tuning("tenor",    t("Tenor"),    QStringLiteral("E A D G C"),
               {28, 33, 38, 43, 48}),
        tuning("drop-a",   t("Drop A"),   QStringLiteral("A E A D G"),
               {21, 28, 33, 38, 43}),
    }});

    out.push_back({QStringLiteral("bass-6"), t("Bass (6-string)"), {
        tuning("standard", t("Standard"), QStringLiteral("B E A D G C"),
               {23, 28, 33, 38, 43, 48}),
    }});

    out.push_back({QStringLiteral("ukulele"), t("Ukulele"), {
        // The fourth string is the high one on a standard uke: the strings run
        // G C E A with the G above the C, which is why this list is not in
        // ascending order and why the auto-detect matches by pitch distance
        // rather than by position.
        tuning("standard", t("Standard (high G)"), QStringLiteral("G C E A"),
               {67, 60, 64, 69}),
        tuning("low-g",    t("Low G"),             QStringLiteral("G C E A"),
               {55, 60, 64, 69}),
        tuning("d-tuning", t("D tuning"),          QStringLiteral("A D F♯ B"),
               {69, 62, 66, 71}),
    }});

    out.push_back({QStringLiteral("ukulele-baritone"), t("Baritone ukulele"), {
        tuning("standard", t("Standard"), QStringLiteral("D G B E"), {50, 55, 59, 64}),
    }});

    out.push_back({QStringLiteral("banjo"), t("Banjo (5-string)"), {
        tuning("open-g",    t("Open G"),          QStringLiteral("g D G B D"),
               {67, 50, 55, 59, 62}),
        tuning("double-c",  t("Double C"),        QStringLiteral("g C G C D"),
               {67, 48, 55, 60, 62}),
        tuning("sawmill",   t("Sawmill (G modal)"), QStringLiteral("g D G C D"),
               {67, 50, 55, 60, 62}),
        tuning("drop-c",    t("Drop C"),          QStringLiteral("g C G B D"),
               {67, 48, 55, 59, 62}),
    }});

    out.push_back({QStringLiteral("banjo-tenor"), t("Tenor banjo"), {
        tuning("standard", t("Standard"),    QStringLiteral("C G D A"), {48, 55, 62, 69}),
        tuning("irish",    t("Irish"),       QStringLiteral("G D A E"), {43, 50, 57, 64}),
    }});

    out.push_back({QStringLiteral("mandolin"), t("Mandolin"), {
        tuning("standard", t("Standard"), QStringLiteral("G D A E"), {55, 62, 69, 76}),
    }});

    out.push_back({QStringLiteral("violin"), t("Violin"), {
        tuning("standard", t("Standard"), QStringLiteral("G D A E"), {55, 62, 69, 76}),
    }});

    out.push_back({QStringLiteral("viola"), t("Viola"), {
        tuning("standard", t("Standard"), QStringLiteral("C G D A"), {48, 55, 62, 69}),
    }});

    out.push_back({QStringLiteral("cello"), t("Cello"), {
        tuning("standard", t("Standard"), QStringLiteral("C G D A"), {36, 43, 50, 57}),
    }});

    out.push_back({QStringLiteral("double-bass"), t("Double bass"), {
        tuning("standard", t("Standard"), QStringLiteral("E A D G"), {28, 33, 38, 43}),
        tuning("solo",     t("Solo"),     QStringLiteral("F♯ B E A"), {30, 35, 40, 45}),
    }});

    // No strings: every note is a target and the window names whatever it
    // hears. What you want for a wind instrument, a singer, or an instrument
    // in a tuning nobody thought to put in the list above.
    out.push_back({QStringLiteral("chromatic"), t("Chromatic"), {
        tuning("all", t("All notes"), QString(), {}),
    }});

    return out;
}

}  // namespace

const QList<InstrumentPreset> &instrumentPresets() {
    static const QList<InstrumentPreset> presets = buildPresets();
    return presets;
}

QString noteLetter(int midiNote) {
    static const char *const kNames[12] = {"C", "C♯", "D", "D♯", "E",  "F",
                                           "F♯", "G", "G♯", "A", "A♯", "B"};
    if (midiNote < 0) return QString();
    // Modulo that stays positive below MIDI 0, which the table never reaches
    // but the fractional maths above it can round into.
    const int pc = ((midiNote % 12) + 12) % 12;
    return QString::fromUtf8(kNames[pc]);
}

QString noteName(int midiNote) {
    if (midiNote < 0) return QString();
    // Scientific pitch notation: MIDI 60 is C4.
    const int octave = midiNote / 12 - 1;
    return noteLetter(midiNote) + QString::number(octave);
}

double noteFrequency(double midiNote, double referenceA4) {
    return referenceA4 * std::pow(2.0, (midiNote - 69.0) / 12.0);
}

double frequencyToMidi(double hz, double referenceA4) {
    if (hz <= 0.0 || referenceA4 <= 0.0) return 0.0;
    return 69.0 + 12.0 * std::log2(hz / referenceA4);
}
