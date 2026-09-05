// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The mixer window: a row of input cards over the two output mixes, with a
// sidebar for the things Wave Link has no equivalent of -- noise suppression,
// the microphone's own hardware controls, and application routing.
//
// The microphone is drawn as the first input card rather than as a special
// case in the sidebar. It has a level in each mix exactly like any other
// input, and hiding that behind a different kind of control was the main
// reason the old layout was hard to read.

#pragma once

#include <QHash>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>

class AboutWindow;
class SettingsWindow;
class CardBase;
class ChannelEffectsWindow;
class ChannelStrip;
class GlobalEffectsWindow;
class IconToggle;
class LevelMeter;
class MixerClient;
struct ChannelInfo;
struct MonitorOutputInfo;
struct OutputInfo;
class CompanionWindow;
class ProfilesWindow;
class SoundboardWindow;
class SoundSharingTab;
class StatusDot;
class VirtualRackWindow;
class TunerWindow;
class Section;
class ToggleSwitch;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // For screenshots and scripted checks.
    void scrollSidebar(int pixels);
    // Opens the tuner and hands it back, so its layout can be grabbed the same
    // way the main window's is.
    QWidget *openTunerWindow();

protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    // Drag-and-drop reordering of the channel cards happens on the strip row;
    // see handleStripDrag().
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onChanged();
    void onLevels();
    void onAvailabilityChanged(bool available);
    void onHardwareMonitorMoved(int value);
    void onHardwareMonitorReleased();
    void onOutputPicked(int row, int comboIndex);
    void onAddMonitorOutput();
    void onRemoveMonitorOutput(int index);
    void showProfiles();
    void showGlobalEffects(const QString &masterId = QStringLiteral("mic"));
    void showVirtualRack(const QString &masterId, const QString &name);
    void showChannelEffects(const QString &id, const QString &name);
    void showAbout();
    void showLatencyDiagnostics();
    void showTuner();
    void showSoundboard();
    void showCompanion();
    void onAddMasterBus();
    void onAddMasterBus(const QString &busType, const QString &deviceMatch);
    void onAddMasterBusMenu();
    void onRemoveMasterBus(const QString &id);
    void onRebuildMasterCapture(const QString &id);

private:
    void followScreen();
    QWidget *buildHeader();
    QWidget *buildInputs();
    QWidget *buildOutputs();
    QWidget *buildSidebar();
    Section *buildSoundSharingSection();
    Section *buildHardwareSection();

    // label + slider + fixed-width readout, laid out consistently.
    QWidget *buildSliderRow(const QString &label, QSlider *&slider, QLabel *&readout,
                            const QString &widest, QWidget *parent);
    // caption + switch, the sidebar's standard setting row.
    QWidget *buildSwitchRow(const QString &label, ToggleSwitch *&sw,
                            const QString &tip, QWidget *parent);

    void rebuildStrips();
    void rebuildMasterStrips();
    void refreshMasterStrip(const QString &masterId, ChannelStrip *strip);
    // Pulls the user's per-card colours and icons out of the daemon and hands
    // them to the theme, so every place a card is drawn -- the strips, the
    // Apps tab's badges, the sharing pickers -- agrees without being told
    // individually. Cheap when nothing changed; the strips are only repainted
    // when the table actually differs.
    void refreshCardLooks();
    // Last known card colours, from our own settings, applied before the
    // daemon has been asked. See kCardLooksKey.
    void applyCachedCardLooks();
    void applyCardLooks();
    // The panel behind a card's icon tile: name, colour, icon.
    void editCardIdentity(const QString &kind, const QString &id);
    void syncAddMasterButton();
    void syncInputsSeparator();
    int channelStripInsertIndex() const;
    // Puts the installed microphone's identity through the whole window: the
    // title, the wordmark's suffix, the name the Stream mix is published
    // under, and whether the hardware panel and the hardware monitor mode
    // exist at all.
    //
    // Called from onChanged() rather than once at construction because the
    // answer comes from the daemon, which is routinely not running yet when
    // the window is built -- and a hardware panel that could only appear by
    // rebuilding the sidebar would never appear for anyone who starts the GUI
    // first and the daemon second.
    void applyDeviceProfile();
    void updateHardwareMonitorLabel(int percent);
    // Header dot and caption: daemon down, device unplugged, or all well.
    // Maps an RMS amplitude onto a 0..1 meter position.
    static double meterPosition(double rms);
    void syncMonitorOutputUi();
    void refreshMonitorOutputCombos();
    void refreshMonitorOutputControls();
    // Fills one row's device picker, leaving out devices other rows already
    // own -- one output device drives exactly one Monitor mix. Takes the lists
    // the caller already fetched: every accessor here is a D-Bus round trip.
    void fillMonitorOutputCombo(QComboBox *combo, int index,
                                const QList<MonitorOutputInfo> &states,
                                const QList<OutputInfo> &outs);
    // A device no Monitor mix has claimed yet, or empty when all are taken.
    static QString freeMonitorOutputDevice(const QList<MonitorOutputInfo> &states,
                                           const QList<OutputInfo> &outs);
    QString monitorOutputLabel(int index, int total) const;

    MixerClient *client_ = nullptr;

    // --------------------------------------------------------------- header
    // Graph latency: how much audio moves per PipeWire cycle. In the header
    // rather than the sidebar because it is machine-wide and affects every
    // strip at once -- a sidebar full of per-microphone settings is the wrong
    // neighbourhood for the one control that changes all of them.
    QPushButton *diagnosticsBtn_ = nullptr;
    // Settings: latency, warnings, services and the measured latency table.
    // Created
    // on first use and kept, so it reopens on the tab it was left on.
    SettingsWindow *settingsWindow_ = nullptr;
    QPushButton *manageProfiles_ = nullptr;
    QPushButton *tunerBtn_ = nullptr;
    QPushButton *soundboardBtn_ = nullptr;
    QPushButton *companionBtn_ = nullptr;
    QPushButton *aboutBtn_ = nullptr;
    QLabel *versionLabel_ = nullptr;
    QLabel *bannerLabel_ = nullptr;
    // One row per program taking streams off their channels. Rebuilt only when
    // the set changes: this is driven by the same poll as everything else, and
    // a banner that rebuilt four times a second would eat the click on its own
    // dismiss button.
    QWidget *routingBanner_ = nullptr;
    QVBoxLayout *routingBannerRows_ = nullptr;
    QString routingBannerSignature_;
    void refreshRoutingBanner();

    // --------------------------------------------------------------- inputs
    QHBoxLayout *stripRow_ = nullptr;
    QLabel *emptyLabel_ = nullptr;
    QWidget *inputsSeparator_ = nullptr;
    QPushButton *addMasterBtn_ = nullptr;
    QHash<QString, ChannelStrip *> masterStrips_;
    QStringList masterStripOrder_;
    // What each input device's own monitor send is doing. Cached rather than
    // asked for, because onLevels() runs at meter rate and every masterMic*()
    // getter is a blocking D-Bus round trip.
    struct MasterMonitorUi {
        bool monitoring = false;   // the ear toggle: does it reach the Monitor mix
        bool muted = false;
        double volume = 1.0;
    };
    QHash<QString, MasterMonitorUi> masterMonitor_;
    // What refreshCardLooks() last pushed, as a flat signature, so the common
    // case -- nothing customised, or nothing changed -- costs one comparison
    // rather than a repaint of every card four times a second.
    QString cardLooksSig_;
    // Pushes that state onto the card: top meter tint and caption.
    void applyMasterMonitorState(const QString &masterId);
    QHash<QString, ChannelStrip *> strips_;

    // ------------------------------------------------- channel card order
    // Purely a window preference: the daemon has no notion of a left-to-right
    // order and does not need one, so this lives in QSettings next to the
    // per-card link state rather than going over D-Bus. Ids the daemon no
    // longer reports are kept rather than pruned -- a channel that comes back
    // should come back where the user put it.
    QWidget *stripHost_ = nullptr;
    QStringList channelOrder_;
    void loadChannelOrder();
    void saveChannelOrderFromRow();
    // Sorts the daemon's channel list into the remembered order. Anything not
    // in it goes to the end, in the order the daemon gave, so a newly created
    // channel appears where it always did.
    QList<ChannelInfo> orderedChannels() const;
    // Live drag handling on stripHost_; see the drag handle on ChannelStrip.
    bool handleStripDrag(QEvent *event);
    // The channel strip the cursor is over, or the gap it is nearest.
    int channelDropIndex(const QPoint &hostPos) const;

    // -------------------------------------------------------------- outputs
    struct MonitorOutputRowUi {
        QLabel *icon = nullptr;
        QLabel *label = nullptr;
        QComboBox *combo = nullptr;
        QPushButton *addBtn = nullptr;
        QPushButton *removeBtn = nullptr;
        IconToggle *mute = nullptr;
        QSlider *volume = nullptr;
        LevelMeter *meter = nullptr;
    };
    void applyMonitorOutputRowConnected(MonitorOutputRowUi &row, bool connected);
    // Greys the "+" once every output device already drives a Monitor mix.
    void applyAddMonitorOutputEnabled(MonitorOutputRowUi &row,
                                      const QList<MonitorOutputInfo> &states,
                                      const QList<OutputInfo> &outs);
    QGridLayout *outputsGrid_ = nullptr;
    CardBase *outputsCard_ = nullptr;
    QList<MonitorOutputRowUi> monitorOutputRows_;
    QLabel *streamIcon_ = nullptr;
    QLabel *streamNameLabel_ = nullptr;
    QString monitorOutputsSig_;
    // The Stream mix's fixed name, retitled when the device profile lands.
    class BoxedName *streamMixName_ = nullptr;
    IconToggle *streamMute_ = nullptr;
    QSlider *streamVolume_ = nullptr;
    LevelMeter *streamMeter_ = nullptr;

    // -------------------------------------------------------------- sidebar
    QWidget *sidebar_ = nullptr;
    QScrollArea *sidebarScroll_ = nullptr;
    QHash<QString, GlobalEffectsWindow *> globalEffects_;
    QHash<QString, VirtualRackWindow *> virtualRacks_;
    AboutWindow *aboutWindow_ = nullptr;
    ProfilesWindow *profilesWindow_ = nullptr;
    TunerWindow *tunerWindow_ = nullptr;
    SoundboardWindow *soundboardWindow_ = nullptr;
    CompanionWindow *companionWindow_ = nullptr;
    QHash<QString, ChannelEffectsWindow *> channelEffects_;

    Section *hardwareSection_ = nullptr;
    // What applyDeviceProfile() last applied, so the several widgets it
    // touches are only rewritten when the answer actually changes.
    QString appliedBrand_;
    bool appliedHardware_ = true;
    bool profileApplied_ = false;

    ToggleSwitch *clipguard_ = nullptr;
    QSlider *hwMonitor_ = nullptr;
    QLabel *hwMonitorLabel_ = nullptr;
    bool hwMonitorHeld_ = false;
    QSlider *hpVolume_ = nullptr;
    QLabel *hpVolumeLabel_ = nullptr;
    bool hpVolumeHeld_ = false;
    IconToggle *hpMute_ = nullptr;
    QLabel *deviceLabel_ = nullptr;

    SoundSharingTab *soundSharingTab_ = nullptr;

    QTimer changeDebounce_;
};
