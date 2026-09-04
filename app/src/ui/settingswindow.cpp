// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "settingswindow.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "mixerclient.h"
#include "theme.h"
#include "widgets.h"

namespace {

// How many entries each combo is built with. Anything past this was appended at
// runtime to show a value we do not offer, and is disposable.
constexpr int kLatencyPresets = 8;
constexpr int kHeadroomPresets = 8;

// Mirrors MixerService: the daemon answers with these for "the devices
// disagree" and "there is nothing to read".
constexpr int kHeadroomMixed = -1;
constexpr int kHeadroomUnknown = -2;

// A quantum or a headroom in frames, as the time it represents. Both are
// counted at the graph rate, so one helper covers both columns.
QString framesLabel(int frames) {
    if (frames <= 0) return QObject::tr("off");
    return QStringLiteral("%1 ms").arg(frames * 1000.0 / 48000.0, 0, 'f', 1);
}

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    l->setWordWrap(true);
    return l;
}

// The units this mixer's audio actually depends on, in the order they have to
// come up. Anything absent from a machine is reported as such rather than as a
// failure: pipewire-pulse is genuinely optional, and a system without it is not
// broken, it just has no PulseAudio applications.
struct ServiceRow {
    const char *unit;
    const char *label;
    const char *what;
};

const ServiceRow kServices[] = {
    {"pipewire", "PipeWire", "The audio server itself. Nothing works without it."},
    {"wireplumber", "WirePlumber",
     "Session manager: device discovery, and the rules that set output "
     "headroom and the graph clock."},
    {"pipewire-pulse", "PipeWire (PulseAudio)",
     "Compatibility layer. Most applications reach the mixer through it."},
    {"wavelined", "Waveline daemon",
     "Owns the graph, the mixes and noise suppression. The mixer window is "
     "only a client of it."},
};

}  // namespace

SettingsWindow::SettingsWindow(MixerClient *client, QWidget *parent)
    : QWidget(parent, Qt::Window), client_(client) {
    setWindowTitle(tr("Latency & Diagnostics"));
    resize(975, 775);
    setMinimumSize(620, 500);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(12);

    auto *title = new QLabel(tr("Latency & Diagnostics"), this);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.2);
    title->setFont(tf);
    outer->addWidget(title);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildLatencyTab(), tr("Latency"));
    tabs->addTab(buildDesktopTab(), tr("Desktop"));
    tabs->addTab(buildWarningsTab(), tr("Warnings"));
    tabs->addTab(buildDiagnosticsTab(), tr("Diagnostics"));
    outer->addWidget(tabs, 1);

    auto *buttons = new QHBoxLayout;
    // Copying matters more than it looks: this is the block a bug report needs,
    // and retyping a latency table by hand from a screenshot is how the numbers
    // arrive wrong.
    auto *copy = new QPushButton(tr("Copy diagnostics"), this);
    connect(copy, &QPushButton::clicked, this, [this] {
        QStringList lines;
        for (const QString &r : rows_) {
            const QStringList f = r.split(QLatin1Char('\t'));
            QString line = f.value(0) + QLatin1String(": ") + f.value(1);
            if (f.size() > 2 && !f[2].isEmpty())
                line += QLatin1String("  (") + f[2] + QLatin1Char(')');
            lines << line;
        }
        QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    });
    buttons->addWidget(copy);
    buttons->addStretch();
    auto *close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    buttons->addWidget(close);
    outer->addLayout(buttons);

    poll_ = new QTimer(this);
    poll_->setInterval(2000);
    connect(poll_, &QTimer::timeout, this, &SettingsWindow::refresh);
}

// ------------------------------------------------------------- Latency tab

QWidget *SettingsWindow::buildLatencyTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(12);

    auto *graph = new Section(tr("Graph latency"), page);
    auto *graphLay = graph->contentLayout();
    graphLay->addWidget(dimLabel(
        tr("How much audio moves per PipeWire cycle, for every device on this "
           "machine. Lower is more responsive and asks more of the CPU; if you "
           "hear crackling, go up one."),
        page));

    latencyCombo_ = new QComboBox(page);
    // userData is the quantum in frames; 0 means "leave it alone", the only
    // entry that does not take a global setting away from someone who set it
    // themselves.
    latencyCombo_->addItem(tr("Automatic"), 0);
    latencyCombo_->addItem(tr("Insane (0.7 ms)"), 32);
    latencyCombo_->addItem(tr("Extreme (1.3 ms)"), 64);
    latencyCombo_->addItem(tr("Ultra low (2.7 ms)"), 128);
    latencyCombo_->addItem(tr("Low (5.3 ms)"), 256);
    latencyCombo_->addItem(tr("Balanced (10.7 ms)"), 512);
    latencyCombo_->addItem(tr("Relaxed (21.3 ms)"), 1024);
    latencyCombo_->addItem(tr("Pro Audio (85 ms)"), 4096);
    connect(latencyCombo_, &QComboBox::activated, this,
            &SettingsWindow::onLatencyPicked);
    graphLay->addWidget(latencyCombo_);
    graphLay->addWidget(dimLabel(
        tr("Applies instantly: nothing restarts and no audio is interrupted. "
           "Every input's capture hop is reopened afterwards, which is silent "
           "and is what keeps a quantum change from leaving an input sounding "
           "robotic."),
        page));
    // The one setting a DAW cannot work around. Every entry except Automatic
    // pins clock.force-quantum, and that overrides the buffer size a JACK
    // client asks for -- so Ardour, Reaper and friends are held at whatever is
    // chosen here, their own buffer-size control silently does nothing, and the
    // result is crackling that looks like it comes from the DAW.
    graphLay->addWidget(dimLabel(
        tr("Using a DAW? Set this to Automatic. Every other setting pins the "
           "buffer size for the whole machine and overrides what Ardour, Reaper "
           "or Bitwig ask for — their own buffer-size setting stops working and "
           "you get crackling. Automatic hands that choice back to the DAW."),
        page));
    lay->addWidget(graph);

    auto *headroom = new Section(tr("Non-USB output headroom"), page);
    auto *headLay = headroom->contentLayout();
    headLay->addWidget(dimLabel(
        tr("Extra buffering for output devices that are not driving the graph "
           "clock, typically motherboard and HDMI outputs. Those devices "
           "resample onto another device's clock and crackle under load "
           "without some slack; the device driving the clock never needs any."),
        page));

    headroomCombo_ = new QComboBox(page);
    // userData is headroom in frames. 0 removes the rule file entirely rather
    // than writing a zero, so "Off" leaves WirePlumber as it found it.
    headroomCombo_->addItem(tr("Off"), 0);
    headroomCombo_->addItem(tr("Tiny (0.7 ms)"), 32);
    headroomCombo_->addItem(tr("Minimal (1.3 ms)"), 64);
    headroomCombo_->addItem(tr("Small (2.7 ms)"), 128);
    headroomCombo_->addItem(tr("Medium (5.3 ms)"), 256);
    headroomCombo_->addItem(tr("Large (10.7 ms)"), 512);
    headroomCombo_->addItem(tr("Recommended (21.3 ms)"), 1024);
    headroomCombo_->addItem(tr("Maximum (42.7 ms)"), 2048);
    connect(headroomCombo_, &QComboBox::activated, this,
            &SettingsWindow::onHeadroomPicked);
    headLay->addWidget(headroomCombo_);
    headLay->addWidget(dimLabel(
        tr("USB outputs are left alone on purpose. Many USB microphones are a "
           "microphone and a headphone output on one card, and a rule touching "
           "the output side can stop the microphone working until it is "
           "replugged. Needs a WirePlumber restart, which briefly stops every "
           "stream on the machine."),
        page));

    // Its own row rather than a tooltip. "I set it and nothing changed" is
    // indistinguishable from "I set it and skipped the restart" unless the
    // panel says which one happened.
    headroomPending_ = new QLabel(page);
    headroomPending_->setWordWrap(true);
    headroomPending_->setVisible(false);
    QPalette hp = headroomPending_->palette();
    hp.setColor(QPalette::WindowText, Theme::Warn);
    headroomPending_->setPalette(hp);
    headLay->addWidget(headroomPending_);
    lay->addWidget(headroom);

    // The third latency control, and the one that decides whether the other two
    // can be believed: a 2.7 ms cycle is only achievable if the threads serving
    // it are scheduled ahead of ordinary work. It sits here rather than in
    // Diagnostics because it is a setting, and next to the quantum because that
    // is the number it constrains.
    auto *rt = new Section(tr("Real-time scheduling"), page);
    auto *rtLay = rt->contentLayout();
    rtLay->addWidget(dimLabel(
        tr("Waveline's audio threads normally run at real-time priority so they "
           "meet their deadline even while the machine is busy. Turning this "
           "off makes them ordinary threads: the graph still works, but a "
           "loaded CPU can delay them, which is heard as clicks and pops -- "
           "soonest at the smaller buffer sizes above."),
        page));

    realtimeCheck_ = new QCheckBox(
        tr("Run audio threads at real-time priority"), page);
    // Checked before the signal is connected, so the window opens showing the
    // setting's own default rather than Qt's. It matters when the daemon is not
    // up yet: syncRealtimeCheck() declines to guess in that state, and an
    // unchecked box would be a claim that real-time is off.
    realtimeCheck_->setChecked(true);
    connect(realtimeCheck_, &QCheckBox::toggled, this,
            &SettingsWindow::onRealtimeToggled);
    rtLay->addWidget(realtimeCheck_);
    rtLay->addWidget(dimLabel(
        tr("Turn this off if real-time audio threads are starving something "
           "else on this machine, or to find out whether Waveline is what is "
           "stuttering. Changing it rebuilds the audio graph, which stops "
           "Waveline's audio for a few seconds."),
        page));
    lay->addWidget(rt);

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------- Desktop tab

QWidget *SettingsWindow::buildDesktopTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(12);

    auto *shell = new Section(tr("Desktop panel"), page);
    auto *shellLay = shell->contentLayout();
    shellLay->addWidget(dimLabel(
        tr("Some desktop shells can drive this mixer from their own audio "
           "applet — output levels per Monitor output, input levels per "
           "device, and which applications are really using a microphone. "
           "Waveline offers that whether or not such a shell is installed, and "
           "needs none of it to work."),
        page));

    shellStatus_ = new QLabel(page);
    shellStatus_->setWordWrap(true);
    shellLay->addWidget(shellStatus_);
    lay->addWidget(shell);

    auto *nc = new Section(tr("Noise suppression in the panel"), page);
    auto *ncLay = nc->contentLayout();
    ncLay->addWidget(dimLabel(
        tr("How many input devices get a noise-suppression switch in the "
           "shell's microphone panel, counted from the top of the input list. "
           "Every device's noise suppression is always available here in the "
           "mixer; this only decides how many are worth a switch in a panel "
           "meant to be read at a glance."),
        page));

    auto *row = new QHBoxLayout;
    shellNcSpin_ = new QSpinBox(page);
    shellNcSpin_->setRange(0, 16);
    shellNcSpin_->setSpecialValueText(tr("None"));
    // Set before the signal is connected, so opening the window with no daemon
    // shows the setting's own default rather than a claim of zero.
    shellNcSpin_->setValue(1);
    connect(shellNcSpin_, &QSpinBox::valueChanged, this,
            &SettingsWindow::onShellNcInputsChanged);
    row->addWidget(new QLabel(tr("Input devices with a switch:"), page));
    row->addWidget(shellNcSpin_);
    row->addStretch(1);
    ncLay->addLayout(row);
    ncLay->addWidget(dimLabel(
        tr("One by default: a single-microphone machine gets exactly the "
           "switch it wants and no rows it does not. Raise it for a setup "
           "where more than one microphone is switched on and off during a "
           "session."),
        page));
    lay->addWidget(nc);

    lay->addStretch(1);
    return page;
}

void SettingsWindow::onShellNcInputsChanged(int count) {
    if (syncing_ || !client_) return;
    client_->setShellNoiseSuppressionInputs(count);
}

void SettingsWindow::syncDesktopTab() {
    if (!client_) return;
    syncing_ = true;
    if (shellNcSpin_) shellNcSpin_->setValue(client_->shellNoiseSuppressionInputs());
    syncing_ = false;

    if (shellStatus_) {
        const bool present = client_->shellClientPresent();
        shellStatus_->setText(present
                                  ? tr("A desktop shell is connected to this mixer.")
                                  : tr("No desktop shell is connected right now."));
        QPalette p = shellStatus_->palette();
        p.setColor(QPalette::WindowText, present ? Theme::Accent : Theme::TextDim);
        shellStatus_->setPalette(p);
    }
}

void SettingsWindow::onLatencyPicked(int index) {
    if (syncing_ || !latencyCombo_) return;
    client_->setGraphQuantum(latencyCombo_->itemData(index).toInt());
    // Nothing redrawn here on purpose. The daemon answers once it has actually
    // applied the value, and syncLatencyCombo() then shows what happened --
    // including the case where PipeWire declined.
}

void SettingsWindow::onHeadroomPicked(int index) {
    if (syncing_ || !headroomCombo_) return;
    const int frames = headroomCombo_->itemData(index).toInt();
    if (frames == client_->outputHeadroom()) return;
    client_->setOutputHeadroom(frames);

    // Asked, never assumed. A WirePlumber restart drops every stream on the
    // machine for a moment -- a call, a recording, a game -- and doing that as
    // a side effect of touching a dropdown is not a trade this panel gets to
    // make on the user's behalf.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Restart WirePlumber?"));
    box.setText(tr("Output headroom is set when a device is opened, so this "
                   "takes effect after WirePlumber restarts."));
    box.setInformativeText(
        tr("Restarting stops every audio stream on this machine for a moment. "
           "The setting is saved either way and will apply on your next login "
           "if you skip it."));
    QPushButton *now = box.addButton(tr("Restart now"), QMessageBox::AcceptRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(now);
    box.exec();
    if (box.clickedButton() == now) client_->restartWirePlumber();
    refresh();
}

void SettingsWindow::onRealtimeToggled(bool on) {
    if (syncing_ || !realtimeCheck_) return;
    if (on == client_->realtimeScheduling()) return;

    // Asked before anything is written, and unlike the headroom restart this
    // one cannot be deferred: a PipeWire context keeps whatever scheduling it
    // was created with, so there is no state where the setting is saved and
    // merely waiting. It either applies now or it is not a setting yet -- so
    // Cancel puts the box back rather than leaving a promise the panel would
    // then have to explain.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Rebuild the audio graph?"));
    box.setText(on ? tr("Turning real-time scheduling back on rebuilds the "
                        "audio graph.")
                   : tr("Turning real-time scheduling off rebuilds the audio "
                        "graph."));
    box.setInformativeText(
        tr("Waveline's audio stops for a few seconds while the mixer daemon "
           "restarts. Your levels, routing and effects are all kept."));
    QPushButton *go = box.addButton(tr("Rebuild now"), QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();

    if (box.clickedButton() != go) {
        // Straight back to what the daemon still holds. Guarded, or this write
        // re-enters as another toggle and asks again.
        syncRealtimeCheck();
        return;
    }

    client_->setRealtimeScheduling(on);
    if (!client_->restartDaemon()) {
        QMessageBox::warning(
            this, tr("Could not restart the daemon"),
            tr("The setting was saved, but `systemctl --user restart "
               "wavelined` could not be started. It will apply the next time "
               "the mixer daemon starts."));
    }
    refresh();
}

void SettingsWindow::onDspProfilingToggled(bool on) {
    if (syncing_ || !dspProfilingCheck_) return;
    if (on == client_->dspProfiling()) return;
    client_->setDspProfiling(on);
    // Redrawn at once rather than left to the next tick. The rows this switch
    // adds and removes are the only visible evidence it did anything, and two
    // seconds of an unchanged table reads as a control that does nothing.
    rowsSignature_.clear();
    refreshDiagnostics();
}

void SettingsWindow::syncDspProfilingCheck() {
    if (!dspProfilingCheck_) return;
    if (!client_->available()) return;
    syncing_ = true;
    dspProfilingCheck_->setChecked(client_->dspProfiling());
    syncing_ = false;
}

void SettingsWindow::syncRealtimeCheck() {
    if (!realtimeCheck_) return;
    // Not while the daemon is away, which is exactly what the seconds after a
    // rebuild look like. realtimeScheduling() answers with its default then, so
    // syncing here would tick the box back on in front of the user who just
    // turned it off and is watching the restart happen.
    if (!client_->available()) return;
    syncing_ = true;
    realtimeCheck_->setChecked(client_->realtimeScheduling());
    syncing_ = false;
}

void SettingsWindow::syncLatencyCombo() {
    if (!latencyCombo_) return;
    const int pinned = client_->graphQuantum();
    const int effective = client_->effectiveGraphQuantum();

    syncing_ = true;
    // "Automatic" is a real state, not an absence of one, so it says what it
    // resolved to. Without this the entry a fresh install lands on is the only
    // one that does not tell you your latency.
    latencyCombo_->setItemText(
        0, effective > 0 ? tr("Automatic (%1)").arg(framesLabel(effective))
                         : tr("Automatic"));
    while (latencyCombo_->count() > kLatencyPresets)
        latencyCombo_->removeItem(latencyCombo_->count() - 1);

    int index = latencyCombo_->findData(pinned);
    if (index < 0) {
        // A quantum pinned outside the offered set -- someone's own
        // pw-metadata, or a hand-edited config. Shown rather than snapped to
        // the nearest entry: silently relabelling a user's setting as one of
        // ours is how a control lies about the system.
        latencyCombo_->addItem(tr("Custom (%1)").arg(framesLabel(pinned)), pinned);
        index = latencyCombo_->count() - 1;
    }
    latencyCombo_->setCurrentIndex(index);
    syncing_ = false;
}

void SettingsWindow::syncHeadroomCombo() {
    if (!headroomCombo_) return;
    const int stored = client_->outputHeadroom();
    const int live = client_->effectiveOutputHeadroom();

    syncing_ = true;
    while (headroomCombo_->count() > kHeadroomPresets)
        headroomCombo_->removeItem(headroomCombo_->count() - 1);
    int index = headroomCombo_->findData(stored);
    if (index < 0) {
        headroomCombo_->addItem(tr("Custom (%1)").arg(framesLabel(stored)), stored);
        index = headroomCombo_->count() - 1;
    }
    headroomCombo_->setCurrentIndex(index);
    syncing_ = false;

    // kHeadroomUnknown is not a mismatch: there is either no PCI output on this
    // machine for the rule to apply to, or none has been read yet. Claiming a
    // pending restart in that state is how a panel cries wolf.
    if (live == kHeadroomUnknown) {
        headroomPending_->setVisible(false);
        return;
    }
    // Devices disagreeing with each other carries the same message as
    // disagreeing with the setting: something has not been reopened.
    const bool pending = (live == kHeadroomMixed) || (live != stored);
    headroomPending_->setVisible(pending);
    if (pending) {
        headroomPending_->setText(
            live == kHeadroomMixed
                ? tr("Waiting for a WirePlumber restart. Your outputs are "
                     "currently open with different amounts of headroom.")
                : tr("Waiting for a WirePlumber restart. Your outputs are "
                     "currently open with %1, not %2.")
                      .arg(framesLabel(live), framesLabel(stored)));
    }
}

// ------------------------------------------------------------ Warnings tab

QWidget *SettingsWindow::buildWarningsTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(12);

    lay->addWidget(dimLabel(
        tr("Things Waveline noticed and stopped acting on. Everything here is "
           "reported rather than fought: two programs writing the same routing "
           "key in a loop is a permanent stream of moves and an audible one, "
           "so the router gives up. This list is so that it does not give up "
           "silently."),
        page));

    auto *card = new Section(tr("Active"), page);
    warningsLay_ = card->contentLayout();
    warningsEmpty_ = dimLabel(tr("Nothing to report."), page);
    warningsLay_->addWidget(warningsEmpty_);
    lay->addWidget(card);

    lay->addStretch(1);
    return page;
}

void SettingsWindow::refreshWarnings() {
    if (!warningsLay_) return;
    const QList<StreamConflictInfo> conflicts =
        client_->available() ? client_->streamRoutingConflicts()
                             : QList<StreamConflictInfo>{};
    const QStringList silenced =
        client_->available() ? client_->dismissedStreamRoutingConflicts()
                             : QStringList{};

    QString sig;
    for (const StreamConflictInfo &c : conflicts)
        sig += c.sinkName + QLatin1Char('|') + c.appName + QLatin1Char(';');
    sig += QLatin1Char('#') + silenced.join(QLatin1Char(','));
    if (sig == warningsSignature_) return;
    warningsSignature_ = sig;

    while (QLayoutItem *item = warningsLay_->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }
    warningsEmpty_ = nullptr;

    for (const StreamConflictInfo &c : conflicts) {
        auto *row = new QWidget(this);
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(10);
        // Named by where it went, not by who moved it: PipeWire records the
        // destination and not the mover, and saying more than we know is how a
        // diagnostic sends someone after the wrong program.
        auto *text = new QLabel(
            tr("“%1” keeps being moved off the %2 channel to “%3”.")
                .arg(c.appName, c.channelId, c.sinkLabel),
            row);
        text->setWordWrap(true);
        text->setTextFormat(Qt::PlainText);
        rowLay->addWidget(text, 1);
        auto *dismiss = new QPushButton(tr("Don't warn me"), row);
        dismiss->setToolTip(
            tr("Stops this warning for “%1” for good. Undone below.")
                .arg(c.sinkLabel));
        const QString sink = c.sinkName;
        connect(dismiss, &QPushButton::clicked, this, [this, sink] {
            client_->dismissStreamRoutingConflict(sink);
            warningsSignature_.clear();
            refreshWarnings();
        });
        rowLay->addWidget(dismiss);
        warningsLay_->addWidget(row);
    }

    // The way back from "don't warn me about this again". A dismissal that can
    // only be undone by editing config.json is a dismissal a user will not
    // undo, and this panel is already where someone goes when routing is
    // behaving oddly.
    if (!silenced.isEmpty()) {
        auto *row = new QWidget(this);
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(10);
        rowLay->addWidget(
            dimLabel(tr("Silenced for: %1").arg(silenced.join(QLatin1String(", "))),
                     row),
            1);
        auto *restore = new QPushButton(tr("Show these again"), row);
        connect(restore, &QPushButton::clicked, this, [this] {
            client_->clearStreamRoutingConflictDismissals();
            warningsSignature_.clear();
            refreshWarnings();
        });
        rowLay->addWidget(restore);
        warningsLay_->addWidget(row);
    }

    if (conflicts.isEmpty() && silenced.isEmpty()) {
        warningsEmpty_ = dimLabel(tr("Nothing to report."), this);
        warningsLay_->addWidget(warningsEmpty_);
    }
}

// --------------------------------------------------------- Diagnostics tab

QWidget *SettingsWindow::buildDiagnosticsTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(12);

    auto *services = new Section(tr("Services"), page);
    servicesLay_ = services->contentLayout();
    lay->addWidget(services);

    // In this tab rather than beside the latency controls, because it produces
    // rows in the table below it rather than changing how anything sounds. It
    // is the switch that decides how much the table has to say.
    auto *profiling = new Section(tr("DSP profiling"), page);
    auto *profLay = profiling->contentLayout();
    profLay->addWidget(dimLabel(
        tr("Times every effect Waveline runs and counts the cycles PipeWire "
           "says it missed, then reports both per input device below. This is "
           "how you find out whether the noise suppression or the EQ is the "
           "expensive one, and whether the crackling is Waveline's processing "
           "or the buffer size."),
        page));

    dspProfilingCheck_ =
        new QCheckBox(tr("Measure per-device DSP cost and missed cycles"), page);
    connect(dspProfilingCheck_, &QCheckBox::toggled, this,
            &SettingsWindow::onDspProfilingToggled);
    profLay->addWidget(dspProfilingCheck_);
    profLay->addWidget(dimLabel(
        tr("Applies instantly and interrupts nothing. Leave it off when you "
           "are not reading the numbers: the cost is small — two clock reads "
           "per effect per cycle — but it is charged to the audio threads, and "
           "\"does it still glitch with this off\" should stay a question you "
           "can answer. Switching it on starts the counts again from zero."),
        page));
    lay->addWidget(profiling);

    lay->addWidget(dimLabel(
        tr("Latency figures below are measured from ALSA, not calculated from "
           "settings. They cover everything from the device handing audio to "
           "this machine onwards, and nothing the device did before that. A "
           "camera or headset running its own noise suppression can add far "
           "more upstream of anything Linux can see."),
        page));

    table_ = new QTableWidget(0, 3, page);
    table_->setHorizontalHeaderLabels({tr("What"), tr("Value"), tr("Detail")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setWordWrap(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    lay->addWidget(table_, 1);

    tableEmpty_ = dimLabel(
        tr("wavelined is not running, so there is nothing to report."), page);
    tableEmpty_->setVisible(false);
    lay->addWidget(tableEmpty_);
    return page;
}

void SettingsWindow::refreshServices() {
    if (!servicesLay_) return;

    // One systemctl for the lot. Four processes on a 2 s timer would be four
    // times the cost for the same three lines of output.
    QStringList units;
    for (const ServiceRow &s : kServices) units << QLatin1String(s.unit);
    QProcess p;
    p.start(QStringLiteral("systemctl"),
            QStringList{QStringLiteral("--user"), QStringLiteral("is-active")} + units);
    p.waitForFinished(1500);
    const QStringList states =
        QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'));

    QString sig = states.join(QLatin1Char('|'));
    sig += client_->available() ? QLatin1String("+bus") : QLatin1String("-bus");
    if (sig == servicesSignature_) return;
    servicesSignature_ = sig;

    while (QLayoutItem *item = servicesLay_->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }

    int i = 0;
    for (const ServiceRow &s : kServices) {
        const QString state = states.value(i).trimmed();
        ++i;
        const bool active = state == QLatin1String("active");
        const bool absent = state.isEmpty() || state == QLatin1String("inactive");

        auto *row = new QWidget(this);
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(8);

        auto *dot = new StatusDot(row);
        dot->setColor(active ? Theme::Accent
                             : (absent ? Theme::TextFaint : Theme::Danger));
        rowLay->addWidget(dot);

        auto *name = new QLabel(tr(s.label), row);
        QFont nf = name->font();
        nf.setBold(true);
        name->setFont(nf);
        name->setMinimumWidth(170);
        rowLay->addWidget(name);

        QString status = state.isEmpty() ? tr("not installed") : state;
        // The daemon is the one service this window can check twice: the unit
        // can be active while the bus name is not answering, and that gap is
        // exactly the state where the mixer looks broken for no visible reason.
        if (qstrcmp(s.unit, "wavelined") == 0 && active && !client_->available())
            status = tr("running, not answering on D-Bus");
        auto *value = new QLabel(status, row);
        rowLay->addWidget(value);
        rowLay->addStretch(1);

        if (!active) {
            auto *hint = new QLabel(
                QStringLiteral("systemctl --user start %1").arg(QLatin1String(s.unit)),
                row);
            hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
            QPalette hp = hint->palette();
            hp.setColor(QPalette::WindowText, Theme::TextDim);
            hint->setPalette(hp);
            rowLay->addWidget(hint);
        }
        servicesLay_->addWidget(row);

        auto *what = dimLabel(tr(s.what), this);
        what->setContentsMargins(18, 0, 0, 4);
        servicesLay_->addWidget(what);
    }
}

void SettingsWindow::refreshDiagnostics() {
    if (!table_) return;
    rows_ = client_->available() ? client_->graphDiagnostics() : QStringList{};

    const QString sig = rows_.join(QLatin1Char('\n'));
    if (sig == rowsSignature_) return;
    rowsSignature_ = sig;

    table_->setVisible(!rows_.isEmpty());
    tableEmpty_->setVisible(rows_.isEmpty());

    table_->setRowCount(rows_.size());
    for (int r = 0; r < rows_.size(); ++r) {
        const QStringList f = rows_[r].split(QLatin1Char('\t'));
        for (int c = 0; c < 3; ++c)
            table_->setItem(r, c,
                            new QTableWidgetItem(c < f.size() ? f[c] : QString()));
    }
    table_->resizeRowsToContents();
}

void SettingsWindow::refresh() {
    syncLatencyCombo();
    syncDesktopTab();
    syncHeadroomCombo();
    syncRealtimeCheck();
    syncDspProfilingCheck();
    refreshWarnings();
    refreshServices();
    refreshDiagnostics();
}

void SettingsWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // Everything is signature-compared, so force one full pass: a panel that
    // opens showing what it last saw is a panel that lies for two seconds.
    warningsSignature_.clear();
    servicesSignature_.clear();
    rowsSignature_.clear();
    refresh();
    poll_->start();
}

void SettingsWindow::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    poll_->stop();
}
