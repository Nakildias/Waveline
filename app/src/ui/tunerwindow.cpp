// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "tunerwindow.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "levelmeter.h"
#include "mixerclient.h"
#include "theme.h"
#include "widgets.h"

namespace {

// How often a reading is fetched. The daemon re-analyses every 30 ms, so
// anything faster is the same number twice; the needle's own interpolation is
// what makes the movement smooth between them.
constexpr int kPollMs = 33;

// Within this the string counts as tuned. Tighter than this is not meaningful
// on a plucked string: the pitch of a struck note drifts by more than a cent
// or two while it decays, and a tuner that will not go green is a tuner people
// stop believing.
constexpr double kInTuneCents = 5.0;
// Once it has been in tune, say so for this long even as the note decays away.
constexpr int kInTuneHoldMs = 900;
// A pluck falls below the detector's floor well before you have finished
// looking at the needle. Hold the last reading rather than blanking.
constexpr int kSilenceHoldMs = 1200;

// Past this the needle is pegged. The number under it still reads true.
constexpr double kDialRangeCents = 50.0;

// Auto mode will not adopt a string further away than this. Beyond it you are
// not tuning that string, you are playing a different note, and the honest
// answer is to name what it heard.
constexpr double kAutoRangeSemitones = 3.0;

// Scaling a font means scaling whichever of the two sizes is actually set.
// A QFont that came from a platform theme specifying a pixel size reports
// pointSizeF() as -1, and multiplying that gives a negative size Qt discards
// in silence -- the text simply comes out at the default size and nothing says
// why.
QFont scaledFont(const QFont &base, double factor, bool bold = false) {
    QFont f = base;
    f.setBold(bold);
    if (base.pointSizeF() > 0.0)
        f.setPointSizeF(base.pointSizeF() * factor);
    else if (base.pixelSize() > 0)
        f.setPixelSize(qMax(1, qRound(base.pixelSize() * factor)));
    return f;
}

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    return l;
}

QColor centsColor(double cents, bool inTune) {
    if (inTune) return Theme::Accent;
    const double a = std::fabs(cents);
    if (a <= kInTuneCents) return Theme::Accent;
    if (a <= 20.0) return Theme::Warn;
    return Theme::Danger;
}

// The needle's angle, in degrees, measured the way QPainter does: 0 at three
// o'clock, counter-clockwise positive. Flat -50 to +50 across a 140 degree
// sweep, which is wide enough to read a couple of cents and narrow enough that
// the whole arc fits over the note.
double centsAngle(double cents) {
    const double clamped = std::clamp(cents, -kDialRangeCents, kDialRangeCents);
    return 90.0 - (clamped / kDialRangeCents) * 70.0;
}

QString signedCents(double cents) {
    const int rounded = static_cast<int>(std::lround(cents));
    return rounded > 0 ? QStringLiteral("+%1").arg(rounded) : QString::number(rounded);
}

// The reference pitches worth offering. 440 is the default; 415 is baroque
// pitch, 432 has its adherents, and continental orchestras routinely sit at
// 442 or 443.
const int kReferences[] = {415, 432, 435, 438, 439, 440, 441, 442, 443, 444};

}  // namespace

// ============================================================== TunerDial

TunerDial::TunerDial(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(230);
    anim_ = new QTimer(this);
    anim_->setInterval(16);
    connect(anim_, &QTimer::timeout, this, &TunerDial::animate);
}

QSize TunerDial::sizeHint() const { return {440, 250}; }
QSize TunerDial::minimumSizeHint() const { return {300, 230}; }

void TunerDial::setIdle(const QString &hint) {
    if (idle_ && hint_ == hint) return;
    idle_ = true;
    inTune_ = false;
    hint_ = hint;
    note_.clear();
    detail_.clear();
    cents_ = 0.0;
    if (!anim_->isActive()) anim_->start();
    update();
}

void TunerDial::setReading(const QString &note, const QString &detail, double cents,
                           bool inTune) {
    idle_ = false;
    note_ = note;
    detail_ = detail;
    inTune_ = inTune;
    cents_ = cents;
    if (!anim_->isActive()) anim_->start();
    update();
}

void TunerDial::animate() {
    const double target = std::clamp(cents_, -kDialRangeCents, kDialRangeCents);
    const double delta = target - drawn_;
    if (std::fabs(delta) < 0.05) {
        drawn_ = target;
        anim_->stop();
        update();
        return;
    }
    drawn_ += delta * 0.28;
    update();
}

void TunerDial::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const double cx = width() / 2.0;
    const double cy = height() * 0.88;
    const double radius = std::min(width() / 2.0 - 24.0, height() * 0.74);
    if (radius <= 20.0) return;

    const QRectF arcRect(cx - radius, cy - radius, radius * 2.0, radius * 2.0);
    const double arcWidth = 9.0;

    // The track.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::Line, arcWidth, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(arcRect, static_cast<int>(centsAngle(kDialRangeCents) * 16),
              static_cast<int>((centsAngle(-kDialRangeCents) -
                                centsAngle(kDialRangeCents)) * 16));

    // The zone that counts as in tune, always visible so the target is not a
    // single invisible point the needle has to be balanced on.
    QColor zone = idle_ ? Theme::AccentDim : Theme::Accent;
    zone.setAlpha(idle_ ? 90 : 150);
    p.setPen(QPen(zone, arcWidth, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(arcRect, static_cast<int>(centsAngle(kInTuneCents) * 16),
              static_cast<int>((centsAngle(-kInTuneCents) -
                                centsAngle(kInTuneCents)) * 16));

    // Ticks every 10 cents, longer on the multiples of 25.
    for (int c = -50; c <= 50; c += 10) {
        const bool major = (c % 25) == 0;
        const double rad = centsAngle(c) * M_PI / 180.0;
        const double inner = radius - arcWidth / 2.0 - (major ? 12.0 : 7.0);
        const double outer = radius - arcWidth / 2.0 - 3.0;
        p.setPen(QPen(major ? Theme::TextDim : Theme::TextFaint, major ? 2.0 : 1.0));
        p.drawLine(QPointF(cx + std::cos(rad) * inner, cy - std::sin(rad) * inner),
                   QPointF(cx + std::cos(rad) * outer, cy - std::sin(rad) * outer));
    }

    // End labels, so the scale is not a mystery.
    const QFont small = scaledFont(font(), 0.85);
    p.setFont(small);
    p.setPen(Theme::TextFaint);
    const double labelR = radius - arcWidth / 2.0 - 30.0;
    const auto drawScaleLabel = [&](int cents, const QString &text) {
        const double rad = centsAngle(cents) * M_PI / 180.0;
        const QPointF at(cx + std::cos(rad) * labelR, cy - std::sin(rad) * labelR);
        p.drawText(QRectF(at.x() - 24, at.y() - 9, 48, 18),
                   Qt::AlignCenter, text);
    };
    drawScaleLabel(-50, QStringLiteral("-50"));
    drawScaleLabel(0, QStringLiteral("0"));
    drawScaleLabel(50, QStringLiteral("+50"));

    // The needle.
    const QColor needle = idle_ ? Theme::TextFaint : centsColor(cents_, inTune_);
    const double rad = centsAngle(drawn_) * M_PI / 180.0;
    const double tip = radius - arcWidth / 2.0 - 4.0;
    p.setPen(QPen(needle, 3.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx, cy), QPointF(cx + std::cos(rad) * tip, cy - std::sin(rad) * tip));
    p.setPen(Qt::NoPen);
    p.setBrush(needle);
    p.drawEllipse(QPointF(cx, cy), 6.5, 6.5);

    // The note, in the space the arc encloses.
    if (idle_) {
        p.setPen(Theme::TextDim);
        p.setFont(font());
        p.drawText(QRectF(0, cy - radius * 0.62, width(), 40), Qt::AlignCenter, hint_);
        return;
    }

    // The note and its offset, painted over the needle rather than beside it.
    // The needle sweeps through the whole interior of the arc, so there is no
    // clear space to put them in -- and the note is the thing being read, so
    // it is the thing that wins.
    //
    // Stacked upwards from just above the hub, measured off the fonts rather
    // than off the radius: these are three lines of text and they have to
    // clear each other at any window size, not at one.
    const QFont noteFont = scaledFont(font(), 3.0, true);
    const QFont centsFont = scaledFont(font(), 1.45, true);
    double bottom = cy - 15.0;

    if (!detail_.isEmpty()) {
        const double h = QFontMetricsF(small).height();
        p.setFont(small);
        p.setPen(Theme::TextFaint);
        p.drawText(QRectF(0, bottom - h, width(), h), Qt::AlignCenter, detail_);
        bottom -= h + 2.0;
    }

    const double centsH = QFontMetricsF(centsFont).height();
    p.setFont(centsFont);
    p.setPen(centsColor(cents_, inTune_));
    p.drawText(QRectF(0, bottom - centsH, width(), centsH), Qt::AlignCenter,
               inTune_ ? tr("in tune") : signedCents(cents_));
    bottom -= centsH + 2.0;

    const double noteH = QFontMetricsF(noteFont).height();
    p.setFont(noteFont);
    p.setPen(inTune_ ? Theme::Accent : Theme::Text);
    p.drawText(QRectF(0, bottom - noteH, width(), noteH), Qt::AlignCenter, note_);
}

// ============================================================ StringButton

StringButton::StringButton(int midiNote, QWidget *parent)
    : QAbstractButton(parent), midiNote_(midiNote) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(tr("Tune to %1").arg(noteName(midiNote)));
}

QSize StringButton::sizeHint() const { return {54, 54}; }

void StringButton::setInTune(bool on) {
    if (inTune_ == on) return;
    inTune_ = on;
    update();
}

void StringButton::enterEvent(QEnterEvent *) { hover_ = true; update(); }
void StringButton::leaveEvent(QEvent *) { hover_ = false; update(); }

void StringButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QColor accent = inTune_ ? Theme::Accent : Theme::Fader;

    QColor bg = Theme::Well;
    QColor border = Theme::Line;
    QColor fg = Theme::TextDim;
    if (isChecked()) {
        bg = accent;
        bg.setAlpha(inTune_ ? 60 : 34);
        border = accent;
        fg = inTune_ ? Theme::Accent : Theme::Text;
    } else if (hover_) {
        bg = Theme::CardHover;
        fg = Theme::Text;
    }

    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, 10, 10);
    p.setPen(QPen(border, isChecked() ? 1.6 : 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 10, 10);
    if (hasFocus()) {
        p.setPen(QPen(Theme::Accent, 1));
        p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 8, 8);
    }

    p.setFont(scaledFont(font(), 1.35, true));
    p.setPen(fg);
    p.drawText(QRectF(r.x(), r.y() + 6, r.width(), r.height() * 0.55),
               Qt::AlignHCenter | Qt::AlignBottom, noteLetter(midiNote_));

    p.setFont(scaledFont(font(), 0.8));
    p.setPen(Theme::TextFaint);
    p.drawText(QRectF(r.x(), r.y() + r.height() * 0.60, r.width(), r.height() * 0.34),
               Qt::AlignHCenter | Qt::AlignTop,
               QString::number(midiNote_ / 12 - 1));
}

// ============================================================= TunerWindow

TunerWindow::TunerWindow(MixerClient *client, QWidget *parent)
    : QWidget(parent, Qt::Window), client_(client) {
    setWindowTitle(tr("Tuner"));
    resize(560, 640);
    setMinimumSize(460, 600);

    loadSettings();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);

    auto *card = new CardBase(this);
    card->setFillColor(Theme::Card);
    card->setRadius(14);
    card->setTopStripe(Theme::Accent);
    outer->addWidget(card);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 18, 20, 18);
    lay->setSpacing(12);

    lay->addWidget(buildControls());

    auto *line = new QFrame(card);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);
    line->setStyleSheet(
        QStringLiteral("background: %1; border: none;").arg(Theme::Line.name()));
    lay->addWidget(line);

    dial_ = new TunerDial(card);
    lay->addWidget(dial_, 1);

    auto *readout = new QHBoxLayout;
    readout->setContentsMargins(4, 0, 4, 0);
    detectedLabel_ = dimLabel(QString(), card);
    targetLabel_ = dimLabel(QString(), card);
    targetLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    readout->addWidget(detectedLabel_);
    readout->addStretch();
    readout->addWidget(targetLabel_);
    lay->addLayout(readout);

    lay->addWidget(buildStrings());

    poll_ = new QTimer(this);
    poll_->setInterval(kPollMs);
    connect(poll_, &QTimer::timeout, this, &TunerWindow::onTick);

    // The list of things to listen to is only as current as the daemon's node
    // list, and that changes when anything is plugged in.
    connect(client_, &MixerClient::changed, this, &TunerWindow::refreshSources);
    // A daemon that went away took the tuner with it, so the next one has to
    // be told to start listening again even though nothing here changed.
    connect(client_, &MixerClient::availabilityChanged, this, [this] {
        startedKind_.clear();
        startedSource_.clear();
        sourceSignatureValid_ = false;
        refreshSources();
    });

    rebuildTunings();
    rebuildStringButtons();
    applyMode(automatic_);
}

QWidget *TunerWindow::buildControls() {
    auto *host = new QWidget(this);
    auto *grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(9);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);

    int row = 0;

    grid->addWidget(dimLabel(tr("Listen to"), host), row, 0);
    sourceBox_ = new QComboBox(host);
    sourceBox_->setToolTip(
        tr("A microphone or line input to listen to, or a MIDI instrument to "
           "read note messages from directly."));
    connect(sourceBox_, &QComboBox::activated, this, &TunerWindow::onSourcePicked);
    grid->addWidget(sourceBox_, row, 1, 1, 2);

    inputMeter_ = new LevelMeter(Qt::Horizontal, host);
    inputMeter_->setThickness(8);
    inputMeter_->setFixedWidth(90);
    inputMeter_->setToolTip(tr("Signal arriving at the tuner"));
    grid->addWidget(inputMeter_, row, 3);
    ++row;

    grid->addWidget(dimLabel(tr("Instrument"), host), row, 0);
    instrumentBox_ = new QComboBox(host);
    for (const InstrumentPreset &i : instrumentPresets())
        instrumentBox_->addItem(i.name, i.id);
    const int wantI = instrumentBox_->findData(wantInstrument_);
    instrumentBox_->setCurrentIndex(wantI >= 0 ? wantI : 0);
    connect(instrumentBox_, &QComboBox::activated, this,
            &TunerWindow::onInstrumentPicked);
    grid->addWidget(instrumentBox_, row, 1);

    grid->addWidget(dimLabel(tr("Tuning"), host), row, 2, Qt::AlignRight);
    tuningBox_ = new QComboBox(host);
    connect(tuningBox_, &QComboBox::activated, this, &TunerWindow::onTuningPicked);
    grid->addWidget(tuningBox_, row, 3);
    ++row;

    grid->addWidget(dimLabel(tr("Mode"), host), row, 0);

    auto *modes = new QWidget(host);
    auto *modeLay = new QHBoxLayout(modes);
    modeLay->setContentsMargins(0, 0, 0, 0);
    modeLay->setSpacing(6);
    autoBtn_ = new QPushButton(tr("Auto"), modes);
    autoBtn_->setObjectName(QStringLiteral("segment"));
    autoBtn_->setCheckable(true);
    autoBtn_->setToolTip(tr("Work out which string is being played."));
    manualBtn_ = new QPushButton(tr("Manual"), modes);
    manualBtn_->setObjectName(QStringLiteral("segment"));
    manualBtn_->setCheckable(true);
    manualBtn_->setToolTip(
        tr("Tune to the string picked below, whatever is played."));
    connect(autoBtn_, &QPushButton::clicked, this, [this] {
        applyMode(true);
        saveSettings();
    });
    connect(manualBtn_, &QPushButton::clicked, this, [this] {
        applyMode(false);
        saveSettings();
    });
    modeLay->addWidget(autoBtn_);
    modeLay->addWidget(manualBtn_);
    modeLay->addStretch();
    grid->addWidget(modes, row, 1);

    grid->addWidget(dimLabel(tr("Reference"), host), row, 2, Qt::AlignRight);
    referenceBox_ = new QComboBox(host);
    for (int hz : kReferences)
        referenceBox_->addItem(tr("A4 = %1 Hz").arg(hz), hz);
    const int wantRef = referenceBox_->findData(wantReference_);
    referenceBox_->setCurrentIndex(wantRef >= 0 ? wantRef : referenceBox_->findData(440));
    referenceBox_->setToolTip(
        tr("Concert pitch. 440 Hz unless you are playing with an ensemble that "
           "does not."));
    connect(referenceBox_, &QComboBox::activated, this, [this] {
        wantReference_ = referenceBox_->currentData().toInt();
        saveSettings();
    });
    grid->addWidget(referenceBox_, row, 3);
    ++row;

    statusLabel_ = dimLabel(QString(), host);
    statusLabel_->setWordWrap(true);
    grid->addWidget(statusLabel_, row, 0, 1, 4);

    return host;
}

QWidget *TunerWindow::buildStrings() {
    auto *well = new CardBase(this);
    well->setFillColor(Theme::Well);
    well->setRadius(12);

    auto *lay = new QVBoxLayout(well);
    lay->setContentsMargins(12, 10, 12, 12);
    lay->setSpacing(8);
    lay->addWidget(caption(tr("STRINGS"), well), 0, Qt::AlignHCenter);

    auto *rowHost = new QWidget(well);
    stringRow_ = new QHBoxLayout(rowHost);
    stringRow_->setContentsMargins(0, 0, 0, 0);
    stringRow_->setSpacing(8);
    stringRow_->addStretch();
    stringRow_->addStretch();
    lay->addWidget(rowHost);

    return well;
}

// ------------------------------------------------------------- presets

const TuningPreset *TunerWindow::currentTuning() const {
    if (!instrumentBox_ || !tuningBox_) return nullptr;
    const int i = instrumentBox_->currentIndex();
    const int t = tuningBox_->currentIndex();
    if (i < 0 || i >= instrumentPresets().size()) return nullptr;
    const QList<TuningPreset> &tunings = instrumentPresets()[i].tunings;
    if (t < 0 || t >= tunings.size()) return nullptr;
    return &tunings[t];
}

double TunerWindow::referenceA4() const {
    if (!referenceBox_) return 440.0;
    const int hz = referenceBox_->currentData().toInt();
    return hz > 0 ? hz : 440.0;
}

void TunerWindow::rebuildTunings() {
    const int i = instrumentBox_->currentIndex();
    if (i < 0 || i >= instrumentPresets().size()) return;

    QSignalBlocker block(tuningBox_);
    tuningBox_->clear();
    for (const TuningPreset &t : instrumentPresets()[i].tunings) {
        const QString label = t.spelling.isEmpty()
                                  ? t.name
                                  : QStringLiteral("%1  ·  %2").arg(t.name, t.spelling);
        tuningBox_->addItem(label, t.id);
    }
    const int want = tuningBox_->findData(wantTuning_);
    tuningBox_->setCurrentIndex(want >= 0 ? want : 0);
    wantTuning_ = tuningBox_->currentData().toString();
}

void TunerWindow::rebuildStringButtons() {
    for (StringButton *b : std::as_const(stringButtons_)) {
        stringRow_->removeWidget(b);
        b->deleteLater();
    }
    stringButtons_.clear();

    const TuningPreset *t = currentTuning();
    const bool chromatic = !t || t->notes.isEmpty();

    if (chromatic) {
        // Nothing to pick from, so manual has nothing to mean. The stored
        // preference is left alone: switching to chromatic and back should not
        // silently throw someone's mode switch for them.
        selected_ = 0;
        if (autoBtn_) autoBtn_->setChecked(true);
        if (manualBtn_) {
            manualBtn_->setChecked(false);
            manualBtn_->setEnabled(false);
        }
        return;
    }
    if (manualBtn_) manualBtn_->setEnabled(true);
    applyMode(automatic_);

    // Insert before the trailing stretch, so the row stays centred.
    int at = 1;
    for (int note : t->notes) {
        auto *b = new StringButton(note, stringRow_->parentWidget());
        b->setToolTip(tr("Tune to this string.\nClick to hear the note it "
                         "should be at, in your Monitor mix."));
        connect(b, &QAbstractButton::clicked, this, [this, b] {
            // Picking a string is a statement about what you are tuning, so it
            // takes the window out of auto rather than being overruled by the
            // next note played.
            const int index = stringButtons_.indexOf(b);
            if (index < 0) return;
            selectString(index);
            applyMode(false);
            saveSettings();
            // And play it: knowing which way to turn the peg is one half of
            // tuning, and hearing what you are aiming at is the other.
            client_->playTunerReference(
                noteFrequency(b->midiNote(), referenceA4()), 1600);
        });
        stringRow_->insertWidget(at++, b);
        stringButtons_.push_back(b);
    }
    if (stringButtons_.isEmpty()) return;
    selected_ = std::clamp(selected_, 0, static_cast<int>(stringButtons_.size()) - 1);
    selectString(selected_);
}

void TunerWindow::selectString(int index) {
    if (index < 0 || index >= stringButtons_.size()) return;
    selected_ = index;
    for (int i = 0; i < stringButtons_.size(); ++i)
        stringButtons_[i]->setChecked(i == index);
}

void TunerWindow::applyMode(bool automatic) {
    automatic_ = automatic;
    if (autoBtn_) autoBtn_->setChecked(automatic);
    if (manualBtn_) manualBtn_->setChecked(!automatic);
}

void TunerWindow::onInstrumentPicked() {
    wantInstrument_ = instrumentBox_->currentData().toString();
    // Every instrument has its own tunings, and "Standard" is the right guess
    // for all of them.
    wantTuning_ = QStringLiteral("standard");
    rebuildTunings();
    rebuildStringButtons();
    saveSettings();
}

void TunerWindow::onTuningPicked() {
    wantTuning_ = tuningBox_->currentData().toString();
    rebuildStringButtons();
    saveSettings();
}

// -------------------------------------------------------------- sources

void TunerWindow::refreshSources() {
    if (!isVisible()) return;

    const QStringList current = sourceBox_->currentData().toStringList();
    const QList<TunerSourceInfo> sources = client_->tunerSources();

    // Rebuilding a combo the user may have open is worse than a stale list, so
    // only touch it when the contents actually differ.
    QStringList signature;
    for (const TunerSourceInfo &s : sources)
        signature << s.kind + QLatin1Char('\t') + s.id + QLatin1Char('\t') + s.label;
    if (sourceSignatureValid_ && signature == sourceSignature_) return;
    sourceSignature_ = signature;
    sourceSignatureValid_ = true;

    QSignalBlocker block(sourceBox_);
    sourceBox_->clear();
    bool separated = false;
    for (const TunerSourceInfo &s : sources) {
        if (s.kind == QLatin1String("midi") && !separated && sourceBox_->count() > 0) {
            sourceBox_->insertSeparator(sourceBox_->count());
            separated = true;
        }
        const QString label = s.kind == QLatin1String("midi")
                                  ? tr("MIDI · %1").arg(s.label)
                                  : s.label;
        sourceBox_->addItem(label, QStringList{s.kind, s.id});
    }

    if (sourceBox_->count() == 0) {
        setStatus(client_->available()
                      ? tr("No inputs found. Plug in a microphone, an audio "
                           "interface, or a MIDI instrument.")
                      : tr("wavelined is not running, so there is nothing to "
                           "listen with."),
                  true);
        startedKind_.clear();
        startedSource_.clear();
        return;
    }

    // Put back what was selected, or what was selected the last time the
    // window was open, before falling back to the first entry.
    int index = -1;
    const QStringList preferred =
        current.size() == 2 ? current : QStringList{wantSourceKind_, wantSourceId_};
    for (int i = 0; i < sourceBox_->count(); ++i) {
        if (sourceBox_->itemData(i).toStringList() == preferred) {
            index = i;
            break;
        }
    }
    if (index < 0) index = 0;
    sourceBox_->setCurrentIndex(index);
    onSourcePicked();
}

void TunerWindow::onSourcePicked() {
    const QStringList data = sourceBox_->currentData().toStringList();
    if (data.size() != 2) return;
    wantSourceKind_ = data[0];
    wantSourceId_ = data[1];
    saveSettings();

    // A MIDI instrument reports the note it is playing; concert pitch is not
    // part of that conversation.
    const bool midi = wantSourceKind_ == QLatin1String("midi");
    referenceBox_->setEnabled(!midi);

    restartTuner();
}

void TunerWindow::restartTuner() {
    if (wantSourceId_.isEmpty()) return;
    if (startedKind_ == wantSourceKind_ && startedSource_ == wantSourceId_) return;

    const QString error = client_->startTuner(wantSourceKind_, wantSourceId_);
    if (!error.isEmpty()) {
        startedKind_.clear();
        startedSource_.clear();
        setStatus(tr("Could not listen to that input: %1").arg(error), true);
        return;
    }
    startedKind_ = wantSourceKind_;
    startedSource_ = wantSourceId_;
    setStatus(QString(), false);
}

void TunerWindow::setStatus(const QString &text, bool error) {
    if (!statusLabel_) return;
    statusLabel_->setVisible(!text.isEmpty());
    statusLabel_->setText(text);
    QPalette p = statusLabel_->palette();
    p.setColor(QPalette::WindowText, error ? Theme::Warn : Theme::TextDim);
    statusLabel_->setPalette(p);
}

// ----------------------------------------------------------------- ticks

void TunerWindow::onTick() {
    const TunerReadingInfo r = client_->tunerReading();
    const bool midi = startedKind_ == QLatin1String("midi");

    // Level as a meter position rather than a raw amplitude: -60 dBFS is the
    // bottom of the bar, which is roughly where the detector gives up anyway.
    if (inputMeter_) {
        const double db = r.level > 0.0 ? 20.0 * std::log10(r.level) : -120.0;
        inputMeter_->setLevel(std::clamp((db + 60.0) / 60.0, 0.0, 1.0));
    }

    double heard = 0.0;   // fractional MIDI note
    if (midi) {
        if (r.midiNote >= 0) heard = r.midiNote + r.bendCents / 100.0;
    } else if (r.frequencyHz > 0.0) {
        heard = frequencyToMidi(r.frequencyHz, referenceA4());
    }

    if (heard <= 0.0) {
        silentMs_ += kPollMs;
        if (silentMs_ >= kSilenceHoldMs) {
            inTuneMs_ = 0;
            dial_->setIdle(midi ? tr("Play a note") : tr("Play a string"));
            detectedLabel_->setText(QString());
            targetLabel_->setText(QString());
            for (StringButton *b : std::as_const(stringButtons_)) b->setInTune(false);
        }
        return;
    }
    silentMs_ = 0;

    const TuningPreset *tuning = currentTuning();
    const bool chromatic = !tuning || tuning->notes.isEmpty();

    // Which note is being aimed at. Chromatic aims at whatever is nearest;
    // manual aims at the chosen string; auto aims at the closest string, and
    // gives up on the idea if nothing is close.
    int target = 0;
    int matchedString = -1;
    if (chromatic) {
        target = static_cast<int>(std::lround(heard));
    } else if (!automatic_ && selected_ < tuning->notes.size()) {
        target = tuning->notes[selected_];
        matchedString = selected_;
    } else {
        double bestDistance = 0.0;
        for (int i = 0; i < tuning->notes.size(); ++i) {
            const double distance = std::fabs(heard - tuning->notes[i]);
            if (matchedString < 0 || distance < bestDistance) {
                bestDistance = distance;
                matchedString = i;
            }
        }
        if (matchedString < 0 || bestDistance > kAutoRangeSemitones) {
            // Nowhere near a string of this tuning. Naming the note is more
            // use than pinning the needle to a string nobody is playing.
            target = static_cast<int>(std::lround(heard));
            matchedString = -1;
        } else {
            target = tuning->notes[matchedString];
            if (automatic_) selectString(matchedString);
        }
    }

    const double cents = (heard - target) * 100.0;
    const bool onPitch = std::fabs(cents) <= kInTuneCents;
    inTuneMs_ = onPitch ? kInTuneHoldMs : std::max(0, inTuneMs_ - kPollMs);
    const bool showInTune = onPitch || inTuneMs_ > 0;

    const int heardNote = static_cast<int>(std::lround(heard));
    const QString detail =
        heardNote == target ? QString() : tr("heard %1").arg(noteName(heardNote));
    dial_->setReading(noteName(target), detail, cents, showInTune);

    const double targetHz = noteFrequency(target, referenceA4());
    const double heardHz = midi ? noteFrequency(heard, referenceA4()) : r.frequencyHz;
    detectedLabel_->setText(tr("Heard %1 Hz").arg(heardHz, 0, 'f', 1));
    targetLabel_->setText(
        tr("Target %1 · %2 Hz").arg(noteName(target)).arg(targetHz, 0, 'f', 2));

    for (int i = 0; i < stringButtons_.size(); ++i) {
        stringButtons_[i]->setChecked(i == matchedString);
        stringButtons_[i]->setInTune(i == matchedString && showInTune);
    }
}

// ---------------------------------------------------------------- window

void TunerWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    silentMs_ = kSilenceHoldMs;
    inTuneMs_ = 0;
    // Force the list to be rebuilt: the signature guard would otherwise skip a
    // reopen where nothing about the machine has changed, and the tuner has to
    // be started again regardless -- closing the window stopped it.
    sourceSignatureValid_ = false;
    refreshSources();
    poll_->start();
}

void TunerWindow::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    poll_->stop();
    // The daemon holds a capture stream open for as long as the tuner runs,
    // and nobody is looking at it now.
    client_->stopTuner();
    startedKind_.clear();
    startedSource_.clear();
    saveSettings();
}

// -------------------------------------------------------------- settings

void TunerWindow::loadSettings() {
    QSettings s;
    s.beginGroup(QStringLiteral("tuner"));
    wantInstrument_ = s.value(QStringLiteral("instrument"), wantInstrument_).toString();
    wantTuning_ = s.value(QStringLiteral("tuning"), wantTuning_).toString();
    wantSourceKind_ = s.value(QStringLiteral("sourceKind")).toString();
    wantSourceId_ = s.value(QStringLiteral("sourceId")).toString();
    wantReference_ = s.value(QStringLiteral("referenceA4"), 440).toInt();
    automatic_ = s.value(QStringLiteral("automatic"), true).toBool();
    s.endGroup();
}

void TunerWindow::saveSettings() const {
    QSettings s;
    s.beginGroup(QStringLiteral("tuner"));
    s.setValue(QStringLiteral("instrument"), wantInstrument_);
    s.setValue(QStringLiteral("tuning"), wantTuning_);
    s.setValue(QStringLiteral("sourceKind"), wantSourceKind_);
    s.setValue(QStringLiteral("sourceId"), wantSourceId_);
    s.setValue(QStringLiteral("automatic"), automatic_);
    if (referenceBox_)
        s.setValue(QStringLiteral("referenceA4"), referenceBox_->currentData().toInt());
    s.endGroup();
}
