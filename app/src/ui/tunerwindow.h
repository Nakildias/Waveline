// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The instrument tuner.
//
// The daemon does the listening -- the GUI links no PipeWire -- so this window
// is a source picker, a note table and a needle. It starts the daemon's tuner
// when it opens and stops it when it closes, which is the whole of its effect
// on anything else: the tap is not part of the mixer graph and routing carries
// on untouched while it is up.

#pragma once

#include <QAbstractButton>
#include <QWidget>

#include "tunerdata.h"

class CardBase;
class LevelMeter;
class MixerClient;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QTimer;

// -------------------------------------------------------------- TunerDial
// The arc, the needle, and the note in the middle of it.
//
// setCents() sets a target the needle chases, for the same reason LevelMeter
// interpolates: readings land ~30 times a second and a needle that teleports
// between them reads as noise rather than as pitch.
class TunerDial : public QWidget {
    Q_OBJECT

public:
    explicit TunerDial(QWidget *parent = nullptr);

    // Nothing usable being heard. `hint` says what to do about it.
    void setIdle(const QString &hint);
    // `cents` is the real offset, not clamped: the caller's readout wants the
    // true number even when the needle has run out of arc.
    void setReading(const QString &note, const QString &detail, double cents,
                    bool inTune);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void animate();

    QString note_;
    QString detail_;
    QString hint_;
    double cents_ = 0.0;    // where the audio says the needle belongs
    double drawn_ = 0.0;    // where it is right now
    bool idle_ = true;
    bool inTune_ = false;
    QTimer *anim_ = nullptr;
};

// ------------------------------------------------------------ StringButton
// One string of the current tuning: the letter big, the octave small under it.
// Checked means "this is the note being tuned to"; in-tune paints it green.
class StringButton : public QAbstractButton {
    Q_OBJECT

public:
    explicit StringButton(int midiNote, QWidget *parent = nullptr);

    int midiNote() const { return midiNote_; }
    void setInTune(bool on);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    int midiNote_;
    bool inTune_ = false;
    bool hover_ = false;
};

// ------------------------------------------------------------- TunerWindow
class TunerWindow : public QWidget {
    Q_OBJECT

public:
    explicit TunerWindow(MixerClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private slots:
    // The daemon's node list changed: a microphone was plugged in, or the
    // daemon came back. Keeps the current selection if it is still there.
    void refreshSources();
    void onSourcePicked();
    void onInstrumentPicked();
    void onTuningPicked();
    void onTick();

private:
    QWidget *buildControls();
    QWidget *buildStrings();
    void rebuildTunings();
    void rebuildStringButtons();
    void applyMode(bool automatic);
    void selectString(int index);
    void restartTuner();
    void setStatus(const QString &text, bool error);
    void loadSettings();
    void saveSettings() const;

    const TuningPreset *currentTuning() const;
    double referenceA4() const;

    MixerClient *client_ = nullptr;

    QComboBox *sourceBox_ = nullptr;
    QComboBox *instrumentBox_ = nullptr;
    QComboBox *tuningBox_ = nullptr;
    QComboBox *referenceBox_ = nullptr;
    QPushButton *autoBtn_ = nullptr;
    QPushButton *manualBtn_ = nullptr;
    LevelMeter *inputMeter_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    TunerDial *dial_ = nullptr;
    QLabel *detectedLabel_ = nullptr;
    QLabel *targetLabel_ = nullptr;
    QHBoxLayout *stringRow_ = nullptr;
    QList<StringButton *> stringButtons_;

    QTimer *poll_ = nullptr;

    // What the tuner was last asked to listen to, so a source list refresh
    // that does not actually change the selection does not restart the tap.
    QString startedKind_;
    QString startedSource_;

    // The source list as it was last built. Compared before touching the combo
    // so a state poll does not rebuild a list the user may have open. Held
    // separately from a "have we ever built it" flag, because the empty list
    // is a real state -- no inputs, or no daemon -- and must not look like the
    // list not having been read yet.
    QStringList sourceSignature_;
    bool sourceSignatureValid_ = false;

    // Saved between sessions: nobody changes instrument as often as they open
    // this window.
    QString wantInstrument_ = QStringLiteral("guitar");
    QString wantTuning_ = QStringLiteral("standard");
    QString wantSourceId_;
    QString wantSourceKind_;
    int wantReference_ = 440;

    bool automatic_ = true;
    int selected_ = 0;
    // Milliseconds since the last usable reading. A plucked note decays below
    // the detector's floor long before the user has finished looking at it, so
    // the display holds briefly rather than blanking between plucks.
    int silentMs_ = 0;
    // Held so a note that passes through tune on its way somewhere else still
    // registers as having got there.
    int inTuneMs_ = 0;
};
