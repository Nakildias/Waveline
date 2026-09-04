// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// One input, as a card: identity tile, meter, and two vertical faders.
//
// The left fader is Monitor (what you hear) and the right is Stream (what your
// audience hears). Keeping them side by side rather than stacked is what makes
// a wall of channels comparable at a glance, and that separation is the point
// of the whole application.

#pragma once

#include <QColor>
#include <QString>

#include "widgets.h"

class Fader;
class IconToggle;
class QHBoxLayout;
class LevelMeter;
class EqualToggle;
class QLabel;
class QPushButton;
class QSlider;
class QVBoxLayout;

class ChannelStrip : public CardBase {
    Q_OBJECT

public:
    ChannelStrip(const QString &id, const QString &name, QWidget *parent = nullptr);

    const QString &channelId() const { return id_; }
    void setDisplayName(const QString &name);
    void setMasterSlotNumber(int slot);
    // How far behind real time this input runs, in microseconds; negative for
    // "nothing to report". Shown under the slot number on hover -- it is the
    // number that decides whether an input is usable for live monitoring, and
    // nothing else in the window says what it is.
    // -1 means "nothing to report"; kLatencyHidden means "this device decides
    // its own delay internally and no host-side figure is the truth", which is
    // shown as N/A rather than as a number. Mirrors MixerService::
    // kLatencyHidden; declared here so the strip does not have to know about
    // the D-Bus client.
    static constexpr qint64 kLatencyHidden = -2;
    void setInputLatencyUs(qint64 us);
    // Secondary masters only: trash beside the slot number (overlay, no layout shift).
    void enableMasterDelete(bool on = true);
    // All masters: hammer opposite the trash, rebuilds the ALSA→DSP hop.
    void enableMasterRebuild(bool on = true);
    // Reparents btn onto this card and pins it to the right edge; nullptr clears.
    void setAddMasterOverlayButton(QPushButton *btn);
    void enableEditableTitle(bool on = true);
    // Icon tile, top stripe, and main meter tint — for master strips slotted by
    // position rather than channel id.
    void setIdentityBadge(const QString &iconName, const QColor &color);
    // This card's identity colour, for controls the window colours itself.
    QColor accentColor() const { return color_; }
    QString iconName() const { return iconName_; }

    // Updates without echoing signals back to the daemon.
    void setValues(double streamVol, bool streamMuted, double monitorVol,
                   bool monitorMuted);
    // 0..1 bar position for the card's meter.
    void setLevel(double fraction);
    // Second bar: this channel's published microphone, after its NC and EQ.
    // Greyed while the channel publishes no microphone.
    void setMicLevel(double fraction);
    void setMicMeterActive(bool on);
    // Input device strips only: the top bar is that device's own send into the
    // Monitor mix, so it greys out and reads zero while the device is not
    // being monitored. Channel strips leave it active.
    void setMainMeterActive(bool on);
    void setMainMeterTooltip(const QString &tip);
    // Input device strips only: the card's device has been unplugged. Drains
    // the colour out of the identity, the stripe and the meters -- the strip
    // keeps its place and its settings, and says plainly that nothing is
    // arriving. Hardware controls follow separately, since they are also gone
    // for a device that is present but has no vendor protocol.
    void setDeviceConnected(bool on);
    // Extra controls under the faders. The effects button lives here.
    QHBoxLayout *footerLayout() const { return footer_; }

    // What the third fader on the microphone card controls, which depends on
    // the microphone rather than on the layout:
    //
    //   PreampDb   a real analogue preamp reached over a vendor protocol,
    //              0..40 dB, read out in dB. Only the Wave:3 and its kind.
    //   InputLevel the capture device's own volume, 0..100%. Every other
    //              microphone -- the same control a volume applet shows.
    //
    // The signals are the same either way; only the units and the readout
    // differ, and gainChanged() carries the fader's value in its own units.
    enum class GainStyle { PreampDb, InputLevel };

    // Third vertical fader on the microphone card: gain and mute.
    void enableGainFader(bool on = true);
    void setGainStyle(GainStyle style);
    GainStyle gainStyle() const { return gainStyle_; }
    // The fader's value in the current style's units: dB, or percent.
    void setGainValue(double value);
    // Software mic send from the global mic path (channel strips only).
    void enableMicSendFader(bool on = true);
    void setMicSend(double level);
    void setMicMuted(bool muted);
    void setMicControlsActive(bool on);
    void enableEffectsButton(bool on = true);
    IconToggle *effectsButton() const { return fxButton_; }
    // The gear beside the heartbeat: opens this card's effects panel. A button
    // of its own rather than a modifier on the heartbeat, because a shortcut
    // nothing on screen mentions is a shortcut nobody finds.
    void enableSettingsButton(bool on = true);
    IconToggle *settingsButton() const { return settingsButton_; }
    // Ear toggle in the header: software monitor of this channel's published
    // microphone. Shown only while the channel publishes one.
    void enableMicMonitorButton(bool on = true);
    IconToggle *micMonitorButton() const { return micMonitorButton_; }
    void setMicMonitorButtonVisible(bool on);
    void setMicMonitorChecked(bool on);

    // Extra widget in the title row, right-aligned. The microphone card puts
    // the monitor-mode button here.
    void addHeaderWidget(QWidget *w);
    // Overrides both meter captions. The microphone card's bars mean something
    // different from a channel's, so the defaults would misdescribe them.
    void setMeterTooltips(const QString &main, const QString &mic);
    // Repoints the left column from "this input's level in the Monitor mix" to
    // the master level of the Monitor mix itself. Only the microphone card
    // does this; it still emits volumeChanged/muteToggled with mix "monitor", and
    // the window routes those to the master instead.
    void makeMonitorColumnMaster(const QString &tip);
    void setMonitorColumnTip(const QString &faderTip, const QString &muteTip);
    // Secondary master strips publish a virtual source only -- hide the mix
    // faders while keeping the card readable.
    void setMixFadersVisible(bool on, const QString &disabledTip = QString());
    // Grey out individual mix columns while keeping them visible.
    void setMixColumnEnabled(const QString &mix, bool on,
                             const QString &disabledTip = QString());

    // Optional vendor hardware controls on the microphone card.
    void enableHardwareMute(bool on = true);
    void setGainDb(double db);   // PreampDb style only
    void setHardwareMuted(bool muted);
    // Greys the preamp gain fader and the hardware mute while the microphone
    // is unplugged: both reach the device over USB and do nothing without it.
    void setHardwareControlsEnabled(bool on);

    // "Keep both faders equal". Window state rather than mixer state -- the
    // daemon has no idea the two faders are being moved together -- so the
    // window is what remembers it, per card, between sessions.
    bool fadersLinked() const;
    void setFadersLinked(bool on);

signals:
    void fadersLinkedChanged(bool on);
    // The identity tile was clicked: open this card's name/colour/icon panel.
    void identityClicked();
    void masterDeleteRequested();
    void masterRebuildRequested();
    void volumeChanged(const QString &id, const QString &mix, double volume);
    void muteToggled(const QString &id, const QString &mix, bool muted);
    void gainChanged(double db);
    void micSendChanged(const QString &id, double level);
    void micMuteToggled(const QString &id, bool muted);
    void hardwareMuteToggled(bool muted);
    void displayNameEdited(const QString &name);

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct MixColumn {
        // The column as a whole, so one that does not apply to this microphone
        // can be taken out of the layout entirely. Hiding only the fader and
        // its readout left the caption behind, labelling nothing.
        QWidget *host = nullptr;
        Fader *fader = nullptr;
        IconToggle *mute = nullptr;
        QLabel *value = nullptr;
        // The value this fader was last seen at, so a linked move can be
        // applied as a delta rather than by copying one fader onto the other.
        int last = 100;
    };
    QWidget *buildColumn(const QString &mix, const QString &iconOn,
                         const QString &iconOff, const QString &tip,
                         MixColumn &out);
    void onFaderMoved(const QString &mix, int value);
    void beginTitleEdit();
    void finishTitleEdit();
    void positionDeleteButton();
    void positionRebuildButton();
    void positionAddMasterButton();
    void positionOverlays();
    void scheduleOverlayLayout();
    void applyAccentToToggles();
    void applyTitleEditAccent();

    QString id_;
    QLabel *titleLabel_ = nullptr;
    QLineEdit *titleEdit_ = nullptr;
    bool titleEditable_ = false;
    QWidget *badgeTile_ = nullptr;
    // Held so the identity can be repainted in grey and back without the
    // caller having to hand it over again.
    QString iconName_;
    QColor color_;
    bool deviceConnected_ = true;
    MixColumn monitor_;
    MixColumn stream_;
    MixColumn gain_;
    MixColumn micSend_;
    GainStyle gainStyle_ = GainStyle::PreampDb;
    EqualToggle *link_ = nullptr;
    // Held rather than looked up by index: adding a widget above the faders
    // used to silently reroute them into whatever layout had shifted into slot
    // 3, which put the mic send fader above the row instead of beside it.
    QHBoxLayout *cols_ = nullptr;
    QHBoxLayout *head_ = nullptr;
    LevelMeter *meter_ = nullptr;
    LevelMeter *micMeter_ = nullptr;
    bool mainMeterActive_ = true;
    bool micMeterActive_ = false;
    bool micMeterTipFixed_ = false;
    QHBoxLayout *footer_ = nullptr;
    QWidget *footerHost_ = nullptr;
    QWidget *footerLeftSpacer_ = nullptr;
    QWidget *footerRightSpacer_ = nullptr;
    QWidget *footerLeftBox_ = nullptr;
    QWidget *footerRightBox_ = nullptr;
    QLabel *slotLabel_ = nullptr;
    int masterSlot_ = 0;
    qint64 inputLatencyUs_ = -1;
    void updateSlotTooltip();
    IconToggle *deleteButton_ = nullptr;
    IconToggle *rebuildButton_ = nullptr;
    QPushButton *addMasterOverlay_ = nullptr;
    bool masterDeleteEnabled_ = false;
    bool masterRebuildEnabled_ = false;
    bool overlayLayoutScheduled_ = false;
    IconToggle *fxButton_ = nullptr;
    IconToggle *settingsButton_ = nullptr;
    IconToggle *micMonitorButton_ = nullptr;
    IconToggle *hwMute_ = nullptr;
    // Guards the second half of a linked move, which would otherwise come
    // straight back in and move the first fader again.
    bool linking_ = false;
};
