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
#include <QPoint>
#include <QString>

#include "widgets.h"

class Fader;
class IconToggle;
class QHBoxLayout;
class LevelMeter;
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
    // The Creative FX rack glyph beside the slot number (overlay, no layout
    // shift); clicking it opens this input's Virtual Rack.
    void enableMasterRack(bool on = true);
    // A channel's Creative FX rack: the same glyph, in the footer opposite the
    // gear. Emits rackRequested() as the input's does.
    void enableChannelRack(bool on = true);
    // All masters: hammer opposite the rack glyph, rebuilds the ALSA→DSP hop.
    void enableMasterRebuild(bool on = true);
    // Reparents btn onto this card and pins it to the right edge; nullptr clears.
    void setAddMasterOverlayButton(QPushButton *btn);
    // The same for the "-" that removes an input, pinned just under the "+".
    void setRemoveMasterOverlayButton(QPushButton *btn);
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
    // 0..1, before the faders: what reaches this card. Each mix fader's track
    // shows it through that fader and mute.
    void setLevel(double fraction);
    // A channel's published microphone, after its noise suppression and EQ,
    // shown through its mic fader; an input device's published signal, shown
    // behind its gain fader. Greyed and zero while inactive.
    void setMicLevel(double fraction);
    void setMicMeterActive(bool on);
    // Input device strips only: whether the Monitor send is live -- the device
    // is being monitored and not muted there. Its track greys out and reads
    // zero while it is not. Channel strips leave it live.
    void setMainMeterActive(bool on);
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
    // A channel publishing no microphone has nothing for its mic fader to do,
    // so the column goes and the card slims to a plain channel's width; making
    // it a microphone brings both back. Animated, except the first time, when
    // the card simply starts at the right width.
    void setMicColumnShown(bool on);
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
    // The daemon has cut monitoring because a Monitor mix plays on speakers:
    // the ear goes red, whatever it is set to, so it says why nothing is heard.
    void setMicMonitorBlocked(bool blocked);

    // Extra widget in the title row, right-aligned. The microphone card puts
    // the monitor-mode button here.
    void addHeaderWidget(QWidget *w);
    // Overrides both meter captions. The microphone card's bars mean something
    // different from a channel's, so the defaults would misdescribe them.
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

    // Lets the card be moved along the row by any part of it that is not a
    // control: its background, its name, the space around the faders. A press
    // that never travels past the system drag threshold is still whatever it
    // was (a click on the name renames). The window does the moving -- see
    // reorderStarted() and CardShuffle.
    void enableDragHandle(bool on = true);

    // The width this card needs: its floor in the row, and its share of any
    // room to spare. Changes with the mic and gain columns.
    int baseWidth() const { return minimumWidth(); }

signals:
    void baseWidthChanged();
    // The identity tile was clicked: open this card's name/colour/icon panel.
    void identityClicked();
    void rackRequested();
    void masterRebuildRequested();
    void volumeChanged(const QString &id, const QString &mix, double volume);
    void muteToggled(const QString &id, const QString &mix, bool muted);
    void gainChanged(double db);
    void micSendChanged(const QString &id, double level);
    void micMuteToggled(const QString &id, bool muted);
    void hardwareMuteToggled(bool muted);
    void displayNameEdited(const QString &name);
    // The card was pressed and has travelled past the drag threshold:
    // `globalPress` is where the press was, and `wholeGroup` whether Control
    // was held, which moves every card of its kind at once. Moves follow
    // until the release.
    void reorderStarted(const QPoint &globalPress, bool wholeGroup);
    void reorderMoved(const QPoint &globalPos);
    void reorderEnded();

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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
        // The level through this fader, standing in for its track.
        LevelMeter *meter = nullptr;
        // The value this fader was last seen at.
        int last = 100;
    };
    QWidget *buildColumn(const QString &mix, const QString &iconOn,
                         const QString &iconOff, const QString &tip,
                         MixColumn &out);
    void onFaderMoved(const QString &mix, int value);
    void beginTitleEdit();
    void finishTitleEdit();
    void positionRackButton();
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
    bool micMonitorBlocked_ = false;
    QString micMonitorTip_;
    MixColumn monitor_;
    MixColumn stream_;
    MixColumn gain_;
    MixColumn micSend_;
    // -1 until setMicColumnShown() has said either way.
    int micColumnShown_ = -1;
    class QVariantAnimation *widthAnim_ = nullptr;
    void animateWidthTo(int width);
    void setBaseWidth(int width);
    GainStyle gainStyle_ = GainStyle::PreampDb;
    // The grip in the gap between the Monitor and Stream faders, level with
    // their handles: dragging it moves both. See GangGrip in the .cpp.
    QWidget *gangGrip_ = nullptr;
    // The row the name sits in, between the header and the faders.
    QHBoxLayout *nameRow_ = nullptr;
    IconToggle *channelRackButton_ = nullptr;
    // Held rather than looked up by index: adding a widget above the faders
    // used to silently reroute them into whatever layout had shifted into slot
    // 3, which put the mic send fader above the row instead of beside it.
    QHBoxLayout *cols_ = nullptr;
    QHBoxLayout *head_ = nullptr;
    void applyMeterTints();
    double level_ = 0.0;
    bool mainMeterActive_ = true;
    bool micMeterActive_ = false;
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
    IconToggle *rackButton_ = nullptr;
    IconToggle *rebuildButton_ = nullptr;
    QPushButton *addMasterOverlay_ = nullptr;
    QPushButton *removeMasterOverlay_ = nullptr;
    bool masterRackEnabled_ = false;
    bool masterRebuildEnabled_ = false;
    bool overlayLayoutScheduled_ = false;
    IconToggle *fxButton_ = nullptr;
    IconToggle *settingsButton_ = nullptr;
    IconToggle *micMonitorButton_ = nullptr;
    IconToggle *hwMute_ = nullptr;
    // Drag state for moving the card; see enableDragHandle().
    bool dragHandle_ = false;
    bool dragArmed_ = false;
    bool dragging_ = false;
    QPoint dragPress_;
};
