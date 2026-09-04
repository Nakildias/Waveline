// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "soundtrimdialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "mixerclient.h"
#include "theme.h"
#include "widgets.h"

namespace {

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    return l;
}

QString msToClock(double ms) {
    if (ms < 0) ms = 0;
    const int totalMs = static_cast<int>(std::lround(ms));
    const int secs = totalMs / 1000;
    const int tenths = (totalMs % 1000) / 100;
    return QStringLiteral("%1:%2.%3")
        .arg(secs / 60, 1, 10, QLatin1Char('0'))
        .arg(secs % 60, 2, 10, QLatin1Char('0'))
        .arg(tenths);
}

}  // namespace

// ================================================================ WaveformView

WaveformView::WaveformView(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(120);
    setCursor(Qt::PointingHandCursor);
}

void WaveformView::setPeaks(const QVector<float> &peaks, double durationMs) {
    peaks_ = peaks;
    durationMs_ = std::max(0.0, durationMs);
    update();
}

void WaveformView::setTrim(int startMs, int endMs) {
    startMs_ = std::clamp(startMs, 0, static_cast<int>(durationMs_));
    endMs_ = std::clamp(endMs <= 0 ? static_cast<int>(durationMs_) : endMs, startMs_,
                        static_cast<int>(durationMs_));
    update();
}

QSize WaveformView::sizeHint() const { return QSize(560, 140); }
QSize WaveformView::minimumSizeHint() const { return QSize(240, 100); }

int WaveformView::xAtMs(double ms) const {
    if (durationMs_ <= 0) return 0;
    return static_cast<int>((ms / durationMs_) * width());
}

double WaveformView::msAtX(int x) const {
    if (width() <= 0) return 0;
    return std::clamp(static_cast<double>(x) / width(), 0.0, 1.0) * durationMs_;
}

void WaveformView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::Well);

    if (peaks_.isEmpty() || durationMs_ <= 0) {
        p.setPen(Theme::TextFaint);
        p.drawText(rect(), Qt::AlignCenter, tr("No waveform"));
        return;
    }

    const int midY = height() / 2;
    const int startX = xAtMs(startMs_);
    const int endX = xAtMs(endMs_);

    const double pxPerPeak = static_cast<double>(width()) / peaks_.size();
    for (int i = 0; i < peaks_.size(); ++i) {
        const int x = static_cast<int>(i * pxPerPeak);
        const int barW = std::max(1, static_cast<int>(pxPerPeak));
        const int h = std::max(1, static_cast<int>(peaks_[i] * (height() * 0.9)));
        const bool inTrim = x >= startX && x <= endX;
        p.setPen(Qt::NoPen);
        p.setBrush(inTrim ? Theme::Accent : Theme::Line);
        p.drawRect(x, midY - h / 2, barW, h);
    }

    // Dim the trimmed-away regions with a translucent overlay, so the kept
    // range reads as "in the light" rather than the cut parts merely being a
    // different bar colour easy to miss at a glance.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 140));
    if (startX > 0) p.drawRect(0, 0, startX, height());
    if (endX < width()) p.drawRect(endX, 0, width() - endX, height());

    auto drawHandle = [&](int x, const QColor &c) {
        p.setPen(QPen(c, 2));
        p.drawLine(x, 0, x, height());
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        QPolygon tri;
        tri << QPoint(x - 5, 0) << QPoint(x + 5, 0) << QPoint(x, 8);
        p.drawPolygon(tri);
    };
    drawHandle(startX, Theme::Accent);
    drawHandle(endX, Theme::Danger);
}

void WaveformView::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || durationMs_ <= 0) return;
    const int startX = xAtMs(startMs_);
    const int endX = xAtMs(endMs_);
    const int x = e->pos().x();
    dragging_ =
        std::abs(x - startX) <= std::abs(x - endX) ? Handle::Start : Handle::End;
    mouseMoveEvent(e);
}

void WaveformView::mouseMoveEvent(QMouseEvent *e) {
    if (dragging_ == Handle::None || durationMs_ <= 0) return;
    const int ms = static_cast<int>(msAtX(e->pos().x()));
    if (dragging_ == Handle::Start) {
        startMs_ = std::clamp(ms, 0, endMs_);
    } else {
        endMs_ = std::clamp(ms, startMs_, static_cast<int>(durationMs_));
    }
    update();
    emit trimChanged(startMs_, endMs_);
}

void WaveformView::mouseReleaseEvent(QMouseEvent *) { dragging_ = Handle::None; }

// ============================================================== SoundTrimDialog

SoundTrimDialog::SoundTrimDialog(MixerClient *client, const QString &sourcePath,
                                 const SoundboardSoundInfo *existing, QWidget *parent)
    : QDialog(parent), client_(client), sourcePath_(sourcePath),
      editMode_(existing != nullptr) {
    setWindowTitle(editMode_ ? tr("Edit Sound") : tr("Add Sound"));
    setMinimumWidth(600);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(18, 18, 18, 18);
    lay->setSpacing(12);

    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(dimLabel(tr("Name"), this));
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setText(existing ? existing->name
                                : QFileInfo(sourcePath).completeBaseName());
    nameRow->addWidget(nameEdit_, 1);
    lay->addLayout(nameRow);

    waveform_ = new WaveformView(this);
    lay->addWidget(waveform_);
    connect(waveform_, &WaveformView::trimChanged, this,
            [this](int, int) { updateDurationLabel(); });

    auto *durRow = new QHBoxLayout;
    durationLabel_ = dimLabel(QString(), this);
    durRow->addWidget(durationLabel_);
    durRow->addStretch();
    replaceBtn_ = new QPushButton(tr("Replace File..."), this);
    replaceBtn_->setVisible(editMode_);
    connect(replaceBtn_, &QPushButton::clicked, this, &SoundTrimDialog::onReplaceFile);
    durRow->addWidget(replaceBtn_);
    previewBtn_ = new QPushButton(tr("Preview"), this);
    connect(previewBtn_, &QPushButton::clicked, this, &SoundTrimDialog::onPreview);
    durRow->addWidget(previewBtn_);
    lay->addLayout(durRow);

    auto *volRow = new QHBoxLayout;
    volRow->addWidget(dimLabel(tr("Volume"), this));
    volumeSlider_ = new TrackSlider(this);
    volumeSlider_->setRange(0, 200);
    volumeSlider_->setValue(static_cast<int>((existing ? existing->volume : 1.0) * 100.0 + 0.5));
    volumeSlider_->setAccent(Theme::Accent);
    volumeSlider_->setToolTip(
        tr("This sound's own level. 100% is unity; boost a quiet clip up to 200%."));
    volRow->addWidget(volumeSlider_, 1);
    volumePct_ = dimLabel(QStringLiteral("%1%").arg(volumeSlider_->value()), this);
    volumePct_->setMinimumWidth(44);
    volumePct_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volRow->addWidget(volumePct_);
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int v) {
        volumePct_->setText(QStringLiteral("%1%").arg(v));
    });
    lay->addLayout(volRow);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    QPalette warnPal = statusLabel_->palette();
    warnPal.setColor(QPalette::WindowText, Theme::Danger);
    statusLabel_->setPalette(warnPal);
    statusLabel_->setVisible(false);
    lay->addWidget(statusLabel_);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    loadWaveform();
    if (existing) waveform_->setTrim(existing->trimStartMs, existing->trimEndMs);
    updateDurationLabel();
}

SoundTrimDialog::~SoundTrimDialog() {
    if (client_) client_->stopSoundboardPreview();
}

void SoundTrimDialog::loadWaveform() {
    const QString reply = client_->analyzeSoundboardSource(sourcePath_);
    if (reply.isEmpty()) {
        statusLabel_->setText(
            tr("Could not read '%1'.\n\n%2").arg(sourcePath_, client_->lastError()));
        statusLabel_->setVisible(true);
        return;
    }
    statusLabel_->setVisible(false);
    const int tab = reply.indexOf(QLatin1Char('\t'));
    const double durationMs = (tab < 0 ? reply : reply.left(tab)).toDouble();
    const QStringList peakStrs =
        (tab < 0 ? QString() : reply.mid(tab + 1)).split(QLatin1Char(','), Qt::SkipEmptyParts);
    QVector<float> peaks;
    peaks.reserve(peakStrs.size());
    for (const QString &s : peakStrs) peaks.push_back(s.toFloat());
    waveform_->setPeaks(peaks, durationMs);
    waveform_->setTrim(0, static_cast<int>(durationMs));
}

void SoundTrimDialog::updateDurationLabel() {
    durationLabel_->setText(tr("%1 - %2 of %3")
                                .arg(msToClock(waveform_->startMs()),
                                     msToClock(waveform_->endMs()),
                                     msToClock(waveform_->durationMs())));
}

void SoundTrimDialog::onPreview() {
    client_->previewSoundboardTrim(sourcePath_, waveform_->startMs(), waveform_->endMs(),
                                   volumeSlider_->value() / 100.0);
}

void SoundTrimDialog::onReplaceFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Replace File"), QFileInfo(sourcePath_).absolutePath(),
        tr("Audio files (*.wav *.mp3)"));
    if (path.isEmpty()) return;
    client_->stopSoundboardPreview();
    sourcePath_ = path;
    loadWaveform();
    updateDurationLabel();
}

QString SoundTrimDialog::soundName() const {
    const QString name = nameEdit_->text().trimmed();
    return name.isEmpty() ? QFileInfo(sourcePath_).completeBaseName() : name;
}

int SoundTrimDialog::trimStartMs() const { return waveform_->startMs(); }
int SoundTrimDialog::trimEndMs() const { return waveform_->endMs(); }
double SoundTrimDialog::volume() const { return volumeSlider_->value() / 100.0; }
