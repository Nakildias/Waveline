// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Add/edit a Soundboard sound: name, waveform trim, and clip volume. The
// same dialog serves both -- Add starts from a freshly picked file with no
// prior settings, Edit preloads them -- because the controls are identical
// either way and a second dialog would only be a second place to keep them
// in sync.
//
// Decoding, trimming and playback all happen in the daemon: this window
// links no audio itself, the same rule every other GUI window in this
// mixer follows. The waveform is drawn from peaks the daemon computed
// (MixerClient::analyzeSoundboardSource) and Preview asks the daemon to
// play the file with the current trim/volume applied
// (MixerClient::previewSoundboardTrim), stopped when the dialog closes.

#pragma once

#include <QDialog>
#include <QVector>
#include <QWidget>

class MixerClient;
struct SoundboardSoundInfo;

class QLabel;
class QLineEdit;
class QPushButton;
class TrackSlider;

// ------------------------------------------------------------ WaveformView
// The peaks, dimmed outside the trimmed range, with two draggable handles at
// the trim boundaries. Whichever handle is nearer a press is the one that
// moves -- there is no third click behaviour to disambiguate.
class WaveformView : public QWidget {
    Q_OBJECT

public:
    explicit WaveformView(QWidget *parent = nullptr);

    void setPeaks(const QVector<float> &peaks, double durationMs);
    void setTrim(int startMs, int endMs);
    int startMs() const { return startMs_; }
    int endMs() const { return endMs_; }
    double durationMs() const { return durationMs_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void trimChanged(int startMs, int endMs);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    int xAtMs(double ms) const;
    double msAtX(int x) const;

    QVector<float> peaks_;
    double durationMs_ = 0.0;
    int startMs_ = 0;
    int endMs_ = 0;
    enum class Handle { None, Start, End };
    Handle dragging_ = Handle::None;
};

class SoundTrimDialog : public QDialog {
    Q_OBJECT

public:
    // Add mode: `existing` null, `sourcePath` is the file just picked.
    // Edit mode: `existing` names the sound being edited; `sourcePath` is
    // its currently stored file, played for preview until "Replace File..."
    // points elsewhere.
    SoundTrimDialog(MixerClient *client, const QString &sourcePath,
                    const SoundboardSoundInfo *existing, QWidget *parent = nullptr);
    ~SoundTrimDialog() override;

    QString soundName() const;
    int trimStartMs() const;
    int trimEndMs() const;
    double volume() const;
    // The file to save against. Equal to the constructor's sourcePath unless
    // "Replace File..." picked a different one.
    QString sourcePath() const { return sourcePath_; }

private slots:
    void onPreview();
    void onReplaceFile();

private:
    void loadWaveform();
    void updateDurationLabel();

    MixerClient *client_ = nullptr;
    QString sourcePath_;
    bool editMode_ = false;

    QLineEdit *nameEdit_ = nullptr;
    WaveformView *waveform_ = nullptr;
    QLabel *durationLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    TrackSlider *volumeSlider_ = nullptr;
    QLabel *volumePct_ = nullptr;
    QPushButton *previewBtn_ = nullptr;
    QPushButton *replaceBtn_ = nullptr;
};
