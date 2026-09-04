// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Virtual Rack: a physical-gear-style alternate view of one input
// device's Creative FX chain, opened by middle-clicking that device's
// heartbeat button. Same underlying data as the Microphone > Creative tab in
// Global Effects (client_->masterCreativeFx/setMasterCreativeFx) -- this is
// a second control surface for it, not a second copy of it.

#pragma once

#include <QHash>
#include <QWidget>

#include <functional>

#include "mixerclient.h"

class MixerClient;
class Knob;
class PowerSwitch;
class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;
class QVBoxLayout;

// One rack unit: a title, a grip handle for drag-reordering, a row of
// per-effect Knobs, an I/O power switch and a remove button. Each concrete
// effect subclass below only has to describe its own knobs and read/write
// its own slice of CreativeFxInfo -- the chassis (grip, power, remove,
// layout) is shared here so it looks and behaves identically across all
// eight.
class RackModule : public QWidget {
    Q_OBJECT

public:
    RackModule(int stageIndex, const QString &title, const QString &tip,
              QWidget *parent = nullptr);

    int stageIndex() const { return stageIndex_; }
    QWidget *grip() const { return grip_; }
    PowerSwitch *power() const { return power_; }

    // Writes this module's live control values (including power()'s state)
    // into the matching slice of `v`.
    virtual void applyTo(CreativeFxInfo &v) const = 0;
    // Sets every control from `v`, without emitting changed().
    virtual void loadFrom(const CreativeFxInfo &v) = 0;
    // Restores this module's own slice back to factory defaults (including
    // powering it off, matching the tab editor's per-effect Default button)
    // and pushes the change. Generic: it works by loading a default-
    // constructed CreativeFxInfo through the same loadFrom() every subclass
    // already implements, so nothing per-effect is needed here.
    void resetToDefaults();

signals:
    void removeRequested(int stageIndex);
    void changed();

protected:
    struct KnobUi {
        Knob *knob = nullptr;
        QLabel *readout = nullptr;
    };
    // Adds one labeled Knob + numeric readout to the module's controls row.
    // `format` turns the knob's raw int value into the readout text.
    // `quantizeSteps`, when >1, snaps the knob to the nearest of that many
    // evenly-spaced stops (min..max) as soon as it moves, rather than
    // resting anywhere in between -- for controls like Ping-Pong or
    // Tremolo's Shape where every intermediate value would otherwise read
    // as a flickering half-state during a drag.
    KnobUi addKnob(const QString &label, int min, int max,
                  const std::function<QString(int)> &format, int quantizeSteps = 0);
    void emitChanged();

    QHBoxLayout *controlsLay_ = nullptr;
    bool loading_ = false;

private:
    int stageIndex_;
    QWidget *grip_ = nullptr;
    PowerSwitch *power_ = nullptr;
};

class VirtualRackWindow : public QWidget {
    Q_OBJECT

public:
    VirtualRackWindow(MixerClient *client, const QString &masterId, const QString &title,
                      QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;
    // Rack Mode tracks the window's own visibility rather than a manual
    // switch: opening the Rack is the thing that means "drive this device
    // from the Rack now," so it takes effect the moment the window is open
    // and reverts the moment it closes, no separate control to leave in the
    // wrong position by accident.
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    // Creates and inserts a module widget for one effect, seeded from
    // current_, without pushing anything to the daemon -- used both by the
    // "+" menu (where the effect's own values are already correct as-is)
    // and by refresh() (where nothing should be written back just because a
    // module became visible).
    RackModule *addModuleWidget(int stageIndex);
    void onAddRequested(int stageIndex);
    void onRemoveRequested(int stageIndex);
    void onModuleChanged();
    // Walks the modules currently in the rack layout (which is the user's
    // chosen order after any drag), writes each one's live values into
    // current_, rebuilds current_.order from that same sequence with the
    // not-currently-racked effects appended after, and pushes the result.
    void syncFromModulesAndPush();
    void rebuildAddMenu();
    int visualIndexFor(int stageIndex) const;

    // Removes every module widget currently in the rack, without touching
    // current_ or pushing anything -- the shared first step of any operation
    // that replaces the whole rack wholesale (loading a preset), as opposed
    // to onRemoveRequested()'s single-module removal.
    void clearAllModules();
    // Rebuilds the Presets menu's contents from RackPresetStore::instance().
    // Unlike rebuildAddMenu(), this only ever needs to run right after an
    // action that actually changed the preset list (save/rename/delete/
    // import), never on the poll-driven refresh() tick, so it needs no
    // signature-based throttling to avoid closing an open menu.
    void rebuildPresetsMenu();
    void onSavePresetAs();
    void onLoadPreset(const QString &name);
    void onRenamePreset(const QString &name);
    void onDeletePreset(const QString &name);
    void onExportPreset(const QString &name);
    void onImportPresetFromFile();
    // Prompts for a preset name, re-asking on a clash (with an optional
    // replace confirmation) rather than refusing outright -- `allowed` is a
    // name that should not itself count as a clash (the preset's own current
    // name, when renaming it).
    QString askForPresetName(const QString &title, const QString &label,
                             const QString &initial, const QString &allowed = QString());

    MixerClient *client_ = nullptr;
    QString masterId_;
    QVBoxLayout *modulesLay_ = nullptr;
    QWidget *modulesContainer_ = nullptr;
    QPushButton *addBtn_ = nullptr;
    QMenu *addMenu_ = nullptr;
    QPushButton *presetsBtn_ = nullptr;
    QMenu *presetsMenu_ = nullptr;
    QString lastPresetDir_;
    QHash<int, RackModule *> modules_;
    CreativeFxInfo current_;
    RackModule *dragModule_ = nullptr;
    bool updating_ = false;
    // Bitmask (bit i = stage i present) as of the last rebuildAddMenu() that
    // actually touched the menu. refresh() runs on every daemon poll tick
    // (roughly 2.5x/sec) whether or not anything changed, and clearing a
    // QMenu that's currently open closes it -- so the menu is only rebuilt
    // when presence has genuinely changed since last time.
    int presentSignature_ = -1;
};
