// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "mainwindow.h"

#include <QBrush>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

#include "aboutwindow.h"
#include "settingswindow.h"
#include "cardidentity.h"
#include "channelstrip.h"
#include "creativerackwindow.h"
#include "effectswindow.h"
#include "levelmeter.h"
#include "mixerclient.h"
#include "profileswindow.h"
#include "soundboardwindow.h"
#include "soundsharingtab.h"
#include "theme.h"
#include "companionwindow.h"
#include "tunerwindow.h"
#include "version.h"
#include "widgets.h"

namespace {

constexpr int kSidebarWidth = 316;

// Output device names run long -- an HDMI sink carries the monitor model and
// the port, and a USB microphone's own ALSA description is not short either.
// Capped by character count so both rows cut off at the same place; elide at
// the end so the start of the name stays readable.
constexpr int kOutputNameChars = 25;

QString elideEnd(const QString &s, int maxChars = kOutputNameChars) {
    if (s.size() <= maxChars) return s;
    return s.left(maxChars - 1) + QChar(0x2026);
}

// A label in the dim secondary colour, for hints and units.


QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    return l;
}

QPushButton *iconButton(const QString &icon, const QString &tip, QWidget *parent) {
    auto *b = new QPushButton(parent);
    b->setIcon(Theme::icon(icon, Theme::TextDim, 16));
    b->setFixedSize(28, 24);
    b->setFlat(true);
    b->setToolTip(tip);
    return b;
}

QPushButton *squareOverlayButton(const QString &icon, const QString &tip,
                                 QWidget *parent) {
    auto *b = new QPushButton(parent);
    b->setIcon(Theme::icon(icon, Theme::TextDim, 12));
    b->setFixedSize(18, 18);
    b->setFlat(true);
    b->setToolTip(tip);
    return b;
}

// Sideways label: the whole caption rotated, not letter-by-letter.
class SidewaysCaption : public QWidget {
public:
    enum class Face { TowardTop, TowardBottom };

    SidewaysCaption(const QString &text, Face face, QWidget *parent = nullptr)
        : QWidget(parent), text_(text), face_(face) {
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.72);
        f.setBold(true);
        setFont(f);
        QPalette p = palette();
        p.setColor(QPalette::WindowText, Theme::TextDim);
        setPalette(p);
    }

    QSize sizeHint() const override { return captionSize(); }
    QSize minimumSizeHint() const override { return captionSize(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setPen(palette().color(QPalette::WindowText));
        p.setFont(font());
        if (face_ == Face::TowardTop) {
            p.translate(width(), 0);
            p.rotate(90);
        } else {
            p.translate(0, height());
            p.rotate(-90);
        }
        p.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter, text_);
    }

private:
    QSize captionSize() const {
        QFontMetrics fm(font());
        return {fm.height(), fm.horizontalAdvance(text_)};
    }

    QString text_;
    Face face_;
};

class InputsSectionSeparator : public QWidget {
public:
    explicit InputsSectionSeparator(QWidget *parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

        auto *col = new QVBoxLayout(this);
        col->setContentsMargins(0, 10, 0, 10);
        col->setSpacing(6);

        col->addWidget(
            new SidewaysCaption(tr("Input Devices"), SidewaysCaption::Face::TowardTop,
                                this),
            0, Qt::AlignHCenter);

        auto *toInputDevices = new QLabel(this);
        toInputDevices->setPixmap(
            Theme::iconPixmap(QStringLiteral("arrow-left"), Qt::white, 22));
        toInputDevices->setAlignment(Qt::AlignCenter);
        col->addWidget(toInputDevices, 0, Qt::AlignHCenter);

        col->addStretch(1);

        auto *toChannels = new QLabel(this);
        toChannels->setPixmap(
            Theme::iconPixmap(QStringLiteral("arrow-right"), Qt::white, 22));
        toChannels->setAlignment(Qt::AlignCenter);
        col->addWidget(toChannels, 0, Qt::AlignHCenter);

        col->addWidget(
            new SidewaysCaption(tr("Channels"), SidewaysCaption::Face::TowardBottom,
                                this),
            0, Qt::AlignHCenter);
    }
};

// The heartbeat is a checkable button, so a left click has already flipped its
// own appearance by the time the handler runs: green -> unchecked, which reads
// as "effects off" for the whole daemon round trip even when the click is on
// its way to violet. Stamping the state the cycle is moving to keeps the button
// on the three states it actually has; the next Changed confirms it. Right
// clicks never had the flicker, because they do not toggle.
// The card table, as the flat "key\tcolour\ticon" lines refreshCardLooks()
// already builds. Kept in the mixer's own settings so the window can paint the
// right colours before the daemon has answered: the first Channels/Cards round
// trip lands after the first frame, and every launch used to show a moment of
// theme defaults before the user's colours arrived.
constexpr const char *kCardLooksKey = "cards/table";

QHash<QString, Theme::CardLook> looksFromTable(const QString &table) {
    QHash<QString, Theme::CardLook> out;
    for (const QString &row : table.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;
        Theme::CardLook look;
        look.color = QColor(f[1]);
        look.icon = f[2];
        out.insert(f[0], look);
    }
    return out;
}

// "Keep both faders equal" is window state: the daemon never sees it, so it
// lives in the mixer's own settings file, keyed by the same card key the
// colours use.
bool loadFadersLinked(const QString &cardKey) {
    QSettings s;
    s.beginGroup(QStringLiteral("link"));
    const bool on = s.value(cardKey, false).toBool();
    s.endGroup();
    return on;
}

QStringList loadChannelOrderSetting() {
    QSettings s;
    return s.value(QStringLiteral("channelOrder")).toStringList();
}

void saveChannelOrderSetting(const QStringList &order) {
    QSettings s;
    if (order.isEmpty())
        s.remove(QStringLiteral("channelOrder"));
    else
        s.setValue(QStringLiteral("channelOrder"), order);
}

void saveFadersLinked(const QString &cardKey, bool on) {
    QSettings s;
    s.beginGroup(QStringLiteral("link"));
    if (on)
        s.setValue(cardKey, true);
    else
        s.remove(cardKey);   // the default; no need to record it
    s.endGroup();
}

// Heartbeat colours. Violet used to mean "headphones hear the chain" on every
// card at once; the card's own colour says the same thing and says *which*
// card while it is at it. White is "effects on, headphones dry" -- a state
// with no colour of its own to claim. Off keeps the unchecked look, which
// takes no accent at all.
QColor effectsAccent(bool monitorFx, const QColor &card) {
    return monitorFx && card.isValid() ? card : QColor(Theme::Text);
}

void showEffectsState(IconToggle *fx, bool on, bool monitorFx,
                      const QColor &card) {
    if (!fx) return;
    QSignalBlocker block(fx);
    fx->setChecked(on);
    fx->setAccent(effectsAccent(monitorFx && on, card));
}

void cycleEffectsForward(MixerClient *client, const QString &masterId,
                         IconToggle *fx, const QColor &card) {
    const bool on = client->masterMicEffectsEnabled(masterId);
    const bool mon = client->masterMicMonitorFx(masterId);
    if (!on) {
        showEffectsState(fx, true, false, card);
        client->setMasterMicEffectsEnabled(masterId, true);
        client->setMasterMicMonitorFx(masterId, false);
    } else if (!mon) {
        showEffectsState(fx, true, true, card);
        client->setMasterMicMonitorFx(masterId, true);
    } else {
        showEffectsState(fx, false, false, card);
        client->setMasterMicMonitorFx(masterId, false);
        client->setMasterMicEffectsEnabled(masterId, false);
    }
    client->refresh();
}

void cycleEffectsForwardChannel(MixerClient *client, const QString &channelId,
                                IconToggle *fx, const QColor &card) {
    const bool on = client->channelEffectsEnabled(channelId);
    const bool mon = client->channelMonitorFx(channelId);
    if (!on) {
        showEffectsState(fx, true, false, card);
        client->setChannelEffectsEnabled(channelId, true);
        client->setChannelMonitorFx(channelId, false);
    } else if (!mon) {
        showEffectsState(fx, true, true, card);
        client->setChannelMonitorFx(channelId, true);
    } else {
        showEffectsState(fx, false, false, card);
        client->setChannelMonitorFx(channelId, false);
        client->setChannelEffectsEnabled(channelId, false);
    }
    client->refresh();
}

void cycleEffectsBackward(MixerClient *client, const QString &masterId,
                          IconToggle *fx, const QColor &card) {
    const bool on = client->masterMicEffectsEnabled(masterId);
    const bool mon = client->masterMicMonitorFx(masterId);
    if (on && mon) {
        showEffectsState(fx, true, false, card);
        client->setMasterMicMonitorFx(masterId, false);
    } else if (on && !mon) {
        showEffectsState(fx, false, false, card);
        client->setMasterMicEffectsEnabled(masterId, false);
    } else {
        showEffectsState(fx, true, true, card);
        client->setMasterMicEffectsEnabled(masterId, true);
        client->setMasterMicMonitorFx(masterId, true);
    }
    client->refresh();
}

void cycleEffectsBackwardChannel(MixerClient *client, const QString &channelId,
                                 IconToggle *fx, const QColor &card) {
    const bool on = client->channelEffectsEnabled(channelId);
    const bool mon = client->channelMonitorFx(channelId);
    if (on && mon) {
        showEffectsState(fx, true, false, card);
        client->setChannelMonitorFx(channelId, false);
    } else if (on && !mon) {
        showEffectsState(fx, false, false, card);
        client->setChannelEffectsEnabled(channelId, false);
    } else {
        showEffectsState(fx, true, true, card);
        client->setChannelEffectsEnabled(channelId, true);
        client->setChannelMonitorFx(channelId, true);
    }
    client->refresh();
}

// The strip rather than a colour: a card's accent can change under the button
// (the identity panel, or the device being unplugged), and the click has to
// use whatever it is at the time.
void connectMasterEffectsButton(IconToggle *fx, MixerClient *client,
                                const QString &masterId, ChannelStrip *strip) {
    QObject::connect(fx, &QAbstractButton::clicked, fx, [fx, client, masterId, strip] {
        cycleEffectsForward(client, masterId, fx, strip->accentColor());
    });
    QObject::connect(fx, &IconToggle::rightClicked, fx, [fx, client, masterId, strip] {
        cycleEffectsBackward(client, masterId, fx, strip->accentColor());
    });
}

void connectEffectsButton(IconToggle *fx, MixerClient *client,
                          const QString &channelId, ChannelStrip *strip) {
    QObject::connect(fx, &QAbstractButton::clicked, fx, [fx, client, channelId, strip] {
        cycleEffectsForwardChannel(client, channelId, fx, strip->accentColor());
    });
    QObject::connect(fx, &IconToggle::rightClicked, fx, [fx, client, channelId, strip] {
        cycleEffectsBackwardChannel(client, channelId, fx, strip->accentColor());
    });
}

IconToggle *outputMuteToggle(QWidget *parent) {
    auto *m = new IconToggle(QStringLiteral("speaker-muted"),
                             QStringLiteral("speaker-high"), parent);
    m->setAccent(Theme::Danger);
    m->setIconSize(16);
    m->setToolTip(QObject::tr("Mute this output."));
    return m;
}

IconToggle *streamMuteToggle(QWidget *parent) {
    auto *m = new IconToggle(QStringLiteral("stream-off"), QStringLiteral("stream"),
                             parent);
    m->setAccent(Theme::Danger);
    m->setIconSize(16);
    m->setToolTip(QObject::tr("Mute this output."));
    return m;
}

QSlider *outputVolumeSlider(QWidget *parent) {
    auto *s = new QSlider(Qt::Horizontal, parent);
    s->setRange(0, 100);
    s->setMinimumWidth(80);
    s->setFocusPolicy(Qt::StrongFocus);
    return s;
}

}  // namespace

// A label that keeps its stylesheet box -- so it can match the device picker
// beside it -- but elides like one. A plain QLabel clips at its right edge with
// no ellipsis, which at the width of the name column cuts the name mid-word;
// the picker on the row above ellipsizes itself, and the two should agree.
// Elides in the middle for the same reason the character cap does: the head
// names the device and the tail tells two similar ones apart.
class BoxedName : public QLabel {
public:
    // Replaces the text the elide is computed from. QLabel::setText() would be
    // undone by the next resize, which recomputes the elide from full_.
    void setFullText(const QString &text) {
        full_ = text;
        applyElide();
    }

    BoxedName(const QString &text, QWidget *parent)
        : QLabel(parent), full_(text) {
        // Ignored, not Preferred: a QLabel reports the full width of its text
        // as its minimum, which would hold the column open to the length of
        // the name and undo the narrowing the grid asks for.
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        applyElide();
    }

protected:
    void resizeEvent(QResizeEvent *e) override {
        QLabel::resizeEvent(e);
        applyElide();
    }

private:
    void applyElide() {
        // contentsRect() is inside the stylesheet's padding and border, so it
        // is the space the text actually has.
        QLabel::setText(fontMetrics().elidedText(full_, Qt::ElideRight,
                                                 contentsRect().width()));
    }

    QString full_;
};

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("Waveline"));
    setWindowIcon(Theme::icon(QStringLiteral("microphone"), Theme::Accent, 64));

    client_ = new MixerClient(this);
    changeDebounce_.setSingleShot(true);
    changeDebounce_.setInterval(100);
    connect(&changeDebounce_, &QTimer::timeout, this, &MainWindow::onChanged);
    // Card colours first, undebounced and before the tabs are built: the Apps
    // and Sound Sharing tables are connected to the same signal and draw their
    // badges and sliders from this table, and Qt delivers to slots in the order
    // they were connected. Debouncing this one would hand them yesterday's
    // colours on the poll that changed them.
    connect(client_, &MixerClient::changed, this, &MainWindow::refreshCardLooks);
    connect(client_, &MixerClient::changed, this, [this] { changeDebounce_.start(); });
    connect(client_, &MixerClient::levelsChanged, this, &MainWindow::onLevels);
    connect(client_, &MixerClient::availabilityChanged, this,
            &MainWindow::onAvailabilityChanged);

    // Before a single widget exists: the tabs below build their badges and
    // sliders in their constructors, and the cards are built from these
    // colours too. The cache paints the first frame; the daemon confirms it on
    // the same connection a moment later, or right now if it is already up.
    applyCachedCardLooks();
    refreshCardLooks();

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(buildHeader());

    bannerLabel_ = new QLabel(central);
    bannerLabel_->setWordWrap(true);
    bannerLabel_->setVisible(false);
    bannerLabel_->setContentsMargins(16, 10, 16, 10);
    bannerLabel_->setStyleSheet(
        QStringLiteral("QLabel { background: %1; color: %2; }")
            .arg(QColor(0x4a, 0x1d, 0x1d).name(), Theme::Text.name()));
    outer->addWidget(bannerLabel_);

    // Amber, not the red above it: the daemon being gone means nothing works,
    // this means one application is going to the wrong place. Sorting those
    // two into the same colour is how a warning stops being read.
    routingBanner_ = new QWidget(central);
    routingBanner_->setVisible(false);
    routingBanner_->setStyleSheet(
        QStringLiteral("QWidget { background: %1; }")
            .arg(QColor(0x4a, 0x3a, 0x14).name()));
    routingBannerRows_ = new QVBoxLayout(routingBanner_);
    routingBannerRows_->setContentsMargins(16, 8, 16, 8);
    routingBannerRows_->setSpacing(6);
    outer->addWidget(routingBanner_);

    auto *body = new QHBoxLayout;
    body->setContentsMargins(16, 14, 16, 16);
    body->setSpacing(14);

    auto *left = new QVBoxLayout;
    left->setSpacing(8);
    left->addWidget(caption(tr("Inputs"), central));
    left->addWidget(buildInputs(), 1);
    left->addSpacing(4);
    left->addWidget(caption(tr("Outputs"), central));
    left->addWidget(buildOutputs());
    body->addLayout(left, 1);
    body->addWidget(buildSidebar());

    outer->addLayout(body, 1);
    setCentralWidget(central);

    setUpdatesEnabled(false);
    onAvailabilityChanged(client_->available());
    onChanged();
    setUpdatesEnabled(true);
    resize(1431, 738);
    // Sized from what the Outputs row actually needs -- device picker, meter
    // and volume side by side -- rather than from a round number. Below this
    // the combo starts clipping its text instead of eliding it.
    setMinimumSize(940, 580);
}

void MainWindow::followScreen() {
    QWindow *w = windowHandle();
    if (!w) return;
    if (QScreen *s = w->screen()) {
        MeterTicker::setRefreshRate(s->refreshRate());
        // A screen can change mode under us (a refresh-rate switch, or VRR
        // settling), so track it rather than reading once at startup.
        connect(s, &QScreen::refreshRateChanged, this,
                [](qreal hz) { MeterTicker::setRefreshRate(hz); },
                Qt::UniqueConnection);
    }
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    if (QWindow *w = windowHandle()) {
        connect(w, &QWindow::screenChanged, this, [this](QScreen *) { followScreen(); },
                Qt::UniqueConnection);
    }
    followScreen();
    client_->setPollingEnabled(true);
}

void MainWindow::hideEvent(QHideEvent *e) {
    QMainWindow::hideEvent(e);
    // Minimised or on another workspace: nothing to draw, so stop polling the
    // daemon sixty times a second for numbers nobody can see.
    client_->setPollingEnabled(false);
}

void MainWindow::scrollSidebar(int pixels) {
    if (sidebarScroll_) sidebarScroll_->verticalScrollBar()->setValue(pixels);
}

// ================================================================== header

QWidget *MainWindow::buildHeader() {
    auto *bar = new CardBase(this);
    bar->setRadius(0);
    bar->setFillColor(Theme::Card);
    bar->setFixedHeight(56);

    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(16, 0, 12, 0);
    lay->setSpacing(10);

    auto *mark = new QLabel(QStringLiteral("WAVELINE"), bar);
    QFont mf = mark->font();
    mf.setBold(true);
    mf.setPointSizeF(mf.pointSizeF() * 1.15);
    mf.setLetterSpacing(QFont::AbsoluteSpacing, 1.4);
    mark->setFont(mf);
    lay->addWidget(mark);
    versionLabel_ = dimLabel(QStringLiteral("v%1").arg(QStringLiteral(WAVELINE_VERSION)), bar);
    lay->addWidget(versionLabel_);

    lay->addStretch();

    // One gear, not two dropdowns and an info button.
    //
    // Latency and headroom are machine setup -- set once, checked rarely --
    // and they were taking a third of a bar that also has to fit a status
    // line, a profile switcher, the tuner and the companion. Everything that
    // was here is now a tab behind this button, together with the warnings and
    // the service list, which is where someone goes when audio is misbehaving
    // anyway. See ui/settingswindow.h.
    diagnosticsBtn_ = iconButton(QStringLiteral("gear"),
                                 tr("Latency & Diagnostics: graph latency, "
                                    "output headroom, warnings, service status "
                                    "and measured latency per device."),
                                 bar);
    connect(diagnosticsBtn_, &QPushButton::clicked, this,
            &MainWindow::showLatencyDiagnostics);
    lay->addWidget(diagnosticsBtn_);
    lay->addSpacing(6);

    // One control for profiles, not two. The drop-down beside this button was
    // a second way to do what the panel behind it already does, and the two
    // disagreed in the way that matters: the combo switched on a single click,
    // with the confirmation as the only thing between a mis-click and a live
    // stream being re-routed. The panel makes switching a deliberate act, and
    // the name of the loaded profile is no longer worth 150px of a bar
    // this narrow -- it is on the first row of the panel.
    manageProfiles_ = new QPushButton(bar);
    manageProfiles_->setIcon(
        Theme::icon(QStringLiteral("switch-profile"), Theme::TextDim, 16));
    manageProfiles_->setFixedSize(38, 30);
    manageProfiles_->setToolTip(
        tr("Profiles: switch between saved mixer setups, and save, rename, "
           "delete, export and import them."));
    connect(manageProfiles_, &QPushButton::clicked, this,
            &MainWindow::showProfiles);
    lay->addWidget(manageProfiles_);

    tunerBtn_ = new QPushButton(tr("Tuner"), bar);
    tunerBtn_->setToolTip(
        tr("Tune an instrument from any input, without disturbing your mixer "
           "routing."));
    connect(tunerBtn_, &QPushButton::clicked, this, &MainWindow::showTuner);
    lay->addWidget(tunerBtn_);

    soundboardBtn_ = new QPushButton(tr("Soundboard"), bar);
    soundboardBtn_->setToolTip(
        tr("Play sound clips onto a channel, share them into a microphone, "
           "and trigger them from a Stream Deck with wavelined-cli."));
    connect(soundboardBtn_, &QPushButton::clicked, this, &MainWindow::showSoundboard);
    lay->addWidget(soundboardBtn_);

    companionBtn_ = new QPushButton(tr("Companion"), bar);
    companionBtn_->setToolTip(
        tr("Control this mixer from a phone or tablet on the same network."));
    connect(companionBtn_, &QPushButton::clicked, this,
            &MainWindow::showCompanion);
    lay->addWidget(companionBtn_);

    aboutBtn_ = iconButton(QStringLiteral("info"), tr("About Waveline"), bar);
    connect(aboutBtn_, &QPushButton::clicked, this, &MainWindow::showAbout);
    lay->addWidget(aboutBtn_);

    // No show/hide control for the sidebar: it is narrow enough to leave up
    // permanently, and a panel that can be hidden is a panel users lose.

    return bar;
}

// ================================================================== inputs

QWidget *MainWindow::buildInputs() {
    // The cards live in a recessed well, the way Wave Link groups its inputs.
    // It is what makes them read as one row of things rather than six
    // unrelated panels.
    auto *well = new CardBase(this);
    well->setFillColor(Theme::Well);
    well->setRadius(14);

    auto *wellLay = new QVBoxLayout(well);
    wellLay->setContentsMargins(10, 10, 10, 10);

    auto *host = new QWidget(well);
    stripHost_ = host;
    loadChannelOrder();
    // Cards are reordered by dragging their link button; the row they land in
    // is this widget. See handleStripDrag().
    host->setAcceptDrops(true);
    host->installEventFilter(this);
    stripRow_ = new QHBoxLayout(host);
    stripRow_->setContentsMargins(0, 0, 0, 0);
    stripRow_->setSpacing(8);

    // Shown while there are no cards, so a stopped daemon leaves an
    // explanation rather than a large empty rectangle.
    emptyLabel_ = dimLabel(tr("Waiting for wavelined..."), host);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    stripRow_->addWidget(emptyLabel_, 1);

    inputsSeparator_ = new InputsSectionSeparator(host);
    inputsSeparator_->hide();
    stripRow_->addWidget(inputsSeparator_);

    stripRow_->addStretch();

    // Horizontal scrolling rather than shrinking cards: a fader that is 40 px
    // tall is not a fader.
    auto *scroll = new QScrollArea(well);
    scroll->setWidget(host);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->viewport()->setAutoFillBackground(false);
    host->setAutoFillBackground(false);
    wellLay->addWidget(scroll);

    return well;
}

// ================================================================= outputs

QWidget *MainWindow::buildOutputs() {
    outputsCard_ = new CardBase(this);
    outputsCard_->setFillColor(Theme::Card);
    outputsCard_->setRadius(12);

    outputsGrid_ = new QGridLayout(outputsCard_);
    outputsGrid_->setContentsMargins(14, 12, 14, 12);
    outputsGrid_->setHorizontalSpacing(10);
    outputsGrid_->setVerticalSpacing(10);

    streamIcon_ = new QLabel(outputsCard_);
    streamIcon_->setPixmap(Theme::iconPixmap(QStringLiteral("stream"), Theme::TextDim, 18));

    streamNameLabel_ = new QLabel(tr("Stream mix"), outputsCard_);
    QFont bold = streamNameLabel_->font();
    bold.setBold(true);
    streamNameLabel_->setFont(bold);

    streamMixName_ = new BoxedName(tr("Waveline Stream Mix"), outputsCard_);
    streamMixName_->setObjectName(QStringLiteral("fixedOutput"));
    streamMixName_->setToolTip(
        tr("The Stream mix is a virtual device, not a speaker: it has no output "
           "to choose. Point your recorder or streaming software at it."));

    streamMute_ = streamMuteToggle(outputsCard_);
    streamMute_->setToolTip(tr("Mute the Stream mix virtual device."));
    connect(streamMute_, &QAbstractButton::toggled, this, [this](bool muted) {
        if (!client_->available()) return;
        client_->setStreamMixMuted(muted);
    });

    streamVolume_ = outputVolumeSlider(outputsCard_);
    streamVolume_->setToolTip(tr("Level of the Stream mix virtual device."));
    connect(streamVolume_, &QSlider::valueChanged, this, [this](int v) {
        if (!client_->available()) return;
        client_->setStreamMixVolume(v / 100.0);
    });

    streamMeter_ = new LevelMeter(Qt::Horizontal, outputsCard_);
    streamMeter_->setThickness(8);
    streamMeter_->setMinimumWidth(110);

    outputsGrid_->setColumnStretch(2, 2);
    outputsGrid_->setColumnStretch(5, 3);
    outputsGrid_->setColumnStretch(6, 4);

    syncMonitorOutputUi();
    return outputsCard_;
}

QString MainWindow::monitorOutputLabel(int index, int total) const {
    if (total <= 1) return tr("Monitor mix");
    return tr("Monitor mix #%1").arg(index + 1);
}

// Two Monitor mixes on one device would each load a loopback onto it: they
// fight over the clock driver and the device plays the sum of both faders. So
// a device another row already owns is simply not offered here -- the row's
// own device always is, otherwise it could not stay selected.
void MainWindow::fillMonitorOutputCombo(QComboBox *combo, int index,
                                        const QList<MonitorOutputInfo> &states,
                                        const QList<OutputInfo> &outs) {
    if (!combo) return;
    const MonitorOutputInfo state =
        index < states.size() ? states[index] : MonitorOutputInfo{};
    const QString cur = state.sink;
    QSet<QString> taken;
    for (int j = 0; j < states.size(); ++j) {
        if (j == index || states[j].sink.isEmpty()) continue;
        taken.insert(states[j].sink);
    }

    QSignalBlocker block(combo);
    combo->clear();
    bool found = false;
    for (const auto &o : outs) {
        if (taken.contains(o.name)) continue;
        combo->addItem(elideEnd(o.description), o.name);
        combo->setItemData(combo->count() - 1, o.description, Qt::ToolTipRole);
        if (o.name == cur) found = true;
    }
    // Assigned sink is offline — keep it in the list, greyed, waiting.
    if (!cur.isEmpty() && !found) {
        QString label = state.description.isEmpty() ? cur : state.description;
        label += tr(" (disconnected)");
        combo->addItem(elideEnd(label), cur);
        const int idx = combo->count() - 1;
        combo->setItemData(idx, label, Qt::ToolTipRole);
        combo->setItemData(idx, QBrush(Theme::TextFaint), Qt::ForegroundRole);
    }
    for (int j = 0; j < combo->count(); ++j) {
        if (combo->itemData(j).toString() == cur) {
            combo->setCurrentIndex(j);
            break;
        }
    }
}

// Nothing left to add once every output device drives a Monitor mix: greyed
// with a tooltip that says so, rather than an "+" that silently does nothing.
void MainWindow::applyAddMonitorOutputEnabled(MonitorOutputRowUi &row,
                                              const QList<MonitorOutputInfo> &states,
                                              const QList<OutputInfo> &outs) {
    if (!row.addBtn) return;
    const bool free = !freeMonitorOutputDevice(states, outs).isEmpty();
    row.addBtn->setEnabled(free);
    row.addBtn->setToolTip(
        free ? tr("Add another Monitor mix output (up to 5).")
             : tr("Every output device is already assigned to a Monitor mix."));
}

QString MainWindow::freeMonitorOutputDevice(const QList<MonitorOutputInfo> &states,
                                            const QList<OutputInfo> &outs) {
    QSet<QString> taken;
    for (const MonitorOutputInfo &s : states)
        if (!s.sink.isEmpty()) taken.insert(s.sink);
    for (const OutputInfo &o : outs)
        if (!taken.contains(o.name)) return o.name;
    return {};
}

void MainWindow::refreshMonitorOutputCombos() {
    if (!client_->available()) return;
    const QList<MonitorOutputInfo> states = client_->monitorOutputStates();
    const QList<OutputInfo> outs = client_->outputs();
    for (int i = 0; i < monitorOutputRows_.size(); ++i) {
        MonitorOutputRowUi &row = monitorOutputRows_[i];
        if (!row.combo) continue;
        const MonitorOutputInfo s =
            i < states.size() ? states[i] : MonitorOutputInfo{};
        fillMonitorOutputCombo(row.combo, i, states, outs);
        applyMonitorOutputRowConnected(row, s.connected || s.sink.isEmpty());
        applyAddMonitorOutputEnabled(row, states, outs);
    }
}

void MainWindow::applyMonitorOutputRowConnected(MonitorOutputRowUi &row,
                                                bool connected) {
    const QColor iconColor = connected ? Theme::TextDim : Theme::TextFaint;
    if (row.icon) {
        row.icon->setPixmap(
            Theme::iconPixmap(QStringLiteral("headphones"), iconColor, 18));
        row.icon->setEnabled(connected);
    }
    if (row.label) {
        QPalette pal = row.label->palette();
        pal.setColor(QPalette::WindowText,
                     connected ? Theme::Text : Theme::TextFaint);
        row.label->setPalette(pal);
    }
    // Combo stays usable so the user can pick a different device while waiting.
    if (row.mute) row.mute->setEnabled(connected);
    if (row.volume) row.volume->setEnabled(connected);
    if (row.meter) {
        row.meter->setEnabled(connected);
        if (!connected) row.meter->setLevel(0.0);
    }
}

void MainWindow::refreshMonitorOutputControls() {
    if (!client_->available()) return;
    const QList<MonitorOutputInfo> states = client_->monitorOutputStates();
    for (int i = 0; i < monitorOutputRows_.size(); ++i) {
        MonitorOutputRowUi &row = monitorOutputRows_[i];
        if (i >= states.size()) break;
        const MonitorOutputInfo &s = states[i];
        applyMonitorOutputRowConnected(row, s.connected || s.sink.isEmpty());
        if (row.volume) {
            // Do not push daemon values back while the user is adjusting: that
            // used to fight keyboard focus and felt like arrows only worked once.
            if (row.volume->hasFocus() || row.volume->isSliderDown()) continue;
            QSignalBlocker b(row.volume);
            row.volume->setValue(int(s.volume * 100.0 + 0.5));
        }
        if (row.mute) {
            QSignalBlocker b(row.mute);
            row.mute->setChecked(s.muted);
        }
    }
    if (streamVolume_) {
        if (!streamVolume_->hasFocus() && !streamVolume_->isSliderDown()) {
            QSignalBlocker b(streamVolume_);
            streamVolume_->setValue(int(client_->streamMixVolume() * 100.0 + 0.5));
        }
    }
    if (streamMute_) {
        QSignalBlocker b(streamMute_);
        streamMute_->setChecked(client_->streamMixMuted());
    }
}

void MainWindow::syncMonitorOutputUi() {
    if (!outputsGrid_ || !outputsCard_) return;

    const QList<MonitorOutputInfo> states =
        client_ && client_->available() ? client_->monitorOutputStates()
                                          : QList<MonitorOutputInfo>{};
    const auto outs = client_ && client_->available() ? client_->outputs() : QList<OutputInfo>{};
    const int count = qMax(1, states.size());
    QString sig = QStringLiteral("%1|%2").arg(count).arg(outs.size());
    for (const MonitorOutputInfo &s : states)
        sig += QLatin1Char('|') + s.sink +
               (s.connected ? QLatin1Char('1') : QLatin1Char('0'));

    if (sig == monitorOutputsSig_ && monitorOutputRows_.size() == count) {
        refreshMonitorOutputCombos();
        refreshMonitorOutputControls();
        for (int i = 0; i < monitorOutputRows_.size(); ++i) {
            MonitorOutputRowUi &row = monitorOutputRows_[i];
            if (row.label) row.label->setText(monitorOutputLabel(i, count));
            // Enabled state comes from refreshMonitorOutputCombos() above.
            if (row.addBtn) row.addBtn->setVisible(i == 0 && count < 5);
            if (row.removeBtn) row.removeBtn->setVisible(count > 1 && i > 0);
        }
        return;
    }
    monitorOutputsSig_ = sig;

    for (const MonitorOutputRowUi &row : monitorOutputRows_) {
        if (row.icon) row.icon->deleteLater();
        if (row.label) row.label->deleteLater();
        if (row.combo) row.combo->deleteLater();
        if (row.addBtn) row.addBtn->deleteLater();
        if (row.removeBtn) row.removeBtn->deleteLater();
        if (row.mute) row.mute->deleteLater();
        if (row.volume) row.volume->deleteLater();
        if (row.meter) row.meter->deleteLater();
    }
    monitorOutputRows_.clear();

    if (streamIcon_) outputsGrid_->removeWidget(streamIcon_);
    if (streamNameLabel_) outputsGrid_->removeWidget(streamNameLabel_);
    if (streamMixName_) outputsGrid_->removeWidget(streamMixName_);
    if (streamMute_) outputsGrid_->removeWidget(streamMute_);
    if (streamVolume_) outputsGrid_->removeWidget(streamVolume_);
    if (streamMeter_) outputsGrid_->removeWidget(streamMeter_);

    for (int i = 0; i < count; ++i) {
        MonitorOutputRowUi row;
        const MonitorOutputInfo s =
            i < states.size() ? states[i] : MonitorOutputInfo{};
        const bool connected = s.connected || s.sink.isEmpty();
        row.icon = new QLabel(outputsCard_);
        row.icon->setPixmap(Theme::iconPixmap(
            QStringLiteral("headphones"),
            connected ? Theme::TextDim : Theme::TextFaint, 18));
        outputsGrid_->addWidget(row.icon, i, 0);

        row.label = new QLabel(monitorOutputLabel(i, count), outputsCard_);
        QFont labelFont = row.label->font();
        labelFont.setBold(true);
        row.label->setFont(labelFont);
        outputsGrid_->addWidget(row.label, i, 1);

        row.combo = new QComboBox(outputsCard_);
        row.combo->setMinimumWidth(120);
        row.combo->setToolTip(
            tr("Where this Monitor mix copy is played. Each output receives the "
               "same mix; level and mute are per device. Assigned devices stay "
               "selected when unplugged (shown as disconnected) so monitor never "
               "falls back to another sink."));
        if (client_ && client_->available())
            fillMonitorOutputCombo(row.combo, i, states, outs);
        connect(row.combo, &QComboBox::activated, this,
                [this, i](int idx) { onOutputPicked(i, idx); });
        outputsGrid_->addWidget(row.combo, i, 2);

        if (i == 0 && count < 5) {
            row.addBtn = iconButton(QStringLiteral("plus"),
                                    tr("Add another Monitor mix output (up to 5)."),
                                    outputsCard_);
            connect(row.addBtn, &QPushButton::clicked, this, &MainWindow::onAddMonitorOutput);
            outputsGrid_->addWidget(row.addBtn, i, 3);
            applyAddMonitorOutputEnabled(row, states, outs);
        }
        if (i > 0) {
            row.removeBtn = iconButton(QStringLiteral("minus"),
                                       tr("Remove this Monitor mix output."),
                                       outputsCard_);
            connect(row.removeBtn, &QPushButton::clicked, this,
                    [this, i] { onRemoveMonitorOutput(i); });
            outputsGrid_->addWidget(row.removeBtn, i, 3);
        }

        row.mute = outputMuteToggle(outputsCard_);
        row.mute->setToolTip(tr("Mute this Monitor mix output only."));
        if (i < states.size()) row.mute->setChecked(states[i].muted);
        connect(row.mute, &QAbstractButton::toggled, this, [this, i](bool muted) {
            if (!client_->available()) return;
            client_->setMonitorOutputMutedAt(i, muted);
        });
        outputsGrid_->addWidget(row.mute, i, 4);

        row.volume = outputVolumeSlider(outputsCard_);
        row.volume->setToolTip(tr("Level for this Monitor mix output."));
        if (i < states.size())
            row.volume->setValue(int(states[i].volume * 100.0 + 0.5));
        connect(row.volume, &QSlider::valueChanged, this, [this, i](int v) {
            if (!client_->available()) return;
            client_->setMonitorOutputVolumeAt(i, v / 100.0);
        });
        outputsGrid_->addWidget(row.volume, i, 5);

        row.meter = new LevelMeter(Qt::Horizontal, outputsCard_);
        row.meter->setThickness(8);
        row.meter->setMinimumWidth(80);
        outputsGrid_->addWidget(row.meter, i, 6);

        applyMonitorOutputRowConnected(row, connected);
        monitorOutputRows_.append(row);
    }

    outputsGrid_->addWidget(streamIcon_, count, 0);
    outputsGrid_->addWidget(streamNameLabel_, count, 1);
    outputsGrid_->addWidget(streamMixName_, count, 2);
    outputsGrid_->addWidget(streamMute_, count, 4);
    outputsGrid_->addWidget(streamVolume_, count, 5);
    outputsGrid_->addWidget(streamMeter_, count, 6);
}

// ================================================================= sidebar

QWidget *MainWindow::buildSidebar() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(12);
    lay->addWidget(buildSoundSharingSection(), 1);
    // Vendor hardware (Clipguard, direct monitor, …) lives in each master's
    // Global Effects panel when that bus is assigned a supported microphone.

    sidebarScroll_ = new QScrollArea(this);
    sidebarScroll_->setWidget(page);
    sidebarScroll_->setWidgetResizable(true);
    sidebarScroll_->setFrameShape(QFrame::NoFrame);
    sidebarScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarScroll_->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    sidebarScroll_->setFixedWidth(kSidebarWidth);

    sidebar_ = sidebarScroll_;
    return sidebar_;
}

Section *MainWindow::buildSoundSharingSection() {
    // Applications, their channel and their microphone sharing are one
    // decision per app, so they live in one place rather than two sections
    // that had to be kept in step by eye.
    auto *s = new Section(tr("Application Settings"), this);
    soundSharingTab_ = new SoundSharingTab(client_, s);
    s->contentLayout()->addWidget(soundSharingTab_);
    return s;
}

Section *MainWindow::buildHardwareSection() {
    // Reaches the device over the vendor protocol; no ALSA control exists for
    // either of these.
    // Built unconditionally and hidden when the profile has no vendor
    // protocol, rather than skipped: the daemon that answers the question may
    // not be running yet when the window is constructed, and a panel that can
    // only appear by rebuilding the sidebar would never appear at all for
    // anyone who starts the GUI first.
    hardwareSection_ = new Section(tr("Microphone hardware"), this);
    auto *s = hardwareSection_;
    auto *lay = s->contentLayout();

    lay->addWidget(buildSwitchRow(tr("Clipguard"), clipguard_,
                                  tr("Hardware anti-clipping inside the "
                                     "microphone.\nStored in the device, so it "
                                     "survives reboots."),
                                  s));
    connect(clipguard_, &QAbstractButton::toggled, client_, &MixerClient::setClipguard);

    lay->addWidget(buildSliderRow(tr("Direct monitor"), hwMonitor_, hwMonitorLabel_,
                                  QStringLiteral("100% you / 100% PC"), s));
    hwMonitor_->setToolTip(
        tr("Hardware direct monitor: your voice blended with the PC's playback "
           "and sent to the headphone jack ON THE MICROPHONE.\n"
           "0% = all PC, 100% = only you. Does nothing for headphones plugged "
           "into the computer -- use the Monitor mix for those."));
    connect(hwMonitor_, &QSlider::sliderPressed, this, [this] { hwMonitorHeld_ = true; });
    connect(hwMonitor_, &QSlider::sliderReleased, this,
            &MainWindow::onHardwareMonitorReleased);
    connect(hwMonitor_, &QSlider::valueChanged, this,
            &MainWindow::onHardwareMonitorMoved);

    // Directly under Direct monitor because they are the same signal path: the
    // USB descriptors put feature unit 5 between the PC's audio and the
    // headphone terminal, with no mixer unit anywhere, so this attenuates the
    // PC leg of the mic's headphone jack and nothing else. Direct monitor sets
    // the blend, this sets how loud the PC side of that blend is.
    //
    // Explicitly NOT tied to the Monitor mix: that is a software mix which may
    // be going to a completely different output device.
    auto *hpRow = buildSliderRow(tr("Headphone"), hpVolume_, hpVolumeLabel_,
                                 QStringLiteral("-60.0 dB"), s);
    // Mute lives on the same row rather than as a switch of its own: it is the
    // same control as the fader beside it, not a separate setting.
    hpMute_ = new IconToggle(QStringLiteral("speaker-muted"),
                             QStringLiteral("speaker-high"), hpRow);
    hpMute_->setIconSize(16);
    hpMute_->setAccent(Theme::Danger);
    hpMute_->setToolTip(tr("Mute the headphone jack on the microphone."));
    hpMute_->setCursor(Qt::PointingHandCursor);
    connect(hpMute_, &QAbstractButton::toggled, client_,
            &MixerClient::setHeadphoneMuted);
    if (auto *hl = qobject_cast<QHBoxLayout *>(hpRow->layout()))
        hl->addWidget(hpMute_);
    lay->addWidget(hpRow);
    // The slider's own units are the ALSA control's: 0..120 in half-decibel
    // steps from -60 dB. Working in the hardware's steps keeps the readout
    // exact instead of rounding a percentage back and forth.
    hpVolume_->setRange(0, 120);
    hpVolume_->setToolTip(
        tr("How loud the computer's audio is in the headphone jack ON THE "
           "MICROPHONE.\n"
           "Does not change your own voice there -- use Direct monitor for the "
           "balance,\nand it does nothing for headphones plugged into the "
           "computer."));
    connect(hpVolume_, &QSlider::sliderPressed, this, [this] { hpVolumeHeld_ = true; });
    connect(hpVolume_, &QSlider::sliderReleased, this, [this] {
        hpVolumeHeld_ = false;
        client_->setHeadphoneVolumeDb(hpVolume_->value() / 2.0 - 60.0);
    });
    connect(hpVolume_, &QSlider::valueChanged, this, [this](int v) {
        const double db = v / 2.0 - 60.0;
        hpVolumeLabel_->setText(tr("%1 dB").arg(db, 0, 'f', 1));
        client_->setHeadphoneVolumeDb(db);
    });

    auto *row = new QHBoxLayout;
    row->addWidget(dimLabel(tr("Device"), s));
    row->addStretch();
    deviceLabel_ = dimLabel(QString(), s);
    row->addWidget(deviceLabel_);
    lay->addLayout(row);

    return s;
}

// ================================================================= helpers

QWidget *MainWindow::buildSliderRow(const QString &label, QSlider *&slider,
                                    QLabel *&readout, const QString &widest,
                                    QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto *name = dimLabel(label, row);
    name->setFixedWidth(92);
    lay->addWidget(name);

    slider = new QSlider(Qt::Horizontal, row);
    slider->setRange(0, 100);
    lay->addWidget(slider, 1);

    readout = fixedReadout(widest, row);
    // fixedReadout sizes itself from `widest` and leaves that string showing.
    // With the daemon down nothing ever overwrites it, so the panel would sit
    // there claiming "100% you / 100% PC".
    readout->setText(QStringLiteral("—"));
    lay->addWidget(readout);
    return row;
}

QWidget *MainWindow::buildSwitchRow(const QString &label, ToggleSwitch *&sw,
                                    const QString &tip, QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto *name = new QLabel(label, row);
    name->setToolTip(tip);
    lay->addWidget(name, 1);

    sw = new ToggleSwitch(row);
    sw->setToolTip(tip);
    lay->addWidget(sw);
    return row;
}

double MainWindow::meterPosition(double rms) {
    return ::meterPosition(rms);
}

// ================================================================== actions

void MainWindow::showProfiles() {
    if (!profilesWindow_) profilesWindow_ = new ProfilesWindow(client_, this);
    profilesWindow_->show();
    profilesWindow_->raise();
    profilesWindow_->activateWindow();
}

void MainWindow::updateHardwareMonitorLabel(int percent) {
    if (percent == 0) hwMonitorLabel_->setText(tr("all PC"));
    else if (percent == 100) hwMonitorLabel_->setText(tr("all mic"));
    else hwMonitorLabel_->setText(tr("%1% you / %2% PC").arg(percent).arg(100 - percent));
}

void MainWindow::onHardwareMonitorMoved(int value) {
    updateHardwareMonitorLabel(value);
    client_->setHardwareMonitor(value);
}

void MainWindow::onHardwareMonitorReleased() {
    hwMonitorHeld_ = false;
    client_->setHardwareMonitor(hwMonitor_->value());
}


void MainWindow::showGlobalEffects(const QString &masterId) {
    if (!globalEffects_.contains(masterId))
        globalEffects_.insert(masterId,
                              new GlobalEffectsWindow(client_, masterId, this));
    GlobalEffectsWindow *w = globalEffects_.value(masterId);
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::showVirtualRack(const QString &masterId, const QString &name) {
    if (!virtualRacks_.contains(masterId))
        virtualRacks_.insert(masterId,
                             new VirtualRackWindow(client_, masterId, name, this));
    VirtualRackWindow *w = virtualRacks_.value(masterId);
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::showAbout() {
    if (!aboutWindow_)
        aboutWindow_ = new AboutWindow(this);
    aboutWindow_->show();
    aboutWindow_->raise();
    aboutWindow_->activateWindow();
}





void MainWindow::refreshRoutingBanner() {
    if (!routingBanner_ || !routingBannerRows_) return;
    const QList<StreamConflictInfo> conflicts =
        client_->available() ? client_->streamRoutingConflicts()
                             : QList<StreamConflictInfo>{};

    QString signature;
    for (const StreamConflictInfo &c : conflicts)
        signature += c.sinkName + QLatin1Char('|') + c.appName + QLatin1Char(';');
    if (signature == routingBannerSignature_) return;
    routingBannerSignature_ = signature;

    while (QLayoutItem *item = routingBannerRows_->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }
    routingBanner_->setVisible(!conflicts.isEmpty());
    if (conflicts.isEmpty()) return;

    for (const StreamConflictInfo &c : conflicts) {
        auto *row = new QWidget(routingBanner_);
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(10);

        auto *text = new QLabel(row);
        text->setWordWrap(true);
        text->setTextFormat(Qt::PlainText);
        // Named by where it went, not by who moved it: PipeWire records the
        // destination and not the mover, and saying more than we know is how a
        // diagnostic sends someone after the wrong program.
        text->setText(tr("“%1” keeps being moved off the %2 channel to “%3”. "
                         "Another program is managing your audio routing — "
                         "Waveline has stopped trying, so this app will not be "
                         "on its channel.")
                          .arg(c.appName, c.channelId, c.sinkLabel));
        QPalette tp = text->palette();
        tp.setColor(QPalette::WindowText, Theme::Text);
        text->setPalette(tp);
        lay->addWidget(text, 1);

        auto *dismiss = new QPushButton(tr("Don't warn me about %1").arg(c.sinkLabel), row);
        dismiss->setToolTip(tr("Stops this warning for “%1” for good.\n"
                               "Undo it under Audio diagnostics.")
                                .arg(c.sinkLabel));
        dismiss->setCursor(Qt::PointingHandCursor);
        const QString sink = c.sinkName;
        connect(dismiss, &QPushButton::clicked, this, [this, sink] {
            client_->dismissStreamRoutingConflict(sink);
            // The daemon drops it and signals, but the click should land now
            // rather than at the next poll.
            routingBannerSignature_.clear();
            refreshRoutingBanner();
        });
        lay->addWidget(dismiss);
        routingBannerRows_->addWidget(row);
    }
}

void MainWindow::showLatencyDiagnostics() {
    if (!settingsWindow_) settingsWindow_ = new SettingsWindow(client_, this);
    settingsWindow_->show();
    settingsWindow_->raise();
    settingsWindow_->activateWindow();
}

QWidget *MainWindow::openTunerWindow() {
    showTuner();
    return tunerWindow_;
}

void MainWindow::showCompanion() {
    if (!companionWindow_)
        companionWindow_ = new CompanionWindow(client_, this);
    companionWindow_->show();
    companionWindow_->raise();
    companionWindow_->activateWindow();
}

void MainWindow::showTuner() {
    if (!tunerWindow_)
        tunerWindow_ = new TunerWindow(client_, this);
    tunerWindow_->show();
    tunerWindow_->raise();
    tunerWindow_->activateWindow();
}

void MainWindow::showSoundboard() {
    if (!soundboardWindow_)
        soundboardWindow_ = new SoundboardWindow(client_, this);
    soundboardWindow_->show();
    soundboardWindow_->raise();
    soundboardWindow_->activateWindow();
}

void MainWindow::showChannelEffects(const QString &id, const QString &name) {
    if (!channelEffects_.contains(id))
        channelEffects_.insert(id, new ChannelEffectsWindow(id, name, client_, this));
    ChannelEffectsWindow *w = channelEffects_.value(id);
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::onOutputPicked(int row, int comboIndex) {
    if (row < 0 || row >= monitorOutputRows_.size()) return;
    QComboBox *combo = monitorOutputRows_[row].combo;
    if (!combo) return;
    const QString name = combo->itemData(comboIndex).toString();
    if (!name.isEmpty()) client_->setMonitorOutputAt(row, name);
}

void MainWindow::onAddMonitorOutput() {
    if (!client_->available()) return;
    const QList<MonitorOutputInfo> states = client_->monitorOutputStates();
    if (states.size() >= 5) return;
    // No free device means no new output: the daemon rejects a second mix on a
    // device that already has one, and starting the row on a duplicate would
    // only produce an assignment that cannot be applied.
    const QString pick = freeMonitorOutputDevice(states, client_->outputs());
    if (pick.isEmpty()) return;
    client_->addMonitorOutput(pick);
}

void MainWindow::onRemoveMonitorOutput(int index) {
    if (!client_->available() || index < 1) return;
    client_->removeMonitorOutput(index);
}

void MainWindow::onAvailabilityChanged(bool available) {
    // Also here, not only from onChanged(): with the daemon down onChanged()
    // returns immediately, and the window would sit there branded for nothing
    // in particular until something started. MixerClient falls back to the
    // profile file exactly so this call has an answer.
    applyDeviceProfile();
    // Clears the banner when the daemon goes away: a routing complaint left
    // on screen above a "wavelined is not running" bar is two explanations for
    // one problem, and the wrong one is the eye-catching one.
    refreshRoutingBanner();

    // The diagnostics button is deliberately NOT gated: with the daemon down
    // it opens and says so, which is more use than a dead control on the one
    // occasion someone is looking for an explanation.
    const QList<QWidget *> gated = {clipguard_, hwMonitor_, outputsCard_,
                                    manageProfiles_};
    for (QWidget *w : gated)
        if (w) w->setEnabled(available);
    for (const MonitorOutputRowUi &row : monitorOutputRows_) {
        if (row.combo) row.combo->setEnabled(available);
        if (row.addBtn) row.addBtn->setEnabled(available);
        if (row.removeBtn) row.removeBtn->setEnabled(available);
        if (row.mute) row.mute->setEnabled(available);
        if (row.volume) row.volume->setEnabled(available);
    }
    if (streamMute_) streamMute_->setEnabled(available);
    if (streamVolume_) streamVolume_->setEnabled(available);
    for (auto *s : strips_) s->setEnabled(available);
    for (auto *s : masterStrips_) s->setEnabled(available);
    if (addMasterBtn_) addMasterBtn_->setEnabled(available);


    if (available) {
        bannerLabel_->setVisible(false);
        onChanged();
    } else {
        bannerLabel_->setText(
            tr("wavelined is not running. Start it with:  systemctl --user start "
               "wavelined\n"
               "The mixer, noise suppression and routing all live in the "
               "daemon, so that audio keeps working when this window is "
               "closed."));
        bannerLabel_->setVisible(true);
    }
}


// ================================================================== refresh

void MainWindow::onAddMasterBusMenu() {
    if (!client_->available()) return;

    QSet<QString> usedCapture;
    QSet<QString> usedMidi;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.busType == QLatin1String("midi"))
            usedMidi.insert(b.captureMatch);
        else
            usedCapture.insert(b.captureMatch);
    }

    QMenu menu(this);
    QMenu *audioMenu = menu.addMenu(tr("Audio input"));
    QMenu *midiMenu = menu.addMenu(tr("MIDI input"));

    auto midiInUse = [&](const QString &match) {
        if (usedMidi.contains(match)) return true;
        const int sep = match.indexOf(QLatin1Char('|'));
        if (sep >= 0 && usedMidi.contains(match.left(sep))) return true;
        for (const QString &u : usedMidi) {
            if (u.contains(QLatin1Char('|'))) continue;
            if (match.startsWith(u + QLatin1Char('|'))) return true;
        }
        return false;
    };

    int audioCount = 0;
    for (const CaptureDeviceInfo &d : client_->captureDevices()) {
        if (usedCapture.contains(d.nodeName)) continue;
        ++audioCount;
        audioMenu->addAction(d.description, this, [this, node = d.nodeName] {
            onAddMasterBus(QStringLiteral("capture"), node);
        });
    }
    if (audioCount == 0)
        audioMenu->addAction(tr("No available audio inputs"))->setEnabled(false);

    int midiCount = 0;
    for (const MidiDeviceInfo &d : client_->midiDevices()) {
        if (midiInUse(d.nodeName)) continue;
        ++midiCount;
        midiMenu->addAction(d.description, this, [this, node = d.nodeName] {
            onAddMasterBus(QStringLiteral("midi"), node);
        });
    }
    if (midiCount == 0)
        midiMenu->addAction(tr("No available MIDI inputs"))->setEnabled(false);

    if (addMasterBtn_) menu.exec(addMasterBtn_->mapToGlobal(QPoint(0, addMasterBtn_->height())));
}

void MainWindow::onAddMasterBus() {
    onAddMasterBus(QStringLiteral("capture"), {});
}

void MainWindow::onAddMasterBus(const QString &busType, const QString &deviceMatch) {
    if (!client_->available()) return;
    const QString id = client_->addMasterBusEx({}, busType, deviceMatch);
    if (id.isEmpty()) {
        const QString err = client_->lastError();
        QMessageBox::warning(
            this, tr("Add input device"),
            err.isEmpty() ? tr("Could not add an input device.") : err);
        return;
    }
}

void MainWindow::onRemoveMasterBus(const QString &id) {
    if (id == QLatin1String("mic") || !client_->available()) return;

    QString name;
    bool found = false;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == id) {
            found = true;
            name = b.name.isEmpty() ? client_->deviceBrand() : b.name;
            break;
        }
    }
    if (!found) {
        rebuildMasterStrips();
        return;
    }

    const int answer = QMessageBox::question(
        this, tr("Remove input device"),
        tr("Remove \"%1\"?\n\nChannels using this microphone will switch "
           "to your primary input.")
            .arg(name));
    if (answer != QMessageBox::Yes) return;

    if (!client_->removeMasterBus(id)) {
        const QString err = client_->lastError();
        QMessageBox::warning(
            this, tr("Remove input device"),
            err.isEmpty() ? tr("Could not remove this input device.") : err);
        rebuildMasterStrips();
        return;
    }
    masterStripOrder_.clear();
    rebuildMasterStrips();
}

void MainWindow::onRebuildMasterCapture(const QString &id) {
    if (!client_->available() || id.isEmpty()) return;

    QString name = id;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == id) {
            name = b.name.isEmpty() ? client_->deviceBrand() : b.name;
            break;
        }
    }

    const int answer = QMessageBox::question(
        this, tr("Rebuild input device"),
        tr("Rebuild \"%1\"?\n\nThis recreates the capture path for this "
           "microphone. Use it if the audio sounds robotic or glitchy. "
           "Playback will drop briefly.")
            .arg(name));
    if (answer != QMessageBox::Yes) return;

    if (!client_->rebuildMasterCapture(id)) {
        const QString err = client_->lastError();
        QMessageBox::warning(
            this, tr("Rebuild input device"),
            err.isEmpty() ? tr("Could not rebuild this input device.") : err);
    }
}

void MainWindow::syncAddMasterButton() {
    ChannelStrip *last = nullptr;
    if (!masterStripOrder_.isEmpty())
        last = masterStrips_.value(masterStripOrder_.last());

    if (!addMasterBtn_) {
        addMasterBtn_ = squareOverlayButton(QStringLiteral("plus"),
                                            tr("Add another input device."),
                                            this);
        connect(addMasterBtn_, &QPushButton::clicked, this, &MainWindow::onAddMasterBusMenu);
    }

    for (auto *s : masterStrips_)
        s->setAddMasterOverlayButton(s == last ? addMasterBtn_ : nullptr);

    if (last) {
        addMasterBtn_->setEnabled(client_->available());
    } else {
        addMasterBtn_->hide();
    }
}

int MainWindow::channelStripInsertIndex() const {
    int at = masterStripOrder_.size();
    if (inputsSeparator_ && inputsSeparator_->isVisible()) ++at;
    return at;
}

void MainWindow::syncInputsSeparator() {
    if (!inputsSeparator_ || !stripRow_) return;
    const bool show = !masterStrips_.isEmpty();
    if (!show) {
        inputsSeparator_->hide();
        return;
    }

    const int wantIndex = masterStripOrder_.size();
    const int curIndex = stripRow_->indexOf(inputsSeparator_);
    if (curIndex != wantIndex) {
        if (curIndex >= 0) stripRow_->removeWidget(inputsSeparator_);
        stripRow_->insertWidget(wantIndex, inputsSeparator_);
    }
    inputsSeparator_->show();
}

void MainWindow::rebuildMasterStrips() {
    const QList<MasterBusInfo> buses = client_->masterBuses();
    QStringList order;
    QString primaryId = QStringLiteral("mic");
    for (const MasterBusInfo &b : buses) {
        if (b.primary) primaryId = b.id;
    }
    order << primaryId;
    for (const MasterBusInfo &b : buses) {
        if (b.id != primaryId) order << b.id;
    }

    if (order == masterStripOrder_ && masterStrips_.size() == order.size()) {
        bool stripsMatch = true;
        for (const QString &id : order) {
            if (!masterStrips_.contains(id)) {
                stripsMatch = false;
                break;
            }
        }
        if (stripsMatch) {
            for (int slot = 0; slot < order.size(); ++slot) {
                const QString &id = order[slot];
                if (auto *s = masterStrips_.value(id)) {
                    MasterBusInfo busInfo;
                    for (const MasterBusInfo &b : buses) {
                        if (b.id == id) {
                            busInfo = b;
                            break;
                        }
                    }
                    const QString key = Theme::masterCardKey(id);
                    const QString defIcon =
                        busInfo.busType == QLatin1String("midi")
                            ? QStringLiteral("music")
                            : QStringLiteral("microphone");
                    s->setIdentityBadge(
                        Theme::cardIcon(key, defIcon),
                        Theme::cardColor(key, Theme::masterBusColor(slot)));
                    s->setMasterSlotNumber(slot + 1);
                    refreshMasterStrip(id, s);
                }
            }
            syncAddMasterButton();
            syncInputsSeparator();
            return;
        }
    }
    masterStripOrder_ = order;

    if (addMasterBtn_) addMasterBtn_->setParent(this);

    for (auto *s : masterStrips_) {
        stripRow_->removeWidget(s);
        s->deleteLater();
    }
    masterStrips_.clear();
    // Keyed by bus id, and the cards holding those ids are gone; the next
    // refreshMasterStrip() fills it back in for whatever replaces them.
    masterMonitor_.clear();

    int at = 0;
    for (int slot = 0; slot < order.size(); ++slot) {
        const QString &id = order[slot];
        MasterBusInfo info;
        for (const MasterBusInfo &b : buses) {
            if (b.id == id) {
                info = b;
                break;
            }
        }
        const bool primary = info.primary || id == primaryId;
        const QString name = info.name.isEmpty() ? client_->deviceBrand() : info.name;

        auto *strip = new ChannelStrip(id, name, nullptr);
        {
            const QString key = Theme::masterCardKey(id);
            const QString defIcon = info.busType == QLatin1String("midi")
                                        ? QStringLiteral("music")
                                        : QStringLiteral("microphone");
            strip->setIdentityBadge(
                Theme::cardIcon(key, defIcon),
                Theme::cardColor(key, Theme::masterBusColor(slot)));
        }
        connect(strip, &ChannelStrip::identityClicked, this, [this, id] {
            editCardIdentity(QStringLiteral("master"), id);
        });
        {
            const QString cardKey = Theme::masterCardKey(id);
            strip->setFadersLinked(loadFadersLinked(cardKey));
            connect(strip, &ChannelStrip::fadersLinkedChanged, this,
                    [cardKey](bool on) { saveFadersLinked(cardKey, on); });
        }
        strip->setMasterSlotNumber(slot + 1);
        strip->enableEditableTitle();
        connect(strip, &ChannelStrip::displayNameEdited, this,
                [this, id](const QString &name) { client_->setMasterName(id, name); });
        if (info.busType != QLatin1String("midi")) {
            strip->enableMasterRebuild(true);
            connect(strip, &ChannelStrip::masterRebuildRequested, this,
                    [this, id] { onRebuildMasterCapture(id); });
        }
        if (primary) {
            strip->setToolTip(tr("Your primary input.\n"
                                 "Stream is how loudly your audience hears you."));
        } else {
            strip->enableMasterDelete(true);
            connect(strip, &ChannelStrip::masterDeleteRequested, this,
                    [this, id] { onRemoveMasterBus(id); });
        }
        // The Monitor half of both signals also drives this card's top meter,
        // so the cache is updated here rather than waiting for the daemon's
        // Changed to come back: a fader move must not leave the bar reading
        // the old level for a round trip.
        connect(strip, &ChannelStrip::volumeChanged, this,
                [this, id](const QString &, const QString &mix, double v) {
                    if (mix == QLatin1String("monitor"))
                        masterMonitor_[id].volume = v;
                    client_->setMasterMicVolume(id, mix, v);
                });
        connect(strip, &ChannelStrip::muteToggled, this,
                [this, id](const QString &, const QString &mix, bool m) {
                    if (mix == QLatin1String("monitor")) {
                        masterMonitor_[id].muted = m;
                        applyMasterMonitorState(id);
                    }
                    client_->setMasterMicMixMuted(id, mix, m);
                });
        strip->setMicMeterActive(true);
        strip->setMeterTooltips(
            tr("What you hear from this input device."),
            primary ? tr("Your microphone, after noise suppression and EQ. This is "
                         "what the mixes receive.")
                    : tr("This microphone, after noise suppression and EQ. This is "
                         "what the mixes receive."));
        strip->setMonitorColumnTip(
            tr("How loudly you hear this microphone in the Monitor mix.\n"
               "Use the ear toggle to enable monitoring. To change everything "
               "you hear on a device, use that device's Monitor mix level below."),
            tr("Mute this microphone in the Monitor mix.\n"
               "Your audience is unaffected."));
        strip->enableMicMonitorButton();
        if (IconToggle *ear = strip->micMonitorButton()) {
            ear->setToolTip(tr("Hear this input device in the Monitor mix.\n"
                               "Software only — routes the mic into your headphones."));
            ear->setVisible(true);
            connect(ear, &QAbstractButton::toggled, this, [this, id](bool on) {
                masterMonitor_[id].monitoring = on;
                applyMasterMonitorState(id);
                client_->setMasterSoftwareMonitor(id, on);
            });
        }

        strip->enableGainFader();
        strip->enableEffectsButton();
        strip->enableSettingsButton();
        if (IconToggle *gear = strip->settingsButton()) {
            gear->setToolTip(tr("Open Global Effects for this input device."));
            connect(gear, &QAbstractButton::clicked, this,
                    [this, id] { showGlobalEffects(id); });
        }
        connect(strip, &ChannelStrip::gainChanged, this, [this, id](double v) {
            client_->setMasterMicInputVolume(id, v / 100.0);
        });
        connect(strip, &ChannelStrip::hardwareMuteToggled, this,
                [this, id](bool muted) {
                    client_->setMasterMicInputMuted(id, muted);
                });
        if (IconToggle *fx = strip->effectsButton()) {
            fx->setToolTip(tr("Global microphone effects on/off (noise suppression, EQ).\n"
                              "Your settings are kept, so turning it back on restores "
                              "the chain.\n"
                              "The gear beside it opens Global Effects.\n"
                              "Right-click cycles the states in reverse.\n"
                              "Middle-click opens the Creative FX Virtual Rack."));
            connectMasterEffectsButton(fx, client_, id, strip);
            connect(fx, &IconToggle::middleClicked, this,
                    [this, id, name] { showVirtualRack(id, name); });
        }

        stripRow_->insertWidget(at++, strip);
        masterStrips_.insert(id, strip);
        profileApplied_ = false;
    }

    syncAddMasterButton();
    syncInputsSeparator();
    emptyLabel_->setVisible(masterStrips_.isEmpty());
}

void MainWindow::refreshMasterStrip(const QString &masterId, ChannelStrip *strip) {
    if (!strip || !client_->available()) return;

    bool connected = true;
    bool primary = masterId == QLatin1String("mic");
    QString deviceLabel;
    qint64 latencyUs = -1;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == masterId) {
            strip->setDisplayName(b.name);
            connected = b.deviceConnected;
            deviceLabel = b.deviceLabel;
            primary = b.primary;
            latencyUs = b.latencyUs;
            break;
        }
    }
    strip->setDeviceConnected(connected);
    // An unplugged device's last known figure describes hardware that is not
    // there; better to show nothing than a number nothing is producing.
    strip->setInputLatencyUs(connected ? latencyUs : -1);

    const double monitorVolume =
        client_->masterMicVolume(masterId, QStringLiteral("monitor"));
    const bool monitorMuted =
        client_->masterMicMixMuted(masterId, QStringLiteral("monitor"));
    strip->setValues(client_->masterMicVolume(masterId, QStringLiteral("stream")),
                     client_->masterMicMixMuted(masterId, QStringLiteral("stream")),
                     monitorVolume, monitorMuted);

    strip->setGainStyle(ChannelStrip::GainStyle::InputLevel);
    strip->setGainValue(client_->masterMicInputVolume(masterId) * 100.0);
    strip->setHardwareMuted(client_->masterMicInputMuted(masterId));
    // Input level and hardware mute act on the capture device itself, so they
    // have nothing to act on while it is unplugged.
    strip->setHardwareControlsEnabled(connected);

    if (IconToggle *fx = strip->effectsButton()) {
        QSignalBlocker b(fx);
        const bool on = client_->masterMicEffectsEnabled(masterId);
        const bool mon = on && client_->masterMicMonitorFx(masterId);
        fx->setChecked(on);
        fx->setAccent(effectsAccent(mon, strip->accentColor()));
        fx->setToolTip(
            mon ? tr("Effects ON — headphones hear the chain (diagnostic).\n"
                     "Click to turn effects off. The gear opens Global Effects.")
                : on ? tr("Effects ON — headphones hear the dry microphone.\n"
                          "Click to route the chain to your headphones.\n"
                          "The gear opens Global Effects.")
                     : tr("Effects OFF for the microphone. Settings are kept.\n"
                          "Click to turn them back on. The gear opens Global "
                          "Effects."));
    }

    strip->setGainStyle(ChannelStrip::GainStyle::InputLevel);
    const bool monitoring = client_->masterSoftwareMonitor(masterId);
    strip->setMicMonitorChecked(monitoring);
    // Says why the card is grey, and restores the card's own caption when the
    // device comes back rather than leaving the warning standing.
    if (!connected) {
        strip->setToolTip(tr("%1 is not connected.\n"
                             "This input device stays set up for it and picks "
                             "it up again when it comes back.")
                              .arg(deviceLabel.isEmpty() ? tr("This device")
                                                         : deviceLabel));
    } else {
        strip->setToolTip(primary ? tr("Your primary input.\n"
                                       "Stream is how loudly your audience "
                                       "hears you.")
                                  : QString());
    }

    MasterMonitorUi &m = masterMonitor_[masterId];
    m.monitoring = monitoring;
    m.muted = monitorMuted;
    m.volume = monitorVolume;
    applyMasterMonitorState(masterId);
}

void MainWindow::refreshCardLooks() {
    if (!client_->available()) return;
    const QHash<QString, MixerClient::CardAppearance> saved =
        client_->cardAppearances();

    QStringList sigParts;
    QHash<QString, Theme::CardLook> looks;
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        Theme::CardLook look;
        look.color = QColor(it->color);
        look.icon = it->icon;
        looks.insert(it.key(), look);
        sigParts << it.key() + QLatin1Char('\t') + it->color + QLatin1Char('\t') +
                        it->icon;
    }
    sigParts.sort();
    const QString sig = sigParts.join(QLatin1Char('\n'));
    if (sig == cardLooksSig_) return;
    cardLooksSig_ = sig;

    Theme::setCardLooks(looks);
    // Written through for the next launch, so the window opens in the colours
    // it closed in rather than in the theme's.
    QSettings().setValue(QLatin1String(kCardLooksKey), sig);
    applyCardLooks();
}

void MainWindow::applyCachedCardLooks() {
    const QString table = QSettings().value(QLatin1String(kCardLooksKey)).toString();
    if (table.isEmpty()) return;
    cardLooksSig_ = table;
    Theme::setCardLooks(looksFromTable(table));
}

void MainWindow::applyCardLooks() {
    const QList<MasterBusInfo> buses = client_->masterBuses();
    for (int slot = 0; slot < masterStripOrder_.size(); ++slot) {
        const QString id = masterStripOrder_.at(slot);
        ChannelStrip *strip = masterStrips_.value(id);
        if (!strip) continue;
        bool midi = false;
        for (const MasterBusInfo &b : buses)
            if (b.id == id) midi = b.busType == QLatin1String("midi");
        const QString key = Theme::masterCardKey(id);
        strip->setIdentityBadge(
            Theme::cardIcon(key, midi ? QStringLiteral("music")
                                      : QStringLiteral("microphone")),
            Theme::cardColor(key, Theme::masterBusColor(slot)));
    }
    // Channel cards key off their own id, so channelColor()/channelIconName()
    // already answer with the override where there is one.
    for (auto it = strips_.begin(); it != strips_.end(); ++it) {
        it.value()->setIdentityBadge(Theme::channelIconName(it.key()),
                                     Theme::channelColor(it.key()));
    }
}

void MainWindow::editCardIdentity(const QString &kind, const QString &id) {
    if (!client_->available()) return;
    const bool master = kind == QLatin1String("master");
    const QString key =
        master ? Theme::masterCardKey(id) : Theme::channelCardKey(id);

    QString name;
    QColor defColor;
    QString defIcon;
    if (master) {
        const int slot = masterStripOrder_.indexOf(id);
        defColor = Theme::masterBusColor(slot < 0 ? 0 : slot);
        defIcon = QStringLiteral("microphone");
        for (const MasterBusInfo &b : client_->masterBuses()) {
            if (b.id != id) continue;
            name = b.name;
            if (b.busType == QLatin1String("midi")) defIcon = QStringLiteral("music");
            break;
        }
    } else {
        defColor = Theme::channelColorDefault(id);
        defIcon = Theme::channelIconNameDefault(id);
        for (const ChannelInfo &c : client_->channels())
            if (c.id == id) name = c.name;
    }

    const MixerClient::CardAppearance saved = client_->cardAppearances().value(key);
    CardIdentityDialog dlg(tr("Card — %1").arg(name), name, QColor(saved.color),
                           saved.icon, defColor, defIcon, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString title = dlg.name();
    if (!title.isEmpty() && title != name) {
        if (master)
            client_->setMasterName(id, title);
        else
            client_->setChannelName(id, title);
    }
    const QColor color = dlg.color();
    client_->setCardAppearance(
        key, color.isValid() ? color.name(QColor::HexRgb) : QString(), dlg.icon());

    // Applied here rather than waited for: the card the user was just looking
    // at should be wearing its new colour as the dialog closes. The strips are
    // repainted directly; refresh() then rebuilds the Apps and sharing tabs,
    // which read the same table. The next poll confirms all of it from the
    // daemon -- and cardLooksSig_ is deliberately left stale so it does.
    Theme::CardLook look;
    look.color = color;
    look.icon = dlg.icon();
    Theme::setCardLook(key, look);
    applyCardLooks();
    client_->refresh();
}

void MainWindow::applyMasterMonitorState(const QString &masterId) {
    ChannelStrip *strip = masterStrips_.value(masterId);
    if (!strip) return;
    const MasterMonitorUi m = masterMonitor_.value(masterId);
    const bool live = m.monitoring && !m.muted;
    strip->setMainMeterActive(live);
    strip->setMainMeterTooltip(
        live ? tr("What you hear from this input device: its send into the "
                  "Monitor mix, at this card's Monitor fader.")
             : m.monitoring
                   ? tr("Muted in the Monitor mix, so there is nothing to hear "
                        "from this input device.\n"
                        "Your audience is unaffected.")
                   : tr("Not monitored, so there is nothing to hear from this "
                        "input device.\n"
                        "Turn on the ear toggle to monitor it. Hardware direct "
                        "monitoring never enters the mixer and cannot be "
                        "metered here."));
}

void MainWindow::loadChannelOrder() {
    channelOrder_ = loadChannelOrderSetting();
}

QList<ChannelInfo> MainWindow::orderedChannels() const {
    const QList<ChannelInfo> channels = client_->channels();
    if (channelOrder_.isEmpty()) return channels;

    QList<ChannelInfo> out;
    out.reserve(channels.size());
    // Remembered ones first, in the remembered order.
    for (const QString &id : channelOrder_) {
        for (const ChannelInfo &c : channels) {
            if (c.id == id) {
                out.append(c);
                break;
            }
        }
    }
    // Then anything the user has never moved, still in the daemon's order, so
    // a channel created just now turns up where it would have without any of
    // this.
    for (const ChannelInfo &c : channels) {
        if (!channelOrder_.contains(c.id)) out.append(c);
    }
    return out;
}

void MainWindow::saveChannelOrderFromRow() {
    if (!stripRow_) return;
    // Read back from the row rather than from a model of it: the live drag
    // moves the widgets, so the widgets are the truth by the time this runs.
    QStringList order;
    for (int i = 0; i < stripRow_->count(); ++i) {
        QWidget *w = stripRow_->itemAt(i)->widget();
        if (!w) continue;
        auto *strip = qobject_cast<ChannelStrip *>(w);
        if (!strip) continue;
        const QString id = strip->channelId();
        if (strips_.contains(id)) order << id;
    }
    // Ids that belong to channels the daemon is not reporting right now are
    // kept, at the back: an application that is closed should still come back
    // where it was put.
    for (const QString &id : channelOrder_) {
        if (!order.contains(id)) order << id;
    }
    channelOrder_ = order;
    saveChannelOrderSetting(order);
}

int MainWindow::channelDropIndex(const QPoint &hostPos) const {
    // The gap the cursor is nearest, expressed as a layout index. Each card
    // claims the half of itself nearest the gap in question, which is what
    // makes the card under the cursor swap rather than refuse to move.
    int index = -1;
    for (int i = 0; i < stripRow_->count(); ++i) {
        auto *strip = qobject_cast<ChannelStrip *>(stripRow_->itemAt(i)->widget());
        if (!strip || !strips_.contains(strip->channelId())) continue;
        if (hostPos.x() < strip->geometry().center().x()) return i;
        index = i + 1;
    }
    // Past the centre of every card: the gap after the last one.
    return index;
}

bool MainWindow::handleStripDrag(QEvent *event) {
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *de = static_cast<QDragMoveEvent *>(event);
        if (!de->mimeData()->hasFormat(
                QString::fromLatin1(ChannelStrip::dragMimeType())))
            return false;
        const QString id = QString::fromUtf8(de->mimeData()->data(
            QString::fromLatin1(ChannelStrip::dragMimeType())));
        ChannelStrip *strip = strips_.value(id);
        if (!strip) return false;
        de->setDropAction(Qt::MoveAction);
        de->accept();

        // Reorder as the cursor moves rather than only on drop: the row is
        // its own preview, so there is no separate insertion marker to draw
        // and no way for the two to disagree.
        const int want = channelDropIndex(de->position().toPoint());
        const int have = stripRow_->indexOf(strip);
        if (want >= 0 && have >= 0 && want != have && want != have + 1) {
            stripRow_->removeWidget(strip);
            stripRow_->insertWidget(want > have ? want - 1 : want, strip);
        }
        return true;
    }
    case QEvent::Drop: {
        auto *de = static_cast<QDropEvent *>(event);
        if (!de->mimeData()->hasFormat(
                QString::fromLatin1(ChannelStrip::dragMimeType())))
            return false;
        de->setDropAction(Qt::MoveAction);
        de->accept();
        // The row already shows the answer; all that is left is to keep it.
        saveChannelOrderFromRow();
        return true;
    }
    default:
        return false;
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (stripHost_ && watched == stripHost_ && handleStripDrag(event))
        return true;
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::rebuildStrips() {
    rebuildMasterStrips();

    const auto channels = orderedChannels();
    if (strips_.size() == channels.size()) return;

    for (auto *s : strips_) { stripRow_->removeWidget(s); s->deleteLater(); }
    strips_.clear();
    int at = channelStripInsertIndex();
    for (const auto &c : channels) {
        auto *strip = new ChannelStrip(c.id, c.name, nullptr);
        // Same identity affordances as an input device: the tile opens the
        // card's panel, and the title is editable in place.
        connect(strip, &ChannelStrip::identityClicked, this, [this, id = c.id] {
            editCardIdentity(QStringLiteral("channel"), id);
        });
        {
            const QString cardKey = Theme::channelCardKey(c.id);
            strip->setFadersLinked(loadFadersLinked(cardKey));
            connect(strip, &ChannelStrip::fadersLinkedChanged, this,
                    [cardKey](bool on) { saveFadersLinked(cardKey, on); });
        }
        // Channel cards only. The masters are ordered by the daemon -- slot 1
        // is the primary bus and the numbering means something -- so dragging
        // one somewhere else would be a lie the next refresh undoes.
        strip->enableDragHandle();
        strip->enableEditableTitle();
        connect(strip, &ChannelStrip::displayNameEdited, this,
                [this, id = c.id](const QString &name) {
                    client_->setChannelName(id, name);
                });
        connect(strip, &ChannelStrip::volumeChanged, client_,
                &MixerClient::setChannelVolume);
        connect(strip, &ChannelStrip::muteToggled, client_,
                &MixerClient::setChannelMuted);
        strip->enableEffectsButton();
        strip->enableSettingsButton();
        if (IconToggle *gear = strip->settingsButton())
            connect(gear, &QAbstractButton::clicked, this,
                    [this, id = c.id, name = c.name] { showChannelEffects(id, name); });
        strip->enableMicMonitorButton();
        if (IconToggle *ear = strip->micMonitorButton()) {
            ear->setToolTip(tr("Hear this channel's published microphone in the "
                               "Monitor mix.\n"
                               "Software only: there is no hardware monitor for "
                               "a channel mic."));
            connect(ear, &QAbstractButton::toggled, this,
                    [this, id = c.id](bool on) {
                        client_->setChannelMicMonitor(id, on);
                    });
        }
        strip->enableMicSendFader();
        connect(strip, &ChannelStrip::micSendChanged, client_,
                &MixerClient::setChannelMicSend);
        connect(strip, &ChannelStrip::micMuteToggled, client_,
                &MixerClient::setChannelMicMuted);
        if (IconToggle *fx = strip->effectsButton()) {
            connectEffectsButton(fx, client_, c.id, strip);
        }
        stripRow_->insertWidget(at++, strip);
        strips_.insert(c.id, strip);
    }
}

void MainWindow::onLevels() {
    if (!client_->available()) return;
    const auto &l = client_->levels();

    for (auto it = strips_.begin(); it != strips_.end(); ++it) {
        it.value()->setLevel(meterPosition(l.value(it.key(), 0.0)));
        it.value()->setMicLevel(
            meterPosition(l.value(it.key() + QStringLiteral("-mic"), 0.0)));
    }

    const double monitorMixLevel =
        meterPosition(l.value(QStringLiteral("monitor-mix"), 0.0));
    for (auto it = masterStrips_.begin(); it != masterStrips_.end(); ++it) {
        // The published input device, not the noise filter's output: shared
        // application audio joins the bus downstream of the filter, so only
        // this tap shows the device as its listeners actually hear it.
        const double src =
            meterPosition(l.value(it.key() + QStringLiteral("-src"), 0.0));
        it.value()->setMicLevel(src);
        // Top bar: this device's own send into the Monitor mix -- what you
        // hear from *this* input, and nothing else. It used to show the whole
        // Monitor mix, which put an identical bar on every input device and
        // moved it for audio that card had no part in.
        //
        // The send is a loopback carrying `src` at this card's Monitor fader,
        // muted while the ear toggle is off, so it is the same pre-fader
        // formula the monitor output rows use below.
        const MasterMonitorUi m = masterMonitor_.value(it.key());
        it.value()->setLevel(m.monitoring && !m.muted ? src * m.volume : 0.0);
    }

    for (const MonitorOutputRowUi &row : monitorOutputRows_) {
        if (!row.meter) continue;
        if (!row.meter->isEnabled()) {
            row.meter->setLevel(0.0);
            continue;
        }
        double level = monitorMixLevel;
        // monitor-mix is upstream of each output's fader and mute; scale so the
        // bar reflects what would come out of this device, not the shared bus.
        if (row.mute && row.mute->isChecked())
            level = 0.0;
        else if (row.volume)
            level *= row.volume->value() / 100.0;
        row.meter->setLevel(level);
    }
    // Stream volume is channelVolumes on the sink, so the probe already hears
    // post-fader audio. Monitor meters do meterPosition(pre)*fader instead.
    // Applying the log curve to a post-fader peak (or also multiplying again)
    // will not match — undo the fader, then use the same formula as Monitor.
    {
        const double fader =
            (streamMute_ && streamMute_->isChecked())
                ? 0.0
                : (streamVolume_ ? streamVolume_->value() / 100.0 : 1.0);
        if (fader <= 0.0) {
            streamMeter_->setLevel(0.0);
        } else {
            const double post = l.value(QStringLiteral("stream-mix"), 0.0);
            const double pre = std::min(1.0, post / fader);
            streamMeter_->setLevel(meterPosition(pre) * fader);
        }
    }
}

void MainWindow::applyDeviceProfile() {
    const QString brand = client_->deviceBrand();
    if (profileApplied_ && brand == appliedBrand_) return;
    appliedBrand_ = brand;
    appliedHardware_ = false;
    profileApplied_ = true;

    setWindowTitle(tr("Waveline"));

    if (streamMixName_)
        streamMixName_->setFullText(tr("%1 Stream Mix").arg(brand));

    for (auto *strip : masterStrips_)
        strip->setGainStyle(ChannelStrip::GainStyle::InputLevel);
    ChannelStrip *primaryStrip = masterStrips_.value(QStringLiteral("mic"));
    if (!primaryStrip && !masterStripOrder_.isEmpty())
        primaryStrip = masterStrips_.value(masterStripOrder_.first());
    if (primaryStrip) primaryStrip->enableGainFader(true);
}

void MainWindow::onChanged() {
    if (!client_->available()) return;

    // refreshCardLooks() has already run for this signal, undebounced, so the
    // colours below are current.
    rebuildStrips();
    applyDeviceProfile();
    refreshRoutingBanner();

    for (const auto &c : client_->channels()) {
        if (auto *s = strips_.value(c.id)) {
            // Renaming a channel changes no ids, so the strips are not rebuilt
            // for it -- the title has to be pushed on every refresh.
            s->setDisplayName(c.name);
            s->setValues(c.streamVolume, c.streamMuted, c.monitorVolume,
                         c.monitorMuted);
        }
    }

    const QList<MasterBusInfo> buses = client_->masterBuses();
    for (const MasterBusInfo &b : buses) {
        if (auto *s = masterStrips_.value(b.id))
            refreshMasterStrip(b.id, s);
    }

    for (const auto &c : client_->channels()) {
        if (auto *s = strips_.value(c.id)) {
            if (IconToggle *fx = s->effectsButton()) {
                QSignalBlocker b(fx);
                const bool on = client_->channelEffectsEnabled(c.id);
                const bool mon = on && client_->channelMonitorFx(c.id);
                fx->setChecked(on);
                fx->setAccent(effectsAccent(mon, s->accentColor()));
                fx->setToolTip(
                    mon ? tr("Effects ON — headphones hear the chain (diagnostic).\n"
                             "Click to turn effects off. The gear opens the panel.")
                        : on ? tr("Effects ON — headphones hear the dry channel.\n"
                                  "Click to route the chain to your headphones.\n"
                                  "The gear opens the panel.")
                             : tr("Effects OFF for this channel. Settings are kept.\n"
                                  "Click to turn them back on. The gear opens the "
                                  "panel."));
            }
            s->setMicSend(client_->channelMicSend(c.id));
            const bool hasMic = client_->channelMicSource(c.id);
            s->setMicMeterActive(hasMic);
            s->setMicControlsActive(hasMic);
            s->setMicMonitorButtonVisible(hasMic);
            if (IconToggle *ear = s->micMonitorButton()) {
                QSignalBlocker b(ear);
                ear->setChecked(hasMic && client_->channelMicMonitor(c.id));
            }
            s->setMicMuted(client_->channelMicMuted(c.id));
        }
    }

    // refreshMasterStrip() already set these from each bus's own connection
    // state; a blanket re-enable here would light the controls back up on a
    // card whose device is unplugged.

    // Nothing to refresh for profiles on this timer any more: the list and
    // which one is loaded are the profiles panel's own business, and it is
    // built when it is opened.

    syncMonitorOutputUi();
    refreshMonitorOutputControls();
}
