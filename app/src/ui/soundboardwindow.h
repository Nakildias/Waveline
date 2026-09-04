// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Soundboard: a frameless window with its own title bar, styled after
// the Virtual Rack's (close/minimize, a bold heading, chrome buttons on the
// right) so the two "rack style" windows in this mixer read as the same
// family rather than two different ideas of what a satellite window looks
// like. Below the title bar, a rack of sound pads, drag-reorderable the same
// way the Virtual Rack's effect modules are.
//
// The gear button opens Soundboard Settings -- its Playback tab covers which
// channel everything plays on, whether it joins a microphone, and the two
// live volumes, its Backups tab covers export/import -- in its own panel
// (SoundboardSettingsWindow), because those are board-wide settings, not per
// sound: a sound board is meant to be mashed without first checking where
// each individual button happens to be routed, so they get one place to
// live rather than crowding the rack itself.
//
// Each pad is a circular play/stop button, a name, a glance-sized waveform,
// and edit (opens SoundTrimDialog for the name, waveform trim and clip
// volume) / remove. Everything is daemon state (MixerService's soundboard
// D-Bus surface): both windows are thin clients like every other one in the
// mixer, and the same sound can be triggered from here, from a Stream Deck
// via `wavelined-cli --soundboard-play <id>`, or from another instance of
// this window.

#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QList>
#include <QVector>
#include <QWidget>

#include "mixerclient.h"

class QComboBox;
class QGridLayout;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class ToggleSwitch;
class TrackSlider;

// ------------------------------------------------------------- ProgressClock
// The "position is a pure function of elapsed wall-clock time, not a value
// accumulated frame to frame" fix, factored out of MiniWaveform so
// SimplePad's identical progress ring can share it instead of duplicating
// it: see MiniWaveform's own class comment for why a running total drifts
// and this doesn't.
class ProgressClock {
public:
    // The trimmed range's length -- the real-time speed the playhead moves
    // at.
    void setDurationMs(double ms);
    // Starting a fresh run of playback discards any previous baseline: the
    // very next setProgress() call establishes a new one rather than being
    // treated as a correction to the old (now meaningless) one.
    void reset();
    // 0..1 through the trimmed range, from the daemon's own poll. Used to
    // establish where this run started (first call after reset()) and,
    // afterwards, only to silently correct real desync -- small deviations
    // are deliberately ignored since the local clock is smoother and more
    // precise than anything arriving over a ~400ms poll.
    void setProgress(double p);
    // Where the playhead is *right now*.
    double current() const;

private:
    double durationMs_ = 1000.0;
    double startProgress_ = 0.0;
    QElapsedTimer clock_;
};

// ------------------------------------------------------------------ PlayPad
// A circular play/stop button, painted rather than styled: a soundboard
// button should read as a physical pad, not as a rectangle borrowed from a
// dialog box. Filled and glowing in neutral white while the sound it belongs
// to is playing -- deliberately not the channel's colour: "this pad is
// making noise right now" is a status every channel shows the same way, not
// an identity marker.
class PlayPad : public QAbstractButton {
    Q_OBJECT

public:
    explicit PlayPad(QWidget *parent = nullptr);

    void setPlaying(bool on);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    bool playing_ = false;
    bool hover_ = false;
};

// -------------------------------------------------------------- MiniWaveform
// A glance-sized waveform from the daemon's stored peaks (see
// SoundboardSoundInfo::peaks). Decorative when idle; while its sound is
// playing it sweeps a playhead across the bars, so it doubles as a progress
// bar without needing a separate one bolted on beside it. Deliberately
// neutral (white/grey) rather than tinted to the board's channel colour:
// progress is a status any channel can show the same way, not an identity
// marker.
//
// The playhead position is never accumulated frame to frame -- it is
// computed fresh every paint as elapsed-real-time-since-start / duration
// (see ProgressClock::current()). That is the whole fix for the stutter earlier
// versions of this had: any scheme that adds a per-tick delta to a running
// total is only as accurate as the ticks themselves are regular, and ticks
// are not regular (this GUI's own ~400ms poll blocks the event loop for a
// few ms doing synchronous D-Bus calls) -- so the running total drifts and
// then has to visibly catch up. A pure function of elapsed time can't drift:
// whatever instant a frame happens to render at, the position it computes
// for that instant is exactly correct, whether the previous frame was 16ms
// or 60ms ago.
class MiniWaveform : public QWidget {
    Q_OBJECT

public:
    explicit MiniWaveform(QWidget *parent = nullptr);

    void setPeaks(const QVector<float> &peaks);
    void setActive(bool on);
    // The trimmed range's length -- the real-time speed the playhead moves
    // at, in other words, matching MixerService::PlaySoundboardSound's own
    // trimStartFrames/trimEndFrames.
    void setDurationMs(double ms);
    // 0..1 through the trimmed range, from MixerService::SoundboardProgress().
    // Used to establish where this instance started (first call after
    // becoming active) and, afterwards, only to silently correct real
    // desync -- see setProgress()'s own comment for why small deviations
    // are deliberately ignored rather than acted on every time.
    void setProgress(double p);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private slots:
    // Nothing but a repaint trigger: the position itself is computed in
    // paintEvent() via progress_.current(), not advanced here.
    void animate();

private:
    QVector<float> peaks_;
    bool active_ = false;
    ProgressClock progress_;
    QTimer *anim_ = nullptr;        // ~60fps repaint trigger only
};

// ------------------------------------------------------------------ SimplePad
// The Soundboard's "simple view" pad: a 1:1 match for the web companion's
// .sb-pad (see app/src/daemon/web/app.js's sbBuildPad() and app.css's
// .sb-pad/.pad-ring rules) -- just the sound's name on a rounded rectangle,
// with a progress ring traced around its own border while playing rather
// than a separate waveform or readout, so the two surfaces read as the same
// control drawn twice, not two different ideas of a soundboard pad.
class SimplePad : public QAbstractButton {
    Q_OBJECT

public:
    explicit SimplePad(QWidget *parent = nullptr);

    void setSoundName(const QString &name);
    void setPlaying(bool on);
    // The trimmed range's length, same meaning as MiniWaveform::setDurationMs.
    void setDurationMs(double ms);
    // 0..1 through the trimmed range, same meaning as MiniWaveform::setProgress.
    void setProgress(double p);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private slots:
    // Nothing but a repaint trigger, same division of labour as
    // MiniWaveform::animate(): the ring's actual position is computed fresh
    // in paintEvent() via progress_.current().
    void animate();

private:
    QString name_;
    bool playing_ = false;
    bool hover_ = false;
    ProgressClock progress_;
    QTimer *anim_ = nullptr;
};

// ----------------------------------------------------------- OverlayScrollBar
// A thin, draggable scrollbar that floats over a QScrollArea's viewport
// instead of sitting in its own reserved column -- so the rack's rows stay
// exactly kSoundboardWidth wide whether or not there happen to be enough
// sounds to scroll, while a scrollbar is still visible and usable once
// there are. `area`'s own scrollbar is left running with
// Qt::ScrollBarAlwaysOff: that only stops Qt from *drawing* it and
// reserving space for it, not from tracking range/value as content changes,
// so this widget can stay a thin read/write proxy for it rather than
// reimplementing scroll bookkeeping of its own.
class OverlayScrollBar : public QWidget {
    Q_OBJECT

public:
    // `area`'s vertical scrollbar is what this mirrors; `parent` should be
    // area->viewport(), so the bar floats over the content it scrolls.
    OverlayScrollBar(QScrollArea *area, QWidget *parent);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private slots:
    // The underlying scrollbar's range or value changed -- shows/hides this
    // widget (nothing to scroll = nothing to show) and repaints the thumb.
    void sync();

private:
    QRectF thumbRect() const;

    QScrollArea *area_;
    bool hover_ = false;
    bool dragging_ = false;
    int dragStartY_ = 0;
    int dragStartValue_ = 0;
};

// ---------------------------------------------------------------- IdBadge
// The 4-digit id pill. Clicking it copies the Stream Deck / keybind command
// for this sound to the clipboard -- the id only exists to be typed into
// something else, so the badge that shows it is the natural place to grab it
// from rather than making someone select and retype four digits by hand.
class IdBadge : public QLabel {
    Q_OBJECT

public:
    explicit IdBadge(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *e) override;
};

// One sound: grip, play pad, name + id + duration, mini waveform, edit and
// remove.
class SoundboardSoundRow : public QWidget {
    Q_OBJECT

public:
    explicit SoundboardSoundRow(const QString &id, QWidget *parent = nullptr);

    QString id() const { return id_; }
    QWidget *grip() const { return grip_; }

    // `progress` (0..1) is ignored while !playing.
    void applyInfo(const SoundboardSoundInfo &info, bool playing, double progress);

signals:
    void playRequested(const QString &id);
    void editRequested(const QString &id);
    void removeRequested(const QString &id);

private:
    QString id_;
    class CardBase *card_ = nullptr;
    QWidget *grip_ = nullptr;
    PlayPad *playPad_ = nullptr;
    IdBadge *idBadge_ = nullptr;
    QLabel *nameLabel_ = nullptr;
    QLabel *durationLabel_ = nullptr;
    MiniWaveform *waveform_ = nullptr;
    bool playing_ = false;
};

// ------------------------------------------------------- SoundboardSettingsWindow
// Soundboard Settings, on its own: two tabs, Playback (where every sound
// plays, whether it joins a microphone, and the two live volumes) and
// Backups (export/import the whole board as one file). A small satellite
// window rather than a dialog, opened from the Soundboard's title bar gear
// and left open beside it -- the same relationship the Virtual Rack has to
// Global Effects.
class SoundboardSettingsWindow : public QWidget {
    Q_OBJECT

public:
    explicit SoundboardSettingsWindow(MixerClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;

private slots:
    void refresh();

private:
    QWidget *buildPlaybackTab();
    QWidget *buildBackupsTab();
    // Rebuilds the channel/audio-sharing combo *contents*. Only called when
    // the set of channels or sharing targets actually changed -- clearing a
    // combo the user has open closes it out from under them.
    void populateOptions(const QList<ChannelInfo> &channels, const QStringList &shareTargets);
    // Current selection and slider values/state. Cheap and safe on every
    // refresh: never touches a combo's item list, only which item is current.
    void applySettings(const SoundboardSettingsInfo &settings);
    void exportBackup();
    void importBackup();

    MixerClient *client_ = nullptr;
    QComboBox *channelBox_ = nullptr;
    QComboBox *shareBox_ = nullptr;
    TrackSlider *shareSlider_ = nullptr;
    QLabel *sharePct_ = nullptr;
    TrackSlider *localSlider_ = nullptr;
    QLabel *localPct_ = nullptr;
    QString signature_;

    // ------------------------------------------------------------ Backups tab
    QPushButton *exportBtn_ = nullptr;
    QPushButton *importBtn_ = nullptr;
    QString lastBackupDir_;
};

class SoundboardWindow : public QWidget {
    Q_OBJECT

public:
    explicit SoundboardWindow(MixerClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void refresh();
    void onAddSound();
    void showSettings();
    // The title bar toggle: swaps which of rowsContainer_/simpleContainer_
    // is scroll_'s widget and remembers the choice.
    void setSimpleView(bool on);

private:
    QWidget *buildTitleBar();
    QWidget *buildEmptyState();

    SoundboardSoundRow *addRowWidget(const QString &id);
    SimplePad *addSimplePadWidget(const QString &id);
    void onPlayRequested(const QString &id);
    void onEditRequested(const QString &id);
    void onRemoveRequested(const QString &id);
    void syncOrderFromRows();

    MixerClient *client_ = nullptr;

    // ---------------------------------------------------------- title bar
    QLabel *countLabel_ = nullptr;
    ToggleSwitch *simpleViewToggle_ = nullptr;
    QPushButton *settingsBtn_ = nullptr;
    QPushButton *addBtn_ = nullptr;
    SoundboardSettingsWindow *settingsWindow_ = nullptr;

    // -------------------------------------------------------------- the rack
    QVBoxLayout *rowsLay_ = nullptr;
    QWidget *rowsContainer_ = nullptr;
    QScrollArea *scroll_ = nullptr;
    OverlayScrollBar *overlayScrollBar_ = nullptr;
    QWidget *emptyState_ = nullptr;
    QPushButton *emptyAddBtn_ = nullptr;
    QHash<QString, SoundboardSoundRow *> rows_;

    // ----------------------------------------------------- the simple view
    // A grid of SimplePad, 1:1 with the web companion's soundboard page --
    // see SimplePad's own comment. rowsContainer_'s alternative as scroll_'s
    // widget (see setSimpleView()). simpleContainer_ holds simpleOuterLay_
    // (a QVBoxLayout: the grid sub-widget, then a
    // trailing stretch -- see the constructor's comment on why); simpleLay_
    // is the QGridLayout inside that sub-widget, the one refresh() actually
    // populates.
    QVBoxLayout *simpleOuterLay_ = nullptr;
    QGridLayout *simpleLay_ = nullptr;
    QWidget *simpleContainer_ = nullptr;
    QHash<QString, SimplePad *> simplePads_;
    bool simpleView_ = false;

    // Every sound as of the last refresh, so edit/play/remove handlers do not
    // have to round-trip the daemon to know a sound's current settings.
    QList<SoundboardSoundInfo> cache_;
    SoundboardSoundRow *dragRow_ = nullptr;
    QString signature_;
    QString lastDir_;
};
