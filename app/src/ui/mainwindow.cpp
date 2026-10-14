// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "mainwindow.h"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QComboBox>
#include <QActionGroup>
#include <QDialog>
#include <QDropEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

#include "aboutwindow.h"
#include "settingswindow.h"
#include "cardidentity.h"
#include "cardshuffle.h"
#include "channelstrip.h"
#include "creativerackwindow.h"
#include "mixcard.h"
#include "effectswindow.h"
#include "levelmeter.h"
#include "monarchy/chrome.h"
#include "monarchy/desktop.h"
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
// The window's inset for its content. The row of cards is the exception: it
// runs to the window's edges and carries this as padding inside its scroll.
constexpr int kContentInset = 16;

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
    Theme::setTextColor(l, Theme::TextDim);
    return l;
}

// The cards that are not an input or a channel keep their looks in the same
// table, under keys of their own. A Monitor mix's goes with its number, which
// is what stays put when two are swapped.
QString monitorMixCardKey(int index) {
    return QStringLiteral("mix:monitor:%1").arg(index + 1);
}
const QString kStreamMixCardKey = QStringLiteral("mix:stream");
const QString kAppSettingsCardKey = QStringLiteral("card:app-settings");

// Defaults of their own, clear of the input and channel palettes, so a mix is
// never mistaken for the card feeding it.
QColor monitorMixColor(int index) {
    static const QColor kPalette[] = {
        QColor(0x14, 0xb8, 0xa6),  // 1 teal
        QColor(0x7d, 0xd3, 0xfc),  // 2 pale sky
        QColor(0xfb, 0x71, 0x85),  // 3 coral
        QColor(0xd6, 0xb3, 0x70),  // 4 sand
        QColor(0xc4, 0xb5, 0xfd),  // 5 lavender
    };
    const int n = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
    return kPalette[((index % n) + n) % n];
}
const QColor kStreamMixColor(0xfa, 0xcc, 0x15);    // yellow
const QColor kAppSettingsColor(0x94, 0xa3, 0xb8);  // slate

QPushButton *squareOverlayButton(const QString &icon, const QString &tip,
                                 QWidget *parent) {
    auto *b = new QPushButton(parent);
    b->setIcon(Theme::icon(icon, Theme::TextDim, 12));
    b->setFixedSize(18, 18);
    b->setFlat(true);
    b->setToolTip(tip);
    return b;
}

// Cards in a group stand this far apart; the groups themselves -- mixes,
// input devices, channels, Application Settings -- four times as far. Nothing
// in the gap: the gap is the divider.
constexpr int kCardSpacing = 8;
constexpr int kGroupSpacing = 32;

// The row's groups, by the names their order is remembered under.
const QString kUnitMixes = QStringLiteral("mixes");
const QString kUnitInputs = QStringLiteral("inputs");
const QString kUnitChannels = QStringLiteral("channels");
const QString kUnitApp = QStringLiteral("app");

// A group of cards side by side, as one widget in the row so it can be moved
// as one.
QWidget *groupHost(QWidget *parent, QHBoxLayout **row) {
    auto *w = new QWidget(parent);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *lay = new QHBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(kCardSpacing);
    *row = lay;
    return w;
}

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

}  // namespace

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

    // Which look: the universal one, or Monarchy's own chrome when that is the
    // desktop this is running on. See ui/monarchy/desktop.h.
    const bool monarchy = Monarchy::isActive();
    if (monarchy) Monarchy::makeFrameless(this);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    if (monarchy) {
        // The one window tint, and its margin is the grab strip: with no
        // decoration, it is the only part of the window a resize can reach.
        monarchyCentral_ = central;
        central->setObjectName(QStringLiteral("monarchyCentral"));
        outer->setContentsMargins(Monarchy::kResizeMargin, Monarchy::kResizeMargin,
                                  Monarchy::kResizeMargin, Monarchy::kResizeMargin);
        central->installEventFilter(this);
        outer->addWidget(buildMonarchyHeader());
    } else {
        outer->addWidget(buildHeader());
    }

    bannerLabel_ = new QLabel(central);
    bannerLabel_->setWordWrap(true);
    bannerLabel_->setVisible(false);
    bannerLabel_->setContentsMargins(16, 10, 16, 10);
    Theme::follow(bannerLabel_, [this] {
        bannerLabel_->setStyleSheet(
            QStringLiteral("QLabel { background: %1; color: %2; }")
                .arg(Theme::DangerGround.name(), Theme::Text.name()));
    });
    outer->addWidget(bannerLabel_);

    // Amber, not the red above it: the daemon being gone means nothing works,
    // this means one application is going to the wrong place. Sorting those
    // two into the same colour is how a warning stops being read.
    routingBanner_ = new QWidget(central);
    routingBanner_->setVisible(false);
    Theme::follow(routingBanner_, [this] {
        routingBanner_->setStyleSheet(
            QStringLiteral("QWidget { background: %1; }")
                .arg(Theme::WarnGround.name()));
    });
    routingBannerRows_ = new QVBoxLayout(routingBanner_);
    routingBannerRows_->setContentsMargins(16, 8, 16, 8);
    routingBannerRows_->setSpacing(6);
    outer->addWidget(routingBanner_);

    // No side margins on the body: the row of cards runs from one edge of the
    // window to the other and is cut off by it, with its padding inside the
    // scrolled row instead (see buildInputs()). Everything is in that row now
    // -- inputs, channels, Application Settings and the mixes -- so there is
    // nothing below it and nothing laid over it.
    auto *bodyWidget = new QWidget(central);
    bodyWidget_ = bodyWidget;
    auto *body = new QHBoxLayout(bodyWidget);
    body->setContentsMargins(0, 14, 0, 0);
    body->setSpacing(0);
    body->addWidget(buildInputs(), 1);

    outer->addWidget(bodyWidget, 1);
    setCentralWidget(central);

    if (monarchy) {
        buildMonarchyMenuBar();
        Monarchy::prepareResizeStrip(central);
        applyMonarchyStyle();
        connect(&Monarchy::Settings::instance(), &Monarchy::Settings::changed, this,
                &MainWindow::applyMonarchyStyle);
    }

    setUpdatesEnabled(false);
    onAvailabilityChanged(client_->available());
    onChanged();
    setUpdatesEnabled(true);
    // After a scheme change, once more: the rows it rebuilds (combo entries,
    // the routing banner) take their colours as they are built.
    Theme::onChange(this, [this] { onChanged(); });
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
    // The row of cards, which is where Application Settings lives now.
    if (stripScroll_) stripScroll_->horizontalScrollBar()->setValue(pixels);
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

    addHeaderPills(bar, lay, Theme::Card);

    return bar;
}

void MainWindow::addHeaderPills(QWidget *bar, QHBoxLayout *lay, const QColor &chrome) {
    // Icons in Monarchy's rounded pills, in both looks: one pill per group of
    // related destinations, the way Compass groups its back/forward pair --
    // the mixer's own setup, the tools that sit beside it, and About on its
    // own at the end. The names are in the tooltips, and on Monarchy in the
    // global menu too.
    diagnosticsBtn_ = Monarchy::toolButton(
        QStringLiteral("gear"),
        tr("Settings & Diagnostics: noise suppression, graph latency, output "
           "headroom, warnings, service status and measured latency per "
           "device."), bar);
    manageProfiles_ = Monarchy::toolButton(
        QStringLiteral("switch-profile"),
        tr("Profiles: switch between saved mixer setups, and save, rename, "
           "delete, export and import them."), bar);
    tunerBtn_ = Monarchy::toolButton(
        QStringLiteral("tuning-fork"),
        tr("Tuner: tune an instrument from any input, without disturbing your "
           "mixer routing."), bar);
    soundboardBtn_ = Monarchy::toolButton(
        QStringLiteral("sfx"),
        tr("Soundboard: play sound clips onto a channel, share them into a "
           "microphone, and trigger them from a Stream Deck with wavelined-cli."), bar);
    companionBtn_ = Monarchy::toolButton(
        QStringLiteral("world"),
        tr("Companion: control this mixer from a phone or tablet on the same "
           "network."), bar);
    aboutBtn_ = Monarchy::toolButton(QStringLiteral("info"), tr("About Waveline"), bar);

    lay->addWidget(Monarchy::pillGroup(bar, {diagnosticsBtn_, manageProfiles_}, chrome));
    lay->addWidget(Monarchy::pillGroup(bar, {tunerBtn_, soundboardBtn_, companionBtn_}, chrome));
    lay->addWidget(Monarchy::pillGroup(bar, {aboutBtn_}, chrome));

    connect(diagnosticsBtn_, &QAbstractButton::clicked, this,
            &MainWindow::showLatencyDiagnostics);
    connect(manageProfiles_, &QAbstractButton::clicked, this, &MainWindow::showProfiles);
    connect(tunerBtn_, &QAbstractButton::clicked, this, &MainWindow::showTuner);
    connect(soundboardBtn_, &QAbstractButton::clicked, this, &MainWindow::showSoundboard);
    connect(companionBtn_, &QAbstractButton::clicked, this, &MainWindow::showCompanion);
    connect(aboutBtn_, &QAbstractButton::clicked, this, &MainWindow::showAbout);
}

QWidget *MainWindow::buildMonarchyHeader() {
    // Laid out the way Monarchy's own toolbars are: 36 px pills in a 46 px
    // row, 10 px in from the sides, with nothing painted behind the row -- the
    // window's tint runs straight up to the top edge.
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("monarchyHeader"));
    bar->setFixedHeight(Monarchy::kHeaderHeight);
    bar->installEventFilter(this);
    monarchyHeader_ = bar;

    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(10, 5, 10, 5);
    lay->setSpacing(8);

    lay->addWidget(new Monarchy::WindowButtons(bar), 0, Qt::AlignVCenter);
    lay->addSpacing(8);

    monarchyTitle_ = new QLabel(tr("Waveline"), bar);
    monarchyTitle_->setObjectName(QStringLiteral("monarchyTitle"));
    // Presses on the name drag the window too; the label would otherwise
    // swallow them before the header's filter saw anything.
    monarchyTitle_->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(monarchyTitle_);
    versionLabel_ = dimLabel(QStringLiteral("v%1").arg(QStringLiteral(WAVELINE_VERSION)), bar);
    versionLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(versionLabel_);

    lay->addStretch();

    addHeaderPills(bar, lay, Theme::Bg);

    return bar;
}

void MainWindow::buildMonarchyMenuBar() {
    // No menu named after the mixer: the bar's app-menu shows the application's
    // name as its bold leading label already. The rest is the shape every
    // Monarchy application keeps -- File, View, Window, Help -- so the bar
    // reads the same whichever window is focused.
    QMenuBar *bar = menuBar();
    const auto add = [this](QMenu *menu, const QString &text, auto slot,
                            const QKeySequence &keys = {}) {
        QAction *a = menu->addAction(text);
        if (!keys.isEmpty()) a->setShortcut(keys);
        connect(a, &QAction::triggered, this, slot);
        // On the window as well, so the shortcut still works if the bar is not
        // exported and has to be hidden below.
        addAction(a);
        return a;
    };

    QMenu *file = bar->addMenu(tr("File"));
    add(file, tr("Profiles\u2026"), &MainWindow::showProfiles);
    add(file, tr("Settings\u2026"), &MainWindow::showLatencyDiagnostics,
        QKeySequence::Preferences);
    file->addSeparator();
    add(file, tr("Close Window"), &QWidget::close, QKeySequence::Close);
    add(file, tr("Quit Waveline"), [] { QApplication::quit(); }, QKeySequence::Quit);

    QMenu *view = bar->addMenu(tr("View"));
    add(view, tr("Tuner"), &MainWindow::showTuner);
    add(view, tr("Soundboard"), &MainWindow::showSoundboard);
    add(view, tr("Companion"), &MainWindow::showCompanion);

    QMenu *window = bar->addMenu(tr("Window"));
    add(window, tr("Minimise"), &QWidget::showMinimized, QKeySequence(tr("Ctrl+M")));
    add(window, tr("Zoom"), [this] {
        if (isMaximized()) showNormal();
        else showMaximized();
    });

    QMenu *help = bar->addMenu(tr("Help"));
    add(help, tr("About Waveline"), &MainWindow::showAbout);

    // Exported, the bar lives at the top of the screen and draws nothing here.
    // Where nothing is there to take it -- a Monarchy session without its bar,
    // or the offscreen screenshot platform -- it would land inside the window
    // above the header, so it is hidden and only its shortcuts are kept.
    if (!bar->isNativeMenuBar()) bar->hide();
}

void MainWindow::applyMonarchyStyle() {
    if (!monarchyCentral_) return;
    // The one window tint, at the desktop's window opacity; the pills and the
    // header's type are in the application style sheet.
    QColor tint = Theme::Bg;
    tint.setAlpha(Monarchy::Settings::instance().windowAlpha(!Theme::Light));
    setStyleSheet(QStringLiteral("QMainWindow { background: transparent; }"
                                 "QWidget#monarchyCentral { background: rgba(%1,%2,%3,%4); }"
                                 "QWidget#monarchyHeader { background: transparent; }")
                      .arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alpha()));
}

// ================================================================== inputs

QWidget *MainWindow::buildInputs() {
    // The row of cards. It used to sit in a darker recessed well; now the
    // window shows through and each card is set off by its own lighter fill
    // and coloured outline instead.
    auto *well = new CardBase(this);
    // No fill of its own: the window shows through, the same as behind the
    // Monitor mix section, and the lighter cards stand on it.
    well->setFillColor(Qt::transparent);
    // Square: it runs from the window's edge, and a rounded end there would
    // read as the well stopping short of it.
    well->setRadius(0);
    inputsWell_ = well;

    auto *wellLay = new QVBoxLayout(well);
    // Padding belongs to the scrollable content, not outside its viewport.
    // Otherwise the rightmost channel is cut off 10 px before the well edge.
    wellLay->setContentsMargins(0, 0, 0, 0);

    auto *host = new QWidget(well);
    stripHost_ = host;
    loadChannelOrder();
    stripRow_ = new QHBoxLayout(host);
    // The window's inset, inside the scroll: scrolled to either end, the
    // first or last card lines up with the content below it; scrolled
    // between, the cards run on to the window's edge and are cut off by it.
    stripRow_->setContentsMargins(kContentInset, 10, kContentInset, 10);
    // Only groups sit in the row, so its spacing is the gap between groups.
    // A hidden group takes none.
    stripRow_->setSpacing(kGroupSpacing);

    // Shown while there are no cards, so a stopped daemon leaves an
    // explanation rather than a large empty rectangle.
    emptyLabel_ = dimLabel(tr("Waiting for wavelined..."), host);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    stripRow_->addWidget(emptyLabel_, 1);

    QWidget *mixes = buildOutputs();
    mixes->setParent(host);
    inputsHost_ = groupHost(host, &inputsRow_);
    inputsHost_->hide();
    channelsHost_ = groupHost(host, &channelsRow_);
    channelsHost_->hide();
    QWidget *appSettings = buildSoundSharingSection();
    appSettings->setParent(host);
    appSettings->installEventFilter(this);

    // In the order the user left them, any not in it at their usual place.
    QStringList order = QSettings().value(QStringLiteral("rowOrder")).toStringList();
    const QStringList usual = {kUnitMixes, kUnitInputs, kUnitChannels, kUnitApp};
    for (const QString &u : usual)
        if (!order.contains(u)) order.insert(std::min(usual.indexOf(u), order.size()), u);
    for (const QString &u : order) {
        if (QWidget *w = rowUnit(u)) stripRow_->addWidget(w);
    }
    // No stretch at the end: with room to spare the cards take it between
    // them -- see updateRowStretch().

    // Horizontal scrolling rather than shrinking cards: a fader that is 40 px
    // tall is not a fader.
    auto *scroll = new QScrollArea(well);
    stripScroll_ = scroll;
    scroll->setWidget(host);
    scroll->setWidgetResizable(true);
    shuffle_ = new CardShuffle(this);
    shuffle_->setScrollArea(scroll);
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
    // The mixes, at the front of the row: #1, #2, ... and the Stream mix.
    auto *block = new QWidget(this);
    outputsBlock_ = block;
    block->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *row = new QHBoxLayout(block);
    row->setContentsMargins(0, 0, 0, 0);
    // A little apart from the Monitor mixes: the Stream mix is a mix too, but
    // a different kind.
    row->setSpacing(2 * kCardSpacing);
    outputsRow_ = row;

    monitorHost_ = groupHost(block, &monitorCardsRow_);
    row->addWidget(monitorHost_);

    // Lives on the last Monitor mix card -- see syncMonitorOutputUi().
    addMonitorBtn_ = squareOverlayButton(QStringLiteral("plus"),
                                         tr("Add another Monitor mix output (up to 5)."),
                                         block);
    addMonitorBtn_->hide();
    connect(addMonitorBtn_, &QPushButton::clicked, this, &MainWindow::onAddMonitorOutput);

    streamCard_ = new MixCard(MixCard::Kind::Stream, block);
    streamCard_->setDescription(tr("Waveline Stream Mix"));
    connect(streamCard_, &MixCard::identityClicked, this,
            [this] { editCardIdentity(QStringLiteral("stream"), QString()); });
    connect(streamCard_, &MixCard::volumeChanged, this, [this](double v) {
        if (client_->available()) client_->setStreamMixVolume(v);
    });
    connect(streamCard_, &MixCard::muteToggled, this, [this](bool muted) {
        if (client_->available()) client_->setStreamMixMuted(muted);
    });
    // Before the Monitor mixes or after them, wherever it was last moved.
    if (QSettings().value(QStringLiteral("streamMixFirst")).toBool())
        row->insertWidget(0, streamCard_);
    else
        row->addWidget(streamCard_);
    streamCard_->installEventFilter(this);

    syncMonitorOutputUi();
    return block;
}

void MainWindow::showMonitorOutputMenu(int index) {
    if (!client_->available() || index < 0 || index >= monitorOutputRows_.size()) return;
    MixCard *card = monitorOutputRows_[index].card;
    if (!card) return;
    const QList<MonitorOutputInfo> states = client_->monitorOutputStates();
    const QList<OutputInfo> outs = client_->outputs();
    const MonitorOutputInfo state = index < states.size() ? states[index] : MonitorOutputInfo{};
    const QString cur = state.sink;
    // Two Monitor mixes on one device would each load a loopback onto it: they
    // fight over the clock driver and the device plays the sum of both faders.
    // So a device another mix already owns is simply not offered here.
    QSet<QString> taken;
    for (int j = 0; j < states.size(); ++j) {
        if (j == index || states[j].sink.isEmpty()) continue;
        taken.insert(states[j].sink);
    }

    QMenu menu(this);
    auto *group = new QActionGroup(&menu);
    bool found = false;
    for (const OutputInfo &o : outs) {
        if (taken.contains(o.name)) continue;
        QAction *a = menu.addAction(elideEnd(o.description));
        a->setToolTip(o.description);
        a->setCheckable(true);
        a->setChecked(o.name == cur);
        group->addAction(a);
        if (o.name == cur) found = true;
        connect(a, &QAction::triggered, this, [this, index, name = o.name, cur] {
            if (name != cur && client_->available()) client_->setMonitorOutputAt(index, name);
        });
    }
    // The assigned device is unplugged: it stays the choice, shown waiting.
    if (!cur.isEmpty() && !found) {
        QAction *a = menu.addAction(
            elideEnd((state.description.isEmpty() ? cur : state.description) +
                     tr(" (disconnected)")));
        a->setCheckable(true);
        a->setChecked(true);
        a->setEnabled(false);
    }
    menu.addSeparator();
    // The card's tile is where it is clicked, so its look is edited from here.
    menu.addAction(tr("Colour and icon\u2026"), this, [this, index] {
        editCardIdentity(QStringLiteral("mix"), QString::number(index));
    });
    if (index > 0)
        menu.addAction(tr("Remove this Monitor mix"), this,
                       [this, index] { onRemoveMonitorOutput(index); });
    QWidget *handle = card->handle();
    menu.exec(handle->mapToGlobal(QPoint(0, handle->height())));
}

// Nothing left to add once every output device drives a Monitor mix: greyed
// with a tooltip that says so, rather than a "+" that silently does nothing.
void MainWindow::syncAddMonitorButton(const QList<MonitorOutputInfo> &states,
                                      const QList<OutputInfo> &outs) {
    if (!addMonitorBtn_) return;
    const bool room = states.size() < 5;
    const bool free = !freeMonitorOutputDevice(states, outs).isEmpty();
    addMonitorBtn_->setEnabled(client_->available() && room && free);
    addMonitorBtn_->setToolTip(
        !room   ? tr("Five Monitor mixes is the most there can be.")
        : free  ? tr("Add another Monitor mix output (up to 5).")
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

void MainWindow::refreshMonitorOutputDevices() {
    if (!client_->available()) return;
    const QList<MonitorOutputInfo> states = client_->monitorOutputStates();
    const QList<OutputInfo> outs = client_->outputs();
    for (int i = 0; i < monitorOutputRows_.size(); ++i) {
        MixCard *card = monitorOutputRows_[i].card;
        if (!card) continue;
        const MonitorOutputInfo s = i < states.size() ? states[i] : MonitorOutputInfo{};
        QString name = s.description;
        for (const OutputInfo &o : outs)
            if (o.name == s.sink) name = o.description;
        if (name.isEmpty()) name = s.sink.isEmpty() ? tr("No device") : s.sink;
        card->setDescription(name);
        card->setConnected(s.connected || s.sink.isEmpty());
    }
    syncAddMonitorButton(states, outs);
}

void MainWindow::refreshMonitorOutputControls() {
    if (!client_->available()) return;
    const QList<MonitorOutputInfo> states = client_->monitorOutputStates();
    for (int i = 0; i < monitorOutputRows_.size() && i < states.size(); ++i) {
        MixCard *card = monitorOutputRows_[i].card;
        if (!card) continue;
        card->setConnected(states[i].connected || states[i].sink.isEmpty());
        card->setVolume(states[i].volume);
        card->setMuted(states[i].muted);
    }
    if (streamCard_) {
        streamCard_->setVolume(client_->streamMixVolume());
        streamCard_->setMuted(client_->streamMixMuted());
    }
}

void MainWindow::syncMonitorOutputUi() {
    if (!monitorCardsRow_) return;

    const QList<MonitorOutputInfo> states =
        client_ && client_->available() ? client_->monitorOutputStates()
                                          : QList<MonitorOutputInfo>{};
    const int count = qMax(1, states.size());
    // Rebuilt only when the number of mixes changes: a device moving between
    // them is the same cards with new tooltips and levels.
    const QString sig = QString::number(count);
    if (sig == monitorOutputsSig_ && monitorOutputRows_.size() == count) {
        refreshMonitorOutputDevices();
        return;
    }
    monitorOutputsSig_ = sig;

    // The "+" belongs to whichever card is last; out of the old ones first, or
    // it goes with them.
    if (addMonitorBtn_) {
        addMonitorBtn_->setParent(outputsBlock_);
        addMonitorBtn_->hide();
    }
    for (const MonitorOutputRowUi &row : monitorOutputRows_) {
        if (!row.card) continue;
        monitorCardsRow_->removeWidget(row.card);
        row.card->hide();
        row.card->deleteLater();
    }
    monitorOutputRows_.clear();
    monitorDragFrom_ = -1;
    monitorDragging_ = false;

    for (int i = 0; i < count; ++i) {
        MonitorOutputRowUi row;
        row.card = new MixCard(MixCard::Kind::Monitor, outputsBlock_);
        connect(row.card, &MixCard::volumeChanged, this, [this, i](double v) {
            if (client_->available()) client_->setMonitorOutputVolumeAt(i, v);
        });
        connect(row.card, &MixCard::muteToggled, this, [this, i](bool muted) {
            if (client_->available()) client_->setMonitorOutputMutedAt(i, muted);
        });
        row.card->handle()->installEventFilter(this);
        row.card->installEventFilter(this);
        row.card->setEnabled(client_ && client_->available());
        // Every mix but the first can be removed: there is always one.
        if (i > 0) {
            QPushButton *remove = squareOverlayButton(
                QStringLiteral("minus"), tr("Remove this Monitor mix."), row.card);
            connect(remove, &QPushButton::clicked, this,
                    [this, i] { onRemoveMonitorOutput(i); });
            row.card->setRemoveButton(remove);
        }
        monitorOutputRows_.append(row);
    }
    // Laid out in the order the user put them in, which is only the window's:
    // the daemon keeps its own, and the numbers and colours follow that.
    for (int i : displayedMonitorOrder(states, count))
        monitorCardsRow_->addWidget(monitorOutputRows_[i].card);
    seatMonitorAddButton();
    refreshMonitorOutputDevices();
    refreshMonitorOutputControls();
    applyCardLooks();
    updateRowStretch();
}

void MainWindow::updateRowStretch() {
    if (!stripRow_) return;
    // Each card's share of the row is its own width: with room to spare they
    // all widen by the same factor and keep their proportions -- a channel
    // card stays wider than a mix -- and with none, every one is held at that
    // width and the row scrolls. A group's share is the sum of its cards', so
    // the gaps between groups never grow.
    const auto weight = [](const QWidget *w) { return std::max(1, w->minimumWidth()); };
    const auto spread = [&](QBoxLayout *row) {
        int sum = 0;
        if (!row) return sum;
        for (int i = 0; i < row->count(); ++i) {
            QWidget *w = row->itemAt(i)->widget();
            if (!w) continue;
            row->setStretchFactor(w, weight(w));
            sum += weight(w);
        }
        return sum;
    };
    const int monitors = spread(monitorCardsRow_);
    int mixes = monitors;
    if (outputsRow_ && monitorHost_) outputsRow_->setStretchFactor(monitorHost_, std::max(1, monitors));
    if (outputsRow_ && streamCard_) {
        outputsRow_->setStretchFactor(streamCard_, weight(streamCard_));
        mixes += weight(streamCard_);
    }
    const int inputs = spread(inputsRow_);
    const int channels = spread(channelsRow_);
    if (outputsBlock_) stripRow_->setStretchFactor(outputsBlock_, std::max(1, mixes));
    if (inputsHost_) stripRow_->setStretchFactor(inputsHost_, std::max(1, inputs));
    if (channelsHost_) stripRow_->setStretchFactor(channelsHost_, std::max(1, channels));
    if (sidebarCard_) stripRow_->setStretchFactor(sidebarCard_, weight(sidebarCard_));
}

QWidget *MainWindow::rowUnit(const QString &name) const {
    if (name == kUnitMixes) return outputsBlock_;
    if (name == kUnitInputs) return inputsHost_;
    if (name == kUnitChannels) return channelsHost_;
    if (name == kUnitApp) return sidebarCard_;
    return nullptr;
}

void MainWindow::beginUnitMove(QWidget *unit, const QPoint &press) {
    // Every group on show, Application Settings among them. A hidden one --
    // no channels yet -- keeps its place in the row and is passed over.
    QList<QWidget *> group;
    for (int i = 0; i < stripRow_->count(); ++i) {
        QWidget *w = stripRow_->itemAt(i)->widget();
        if (!w || w == emptyLabel_ || w->isHidden()) continue;
        group << w;
    }
    shuffle_->begin(stripRow_, unit, group, press, [this] {
        QStringList order;
        for (int i = 0; i < stripRow_->count(); ++i) {
            QWidget *w = stripRow_->itemAt(i)->widget();
            for (const QString &u : {kUnitMixes, kUnitInputs, kUnitChannels, kUnitApp})
                if (w && w == rowUnit(u)) order << u;
        }
        QSettings().setValue(QStringLiteral("rowOrder"), order);
        updateRowStretch();
    });
}

void MainWindow::syncGroupVisibility() {
    if (inputsHost_) inputsHost_->setVisible(!masterStrips_.isEmpty());
    if (channelsHost_) channelsHost_->setVisible(!strips_.isEmpty());
}

bool MainWindow::handleBodyDrag(QObject *watched, QEvent *event) {
    // The Stream mix and Application Settings, picked up by their bodies.
    QWidget *body = nullptr;
    if (streamCard_ && watched == streamCard_) body = streamCard_;
    else if (sidebarCard_ && watched == sidebarCard_) body = sidebarCard_;
    if (!body) return false;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() != Qt::LeftButton || shuffle_->busy()) return false;
        bodyDrag_ = body;
        bodyDragging_ = false;
        bodyDragStart_ = e->globalPosition().toPoint();
        return true;
    }
    case QEvent::MouseMove: {
        if (bodyDrag_ != body) return false;
        auto *e = static_cast<QMouseEvent *>(event);
        const QPoint pos = e->globalPosition().toPoint();
        if (bodyDragging_) {
            shuffle_->move(pos);
            return true;
        }
        if ((pos - bodyDragStart_).manhattanLength() < QApplication::startDragDistance())
            return true;
        bodyDragging_ = true;
        body->setCursor(Qt::ClosedHandCursor);
        const bool whole = e->modifiers() & Qt::ControlModifier;
        if (body == sidebarCard_ || whole) {
            beginUnitMove(body == sidebarCard_ ? sidebarCard_ : outputsBlock_, bodyDragStart_);
        } else {
            // The Stream mix trades places with the Monitor mixes as a block.
            shuffle_->begin(outputsRow_, streamCard_, {monitorHost_, streamCard_},
                            bodyDragStart_, [this] {
                                QSettings().setValue(
                                    QStringLiteral("streamMixFirst"),
                                    outputsRow_->indexOf(streamCard_) <
                                        outputsRow_->indexOf(monitorHost_));
                                updateRowStretch();
                            });
        }
        shuffle_->move(pos);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (bodyDrag_ != body) return false;
        bodyDrag_ = nullptr;
        if (bodyDragging_) {
            bodyDragging_ = false;
            body->unsetCursor();
            shuffle_->end();
        }
        return true;
    }
    default:
        return false;
    }
}

// ================================================================= sidebar

Section *MainWindow::buildSoundSharingSection() {
    // Applications, their channel and their microphone sharing are one
    // decision per app, so they live in one place rather than two sections
    // that had to be kept in step by eye. A card in the row like the others,
    // just before the mixes.
    auto *s = new Section(tr("Application Settings"), this);
    sidebarCard_ = s;
    // A floor, like every card's: it widens with the rest when there is room.
    s->setMinimumWidth(kSidebarWidth);
    s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    s->setRadius(12);
    s->setFillColor(Theme::CardHover);
    s->setCaptionCentered(true);
    s->setBorderColor(kAppSettingsColor);
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

void MainWindow::showChannelRack(const QString &channelId, const QString &name) {
    // Keyed apart from the inputs' racks: a channel and an input device can
    // share an id ("mic").
    const QString key = QStringLiteral("channel:") + channelId;
    if (!virtualRacks_.contains(key))
        virtualRacks_.insert(key, new VirtualRackWindow(client_, channelId, name, this, true));
    VirtualRackWindow *w = virtualRacks_.value(key);
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
        Theme::setTextColor(text, Theme::Text);
        lay->addWidget(text, 1);

        auto *dismiss = new QPushButton(tr("Don't warn me about %1").arg(c.sinkLabel), row);
        dismiss->setToolTip(tr("Stops this warning for “%1” for good.\n"
                               "Undo it under Settings & Diagnostics ▸ Warnings.")
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

QWidget *MainWindow::openWindowForScreenshot(const QString &which) {
    const QString mic = QStringLiteral("mic");
    if (which == QLatin1String("tuner")) return openTunerWindow();
    if (which == QLatin1String("soundboard")) { showSoundboard(); return soundboardWindow_; }
    if (which == QLatin1String("companion")) { showCompanion(); return companionWindow_; }
    if (which == QLatin1String("settings")) { showLatencyDiagnostics(); return settingsWindow_; }
    if (which == QLatin1String("profiles")) { showProfiles(); return profilesWindow_; }
    if (which == QLatin1String("about")) { showAbout(); return aboutWindow_; }
    if (which == QLatin1String("effects")) { showGlobalEffects(mic); return globalEffects_.value(mic); }
    if (which == QLatin1String("rack")) {
        showVirtualRack(mic, tr("Microphone"));
        return virtualRacks_.value(mic);
    }
    if (which == QLatin1String("channel-effects")) {
        const QString voice = QStringLiteral("voice");
        showChannelEffects(voice, tr("Voice"));
        return channelEffects_.value(voice);
    }
    return nullptr;
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
    const QList<QWidget *> gated = {clipguard_, hwMonitor_, manageProfiles_};
    for (QWidget *w : gated)
        if (w) w->setEnabled(available);
    // With no daemon there is nothing to route or mix: the cards go, and the
    // row is left to its "Waiting for wavelined" line.
    if (outputsBlock_) outputsBlock_->setVisible(available);
    if (sidebarCard_) sidebarCard_->setVisible(available);
    for (const MonitorOutputRowUi &row : monitorOutputRows_)
        if (row.card) row.card->setEnabled(available);
    if (streamCard_) streamCard_->setEnabled(available);
    for (auto *s : strips_) s->setEnabled(available);
    for (auto *s : masterStrips_) s->setEnabled(available);
    if (addMasterBtn_) addMasterBtn_->setEnabled(available);
    // Coming back, the strips are rebuilt and syncAddMasterButton() decides.
    if (removeMasterBtn_ && !available) removeMasterBtn_->setEnabled(false);


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

void MainWindow::onRemoveMasterBusMenu() {
    if (!client_->available() || !removeMasterBtn_) return;
    QList<QPair<QString, QString>> removable;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == QLatin1String("mic")) continue;
        removable.append({b.id, b.name.isEmpty() ? client_->deviceBrand() : b.name});
    }
    if (removable.isEmpty()) return;
    // One to choose from is no choice: straight to the confirmation.
    if (removable.size() == 1) {
        onRemoveMasterBus(removable.first().first);
        return;
    }
    QMenu menu(this);
    for (const auto &[id, name] : removable)
        menu.addAction(name, this, [this, id] { onRemoveMasterBus(id); });
    menu.exec(removeMasterBtn_->mapToGlobal(QPoint(0, removeMasterBtn_->height())));
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
    if (!masterDisplayOrder_.isEmpty())
        last = masterStrips_.value(masterDisplayOrder_.last());

    if (!addMasterBtn_) {
        addMasterBtn_ = squareOverlayButton(QStringLiteral("plus"),
                                            tr("Add another input device."),
                                            this);
        connect(addMasterBtn_, &QPushButton::clicked, this, &MainWindow::onAddMasterBusMenu);
    }
    if (!removeMasterBtn_) {
        removeMasterBtn_ = squareOverlayButton(QStringLiteral("minus"),
                                               tr("Remove an input device."),
                                               this);
        connect(removeMasterBtn_, &QPushButton::clicked, this,
                &MainWindow::onRemoveMasterBusMenu);
    }

    for (auto *s : masterStrips_) {
        s->setAddMasterOverlayButton(s == last ? addMasterBtn_ : nullptr);
        s->setRemoveMasterOverlayButton(s == last ? removeMasterBtn_ : nullptr);
    }

    // The primary input cannot be removed, so with only it there is nothing
    // for the "-" to do.
    bool anyRemovable = false;
    for (const MasterBusInfo &b : client_->masterBuses())
        anyRemovable = anyRemovable || b.id != QLatin1String("mic");
    if (last) {
        addMasterBtn_->setEnabled(client_->available());
        removeMasterBtn_->setEnabled(client_->available() && anyRemovable);
    } else {
        addMasterBtn_->hide();
        removeMasterBtn_->hide();
    }
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
            syncGroupVisibility();
            return;
        }
    }
    masterStripOrder_ = order;

    if (addMasterBtn_) addMasterBtn_->setParent(this);
    // The "-" rides on the same card, and would be deleted with it.
    if (removeMasterBtn_) removeMasterBtn_->setParent(this);

    for (auto *s : masterStrips_) {
        inputsRow_->removeWidget(s);
        s->deleteLater();
    }
    masterStrips_.clear();
    // Keyed by bus id, and the cards holding those ids are gone; the next
    // refreshMasterStrip() fills it back in for whatever replaces them.
    masterMonitor_.clear();

    // Laid out in the order the user put them in; numbered and coloured in
    // the daemon's, which is the device's own.
    masterDisplayOrder_ = displayedMasterOrder(order);
    for (const QString &id : masterDisplayOrder_) {
        const int slot = order.indexOf(id);
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
        strip->setMasterSlotNumber(slot + 1);
        wireStripReorder(strip, true);
        strip->enableEditableTitle();
        connect(strip, &ChannelStrip::displayNameEdited, this,
                [this, id](const QString &name) { client_->setMasterName(id, name); });
        if (info.busType != QLatin1String("midi")) {
            strip->enableMasterRebuild(true);
            connect(strip, &ChannelStrip::masterRebuildRequested, this,
                    [this, id] { onRebuildMasterCapture(id); });
            // The rack glyph where the trash used to be: removing an input is
            // the "-" under the "+" now.
            strip->enableMasterRack(true);
            connect(strip, &ChannelStrip::rackRequested, this,
                    [this, id, name] { showVirtualRack(id, name); });
        }
        if (primary) {
            strip->setToolTip(tr("Your primary input.\n"
                                 "Stream is how loudly your audience hears you."));
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
        strip->setMonitorColumnTip(
            tr("How loudly you hear this microphone in the Monitor mix.\n"
               "Use the ear toggle to enable monitoring. To change everything "
               "you hear on a device, use that device's Monitor mix card."),
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
                              "Right-click cycles the states in reverse."));
            connectMasterEffectsButton(fx, client_, id, strip);
        }

        connect(strip, &ChannelStrip::baseWidthChanged, this, &MainWindow::updateRowStretch);
        inputsRow_->addWidget(strip);
        masterStrips_.insert(id, strip);
        profileApplied_ = false;
    }

    syncAddMasterButton();
    syncGroupVisibility();
    updateRowStretch();
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
    for (int i = 0; i < monitorOutputRows_.size(); ++i) {
        if (MixCard *card = monitorOutputRows_[i].card) {
            const QString key = monitorMixCardKey(i);
            card->setIdentity(Theme::cardIcon(key, QStringLiteral("headphones")),
                              Theme::cardColor(key, monitorMixColor(i)));
        }
    }
    if (streamCard_)
        streamCard_->setIdentity(QStringLiteral("stream"),
                                 Theme::cardColor(kStreamMixCardKey, kStreamMixColor));
    if (sidebarCard_) {
        const QColor color = Theme::cardColor(kAppSettingsCardKey, kAppSettingsColor);
        sidebarCard_->setBorderColor(color);
    }
}

void MainWindow::editCardIdentity(const QString &kind, const QString &id) {
    if (!client_->available()) return;

    // The mixes and Application Settings: no name to edit, and for the Stream
    // mix and Application Settings no glyph either -- the stream sign and the
    // gear are what they are. Only the look is stored.
    if (kind == QLatin1String("mix") || kind == QLatin1String("stream")
        || kind == QLatin1String("app-settings")) {
        QString key;
        QString title;
        QColor defColor;
        QString defIcon;
        bool iconEditable = false;
        if (kind == QLatin1String("mix")) {
            const int index = id.toInt();
            key = monitorMixCardKey(index);
            title = tr("Monitor mix #%1").arg(index + 1);
            defColor = monitorMixColor(index);
            defIcon = QStringLiteral("headphones");
            iconEditable = true;
        } else if (kind == QLatin1String("stream")) {
            key = kStreamMixCardKey;
            title = tr("Stream mix");
            defColor = kStreamMixColor;
            defIcon = QStringLiteral("stream");
        } else {
            key = kAppSettingsCardKey;
            title = tr("Application Settings");
            defColor = kAppSettingsColor;
            defIcon = QStringLiteral("gear");
        }
        const MixerClient::CardAppearance saved = client_->cardAppearances().value(key);
        CardIdentityDialog dlg(tr("Card — %1").arg(title), title, QColor(saved.color),
                               iconEditable ? saved.icon : QString(), defColor, defIcon,
                               this);
        dlg.setNameEditable(false);
        dlg.setIconEditable(iconEditable);
        if (dlg.exec() != QDialog::Accepted) return;
        const QColor color = dlg.color();
        const QString icon = iconEditable ? dlg.icon() : QString();
        client_->setCardAppearance(
            key, color.isValid() ? color.name(QColor::HexRgb) : QString(), icon);
        Theme::CardLook look;
        look.color = color;
        look.icon = icon;
        Theme::setCardLook(key, look);
        applyCardLooks();
        client_->refresh();
        return;
    }

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
    const bool live = m.monitoring && !m.muted && !monitorCutForSpeakers_;
    strip->setMainMeterActive(live);
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
    if (!channelsRow_) return;
    // Read back from the row rather than from a model of it: the live drag
    // moves the widgets, so the widgets are the truth by the time this runs.
    QStringList order;
    for (int i = 0; i < channelsRow_->count(); ++i) {
        QWidget *w = channelsRow_->itemAt(i)->widget();
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

void MainWindow::wireStripReorder(ChannelStrip *strip, bool master) {
    strip->enableDragHandle();
    connect(strip, &ChannelStrip::reorderStarted, this,
            [this, strip, master](const QPoint &press, bool wholeGroup) {
                // With Control held, the whole group goes.
                if (wholeGroup) {
                    beginUnitMove(master ? inputsHost_ : channelsHost_, press);
                    return;
                }
                // Only cards of its own kind.
                QBoxLayout *row = master ? inputsRow_ : channelsRow_;
                QList<QWidget *> group;
                for (int i = 0; i < row->count(); ++i)
                    if (auto *s = qobject_cast<ChannelStrip *>(row->itemAt(i)->widget()))
                        group << s;
                shuffle_->begin(row, strip, group, press, [this, master] {
                    if (master) saveMasterOrderFromRow();
                    else saveChannelOrderFromRow();
                    // Back in the layout, the card needs its share of the
                    // spare room again.
                    updateRowStretch();
                });
            });
    connect(strip, &ChannelStrip::reorderMoved, this,
            [this](const QPoint &pos) { shuffle_->move(pos); });
    connect(strip, &ChannelStrip::reorderEnded, this, [this] { shuffle_->end(); });
}

QStringList MainWindow::displayedMasterOrder(const QStringList &daemonOrder) const {
    // Remembered ones first, in the remembered order; anything never moved
    // after them, as the daemon has it.
    const QStringList saved = QSettings().value(QStringLiteral("inputOrder")).toStringList();
    QStringList out;
    for (const QString &id : saved)
        if (daemonOrder.contains(id)) out << id;
    for (const QString &id : daemonOrder)
        if (!out.contains(id)) out << id;
    return out;
}

void MainWindow::saveMasterOrderFromRow() {
    QStringList order;
    for (int i = 0; i < inputsRow_->count(); ++i) {
        auto *s = qobject_cast<ChannelStrip *>(inputsRow_->itemAt(i)->widget());
        if (s && masterStrips_.value(s->channelId()) == s) order << s->channelId();
    }
    masterDisplayOrder_ = order;
    // Kept whole, removed devices included, so one plugged back in returns to
    // where it was.
    QStringList saved = QSettings().value(QStringLiteral("inputOrder")).toStringList();
    for (const QString &id : saved)
        if (!order.contains(id)) order << id;
    QSettings().setValue(QStringLiteral("inputOrder"), order);
    // The "+" and "-" ride on whichever card is last now.
    syncAddMasterButton();
}

namespace {
// A Monitor mix is remembered by the device it plays on: its index moves when
// one before it is removed, and no two mixes share a device.
QString monitorOrderKey(const QList<MonitorOutputInfo> &states, int i) {
    if (i < states.size() && !states[i].sink.isEmpty()) return states[i].sink;
    return QStringLiteral("#%1").arg(i);
}
}  // namespace

QList<int> MainWindow::displayedMonitorOrder(const QList<MonitorOutputInfo> &states,
                                             int count) const {
    const QStringList saved =
        QSettings().value(QStringLiteral("monitorOrder")).toStringList();
    QList<int> out;
    for (const QString &key : saved)
        for (int i = 0; i < count; ++i)
            if (!out.contains(i) && monitorOrderKey(states, i) == key) out << i;
    for (int i = 0; i < count; ++i)
        if (!out.contains(i)) out << i;
    return out;
}

void MainWindow::seatMonitorAddButton() {
    if (!addMonitorBtn_ || !monitorCardsRow_) return;
    // On whichever card is last in the row, not whichever is last by number.
    MixCard *last = nullptr;
    for (int i = 0; i < monitorCardsRow_->count(); ++i)
        if (auto *c = qobject_cast<MixCard *>(monitorCardsRow_->itemAt(i)->widget())) last = c;
    for (const MonitorOutputRowUi &row : monitorOutputRows_)
        if (row.card && row.card != last) row.card->setAddButton(nullptr);
    if (last) last->setAddButton(addMonitorBtn_);
}

void MainWindow::commitMonitorOrderFromRow() {
    // Only the window's order: nothing goes to the daemon, so nothing about
    // the mixes themselves changes.
    const QList<MonitorOutputInfo> states =
        client_->available() ? client_->monitorOutputStates() : QList<MonitorOutputInfo>{};
    QStringList order;
    for (int i = 0; i < monitorCardsRow_->count(); ++i) {
        QWidget *w = monitorCardsRow_->itemAt(i)->widget();
        for (int r = 0; r < monitorOutputRows_.size(); ++r)
            if (monitorOutputRows_[r].card == w) order << monitorOrderKey(states, r);
    }
    // Kept whole, devices not in a mix right now included, so one that comes
    // back returns to where it was.
    for (const QString &key : QSettings().value(QStringLiteral("monitorOrder")).toStringList())
        if (!order.contains(key) && !key.startsWith(QLatin1Char('#'))) order << key;
    QSettings().setValue(QStringLiteral("monitorOrder"), order);
    seatMonitorAddButton();
    updateRowStretch();
}

bool MainWindow::handleMonitorOutputDrag(QObject *watched, QEvent *event) {
    int row = -1;
    bool onBadge = false;
    for (int i = 0; i < monitorOutputRows_.size(); ++i) {
        MixCard *card = monitorOutputRows_[i].card;
        if (!card) continue;
        if (watched == card->handle()) {
            row = i;
            onBadge = true;
            break;
        }
        if (watched == card) {
            row = i;
            break;
        }
    }
    if (row < 0) return false;
    MixCard *card = monitorOutputRows_[row].card;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() != Qt::LeftButton || shuffle_->busy()) return false;
        monitorDragFrom_ = row;
        monitorDragging_ = false;
        monitorDragStart_ = e->globalPosition().toPoint();
        return true;
    }
    case QEvent::MouseMove: {
        if (monitorDragFrom_ != row) return false;
        auto *e = static_cast<QMouseEvent *>(event);
        const QPoint pos = e->globalPosition().toPoint();
        if (monitorDragging_) {
            shuffle_->move(pos);
            return true;
        }
        if ((pos - monitorDragStart_).manhattanLength() < QApplication::startDragDistance())
            return true;
        // One mix has nothing to trade places with -- unless the whole group
        // is going.
        if (monitorOutputRows_.size() < 2 && !(e->modifiers() & Qt::ControlModifier)) {
            monitorDragFrom_ = -1;
            return true;
        }
        monitorDragging_ = true;
        card->setCursor(Qt::ClosedHandCursor);
        if (e->modifiers() & Qt::ControlModifier) {
            // With Control held, every mix goes, the Stream mix with them.
            beginUnitMove(outputsBlock_, monitorDragStart_);
        } else {
            QList<QWidget *> group;
            for (int i = 0; i < monitorCardsRow_->count(); ++i)
                if (QWidget *w = monitorCardsRow_->itemAt(i)->widget()) group << w;
            shuffle_->begin(monitorCardsRow_, card, group, monitorDragStart_,
                            [this] { commitMonitorOrderFromRow(); });
        }
        shuffle_->move(pos);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (monitorDragFrom_ != row) return false;
        monitorDragFrom_ = -1;
        if (monitorDragging_) {
            monitorDragging_ = false;
            card->unsetCursor();
            shuffle_->end();
            return true;
        }
        // Pressed and let go without moving far enough: a click, which on
        // the badge opens the device menu. After this event, not inside it:
        // the menu runs its own loop, and a Changed landing there can rebuild
        // the cards -- this badge among them -- while its own event is still
        // being delivered.
        if (onBadge)
            QTimer::singleShot(0, this, [this, row] { showMonitorOutputMenu(row); });
        return true;
    }
    default:
        return false;
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // Monarchy's look: the margin round the window resizes it, and the header
    // row stands in for the titlebar it does not have.
    if (monarchyCentral_ && watched == monarchyCentral_
        && Monarchy::handleResizeStrip(monarchyCentral_, windowHandle(), event))
        return true;
    if (monarchyHeader_ && watched == monarchyHeader_
        && Monarchy::handleTitlebarEvent(this, event))
        return true;
    if (handleMonitorOutputDrag(watched, event))
        return true;
    if (handleBodyDrag(watched, event))
        return true;
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::rebuildStrips() {
    rebuildMasterStrips();

    const auto channels = orderedChannels();
    if (strips_.size() == channels.size()) return;

    for (auto *s : strips_) { channelsRow_->removeWidget(s); s->deleteLater(); }
    strips_.clear();
    for (const auto &c : channels) {
        auto *strip = new ChannelStrip(c.id, c.name, nullptr);
        // Same identity affordances as an input device: the tile opens the
        // card's panel, and the title is editable in place.
        connect(strip, &ChannelStrip::identityClicked, this, [this, id = c.id] {
            editCardIdentity(QStringLiteral("channel"), id);
        });
        // Its Creative FX rack, opposite the gear.
        strip->enableChannelRack();
        connect(strip, &ChannelStrip::rackRequested, this,
                [this, id = c.id, name = c.name] { showChannelRack(id, name); });
        wireStripReorder(strip, false);
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
        connect(strip, &ChannelStrip::baseWidthChanged, this, &MainWindow::updateRowStretch);
        channelsRow_->addWidget(strip);
        strips_.insert(c.id, strip);
    }
    syncGroupVisibility();
    updateRowStretch();
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
        // Each mix fader's track: this device's own send into that mix, which
        // is `src` at the card's fader. The card scales it itself, and holds
        // the Monitor track at zero while the ear toggle is off -- see
        // applyMasterMonitorState().
        it.value()->setLevel(src);
    }

    // Each application's level, for Application Settings' slider tracks.
    if (soundSharingTab_ && soundSharingTab_->isVisible()) {
        QHash<uint, double> apps;
        for (auto it = l.begin(); it != l.end(); ++it) {
            if (!it.key().startsWith(QLatin1String("app-"))) continue;
            bool ok = false;
            const uint id = it.key().mid(4).toUInt(&ok);
            if (ok) apps.insert(id, meterPosition(it.value()));
        }
        soundSharingTab_->setAppLevels(apps);
    }

    // monitor-mix is upstream of each output's fader and mute; the card
    // scales it, so its bar is what leaves for that device.
    for (const MonitorOutputRowUi &row : monitorOutputRows_)
        if (row.card) row.card->setLevel(monitorMixLevel);
    // Stream volume is channelVolumes on the sink, so the probe already hears
    // post-fader audio. Monitor meters do meterPosition(pre)*fader instead.
    // Applying the log curve to a post-fader peak (or also multiplying again)
    // will not match — undo the fader, then use the same formula as Monitor.
    if (streamCard_) {
        const double fader = streamCard_->muted() ? 0.0 : streamCard_->volume();
        if (fader <= 0.0) {
            streamCard_->setLevel(0.0);
        } else {
            const double post = l.value(QStringLiteral("stream-mix"), 0.0);
            streamCard_->setLevel(meterPosition(std::min(1.0, post / fader)));
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

    if (streamCard_)
        streamCard_->setDescription(
            tr("%1 Stream Mix: a virtual device, not a speaker. Point your "
               "recorder or streaming software at it.").arg(brand));

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
    monitorCutForSpeakers_ = client_->monitorCancelledBySpeakers();
    for (const MasterBusInfo &b : buses) {
        if (auto *s = masterStrips_.value(b.id)) {
            refreshMasterStrip(b.id, s);
            s->setMicMonitorBlocked(monitorCutForSpeakers_);
            applyMasterMonitorState(b.id);
        }
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
            s->setMicColumnShown(hasMic);
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
