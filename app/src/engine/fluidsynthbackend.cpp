// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "fluidsynthbackend.h"

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace waveline {
namespace {

std::mutex g_apiMutex;
std::mutex g_instanceMutex;
std::string g_loadedPath;

// Sentinel when symbols come from the executable link map (RTLD_DEFAULT).
void *const kLinkedHandle = reinterpret_cast<void *>(1);

std::string envOr(const char *name) {
    const char *v = ::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string exeDir() {
    char buf[PATH_MAX]{};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    std::string path(buf, static_cast<std::size_t>(n));
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

const char *const kLibNames[] = {
    "libfluidsynth.so.3",
    "libfluidsynth.so.2",
    "libfluidsynth.so.1",
    "libfluidsynth.so",
};

using fluid_settings_t = void;
using fluid_synth_t = void;
using fluid_midi_event_t = void;

using fluid_settings_new_fn = fluid_settings_t *(*)();
using fluid_settings_delete_fn = void (*)(fluid_settings_t *);
using fluid_settings_setnum_fn = int (*)(fluid_settings_t *, const char *, double);
using fluid_settings_setstr_fn = int (*)(fluid_settings_t *, const char *, const char *);
using fluid_synth_new_fn = fluid_synth_t *(*)(fluid_settings_t *);
using fluid_synth_delete_fn = void (*)(fluid_synth_t *);
using fluid_synth_sfload_fn = int (*)(fluid_synth_t *, const char *, int);
using fluid_synth_sfunload_fn = int (*)(fluid_synth_t *, int, int);
using fluid_synth_program_select_fn = int (*)(fluid_synth_t *, int, int, int, int);
using fluid_synth_write_float_fn = int (*)(fluid_synth_t *, int, float *, int, int,
                                           float *, int, int);
using fluid_midi_event_new_fn = fluid_midi_event_t *(*)(void);
using fluid_midi_event_delete_fn = void (*)(fluid_midi_event_t *);
using fluid_midi_event_set_type_fn = void (*)(fluid_midi_event_t *, int);
using fluid_midi_event_set_channel_fn = void (*)(fluid_midi_event_t *, int);
using fluid_midi_event_set_key_fn = void (*)(fluid_midi_event_t *, int);
using fluid_midi_event_set_value_fn = void (*)(fluid_midi_event_t *, int);
using fluid_midi_event_set_velocity_fn = void (*)(fluid_midi_event_t *, int);
using fluid_synth_handle_midi_event_fn = int (*)(fluid_synth_t *, fluid_midi_event_t *);
using fluid_synth_get_sfont_by_id_fn = void *(*)(fluid_synth_t *, int);
using fluid_sfont_iteration_start_fn = void (*)(void *);
using fluid_sfont_iteration_next_fn = void *(*)(void *);
using fluid_preset_get_banknum_fn = int (*)(void *);
using fluid_preset_get_num_fn = int (*)(void *);

constexpr int kNoteOn = 0x90;
constexpr int kNoteOff = 0x80;
constexpr int kControlChange = 0xB0;
constexpr int kProgramChange = 0xC0;
constexpr int kPitchBend = 0xE0;

struct Api {
    void *handle = nullptr;
    bool linked = false;
    fluid_settings_new_fn settings_new = nullptr;
    fluid_settings_delete_fn settings_delete = nullptr;
    fluid_settings_setnum_fn settings_setnum = nullptr;
    fluid_settings_setstr_fn settings_setstr = nullptr;
    fluid_synth_new_fn synth_new = nullptr;
    fluid_synth_delete_fn synth_delete = nullptr;
    fluid_synth_sfload_fn synth_sfload = nullptr;
    fluid_synth_sfunload_fn synth_sfunload = nullptr;
    fluid_synth_program_select_fn synth_program_select = nullptr;
    fluid_synth_write_float_fn synth_write_float = nullptr;
    fluid_midi_event_new_fn midi_event_new = nullptr;
    fluid_midi_event_delete_fn midi_event_delete = nullptr;
    fluid_midi_event_set_type_fn midi_event_set_type = nullptr;
    fluid_midi_event_set_channel_fn midi_event_set_channel = nullptr;
    fluid_midi_event_set_key_fn midi_event_set_key = nullptr;
    fluid_midi_event_set_value_fn midi_event_set_value = nullptr;
    fluid_midi_event_set_velocity_fn midi_event_set_velocity = nullptr;
    fluid_synth_handle_midi_event_fn synth_handle_midi_event = nullptr;
    fluid_synth_get_sfont_by_id_fn synth_get_sfont_by_id = nullptr;
    fluid_sfont_iteration_start_fn sfont_iteration_start = nullptr;
    fluid_sfont_iteration_next_fn sfont_iteration_next = nullptr;
    fluid_preset_get_banknum_fn preset_get_banknum = nullptr;
    fluid_preset_get_num_fn preset_get_num = nullptr;
};

Api &globalApi() {
    static Api api;
    return api;
}

void resetApi(Api &api) {
    if (api.handle && api.handle != kLinkedHandle && !api.linked)
        ::dlclose(api.handle);
    api = Api{};
}

bool resolveSymbols(Api &api, void *symHandle, std::string &error) {
    auto load = [&](auto &fn, const char *name) -> bool {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(symHandle, name));
        if (fn) return true;
        const char *err = ::dlerror();
        error = std::string("FluidSynth missing symbol: ") + name;
        if (err) error += " (";
        if (err) error += err;
        if (err) error += ")";
        return false;
    };

    return load(api.settings_new, "new_fluid_settings") &&
           load(api.settings_delete, "delete_fluid_settings") &&
           load(api.settings_setnum, "fluid_settings_setnum") &&
           load(api.settings_setstr, "fluid_settings_setstr") &&
           load(api.synth_new, "new_fluid_synth") &&
           load(api.synth_delete, "delete_fluid_synth") &&
           load(api.synth_sfload, "fluid_synth_sfload") &&
           load(api.synth_sfunload, "fluid_synth_sfunload") &&
           load(api.synth_program_select, "fluid_synth_program_select") &&
           load(api.synth_write_float, "fluid_synth_write_float") &&
           load(api.midi_event_new, "new_fluid_midi_event") &&
           load(api.midi_event_delete, "delete_fluid_midi_event") &&
           load(api.midi_event_set_type, "fluid_midi_event_set_type") &&
           load(api.midi_event_set_channel, "fluid_midi_event_set_channel") &&
           load(api.midi_event_set_key, "fluid_midi_event_set_key") &&
           load(api.midi_event_set_value, "fluid_midi_event_set_value") &&
           load(api.midi_event_set_velocity, "fluid_midi_event_set_velocity") &&
           load(api.synth_handle_midi_event, "fluid_synth_handle_midi_event") &&
           load(api.synth_get_sfont_by_id, "fluid_synth_get_sfont_by_id") &&
           load(api.sfont_iteration_start, "fluid_sfont_iteration_start") &&
           load(api.sfont_iteration_next, "fluid_sfont_iteration_next") &&
           load(api.preset_get_banknum, "fluid_preset_get_banknum") &&
           load(api.preset_get_num, "fluid_preset_get_num");
}

bool loadFromHandle(Api &api, void *handle, bool linked, std::string &error) {
    resetApi(api);
    if (!resolveSymbols(api, handle, error)) return false;
    api.handle = handle;
    api.linked = linked;
    return true;
}

bool loadApi(Api &api, const char *path, std::string &error) {
    ::dlerror();
    void *handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = ::dlerror();
        error = err ? err : "dlopen failed";
        return false;
    }
    if (!loadFromHandle(api, handle, false, error)) return false;
    return true;
}

#ifdef WAVELINE_FLUIDSYNTH_LINKED
bool loadLinkedApi(Api &api, std::string &error) {
    ::dlerror();
    if (!loadFromHandle(api, RTLD_DEFAULT, true, error)) return false;
    return true;
}
#endif

std::vector<std::string> searchPaths() {
    std::vector<std::string> paths;
    const std::string overridePath = envOr("WAVELINE_FLUIDSYNTH_LIB");
    if (!overridePath.empty()) paths.push_back(overridePath);

    const std::string dir = exeDir();
    if (!dir.empty()) {
        paths.push_back(dir + "/libfluidsynth.so.3");
        paths.push_back(dir + "/libfluidsynth.so");
        paths.push_back(dir + "/../lib/libfluidsynth.so.3");
        paths.push_back(dir + "/../lib/libfluidsynth.so");
    }
    for (const char *name : kLibNames)
        paths.push_back(name);
    return paths;
}

void feedStatusByte(Api &api, fluid_synth_t *synth, fluid_midi_event_t *ev,
                    uint8_t status, uint8_t d1, uint8_t d2) {
    const int type = status & 0xF0;
    const int channel = status & 0x0F;
    api.midi_event_set_channel(ev, channel);
    switch (type) {
    case kNoteOn:
        api.midi_event_set_type(ev, d2 == 0 ? kNoteOff : kNoteOn);
        api.midi_event_set_key(ev, d1);
        api.midi_event_set_velocity(ev, d2);
        break;
    case kNoteOff:
        api.midi_event_set_type(ev, kNoteOff);
        api.midi_event_set_key(ev, d1);
        api.midi_event_set_velocity(ev, d2);
        break;
    case kControlChange:
        api.midi_event_set_type(ev, kControlChange);
        api.midi_event_set_key(ev, d1);
        api.midi_event_set_value(ev, d2);
        break;
    case kProgramChange:
        api.midi_event_set_type(ev, kProgramChange);
        api.midi_event_set_key(ev, d1);
        break;
    case kPitchBend:
        api.midi_event_set_type(ev, kPitchBend);
        api.midi_event_set_value(ev, (d2 << 7) | d1);
        break;
    default:
        return;
    }
    api.synth_handle_midi_event(synth, ev);
}

bool assignLoadedPresets(Api &api, fluid_synth_t *synth, int sfontId) {
    if (!api.synth_get_sfont_by_id || !api.sfont_iteration_start ||
        !api.sfont_iteration_next || !api.preset_get_banknum || !api.preset_get_num)
        return false;

    void *sfont = api.synth_get_sfont_by_id(synth, sfontId);
    if (!sfont) return false;

    int melBank = 0;
    int melNum = 0;
    int drumBank = 128;
    int drumNum = 0;
    bool hasMel = false;
    bool hasDrum = false;

    api.sfont_iteration_start(sfont);
    while (void *preset = api.sfont_iteration_next(sfont)) {
        const int bank = api.preset_get_banknum(preset);
        const int num = api.preset_get_num(preset);
        if (!hasMel && bank != 128) {
            melBank = bank;
            melNum = num;
            hasMel = true;
        }
        if (!hasDrum && bank == 128) {
            drumBank = bank;
            drumNum = num;
            hasDrum = true;
        }
        if (hasMel && hasDrum) break;
    }
    if (!hasMel) {
        api.sfont_iteration_start(sfont);
        if (void *preset = api.sfont_iteration_next(sfont)) {
            melBank = api.preset_get_banknum(preset);
            melNum = api.preset_get_num(preset);
            hasMel = true;
        }
    }
    if (!hasMel) return false;

    for (int ch = 0; ch < 16; ++ch) {
        if (ch == 9 && hasDrum)
            api.synth_program_select(synth, ch, sfontId, drumBank, drumNum);
        else
            api.synth_program_select(synth, ch, sfontId, melBank, melNum);
    }
    return true;
}

}  // namespace

bool FluidSynthBackend::available() {
    std::lock_guard<std::mutex> lock(g_apiMutex);
    Api &api = globalApi();
    if (api.handle) return true;

    std::string err;
#ifdef WAVELINE_FLUIDSYNTH_LINKED
    if (loadLinkedApi(api, err)) {
        g_loadedPath = "linked";
        return true;
    }
    std::fprintf(stderr, "waveline: linked FluidSynth unavailable: %s\n", err.c_str());
#endif

    for (const std::string &path : searchPaths()) {
        err.clear();
        if (loadApi(api, path.c_str(), err)) {
            g_loadedPath = path;
            std::fprintf(stderr, "waveline: FluidSynth loaded from %s\n", path.c_str());
            return true;
        }
    }
    std::fprintf(stderr, "waveline: FluidSynth not available: %s\n", err.c_str());
    return false;
}

std::string FluidSynthBackend::libraryPath() { return g_loadedPath; }

FluidSynthBackend::FluidSynthBackend() = default;

FluidSynthBackend::~FluidSynthBackend() { shutdown(); }

bool FluidSynthBackend::init(int sampleRate, std::string &error) {
    if (!available()) {
        error = "FluidSynth library not found (place libfluidsynth.so next to wavelined "
                "or set WAVELINE_FLUIDSYNTH_LIB)";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_instanceMutex);
    Api &api = globalApi();
    if (!api.settings_new) {
        error = "FluidSynth API not resolved";
        return false;
    }

    settings_ = api.settings_new();
    if (!settings_) {
        error = "new_fluid_settings failed";
        return false;
    }
    auto *st = static_cast<fluid_settings_t *>(settings_);
    // We render via fluid_synth_write_float into PipeWire — no platform drivers.
    if (api.settings_setstr) {
        api.settings_setstr(st, "audio.driver", "none");
        api.settings_setstr(st, "midi.driver", "none");
    }
    api.settings_setnum(st, "synth.sample-rate", sampleRate);
    // FluidSynth's own default is 0.2, which lands a synth far below any
    // microphone: measured against a ModMic on the same channel the keyboard
    // was barely visible in a recording. 1.0 puts it in the same range, and the
    // -6dB mono sum in render() means peaks here stay around where stereo 0.5
    // used to sit. The bus's input volume attenuates from there.
    api.settings_setnum(st, "synth.gain", 1.0);

    synth_ = api.synth_new(st);
    if (!synth_) {
        error = "new_fluid_synth failed";
        api.settings_delete(st);
        settings_ = nullptr;
        return false;
    }
    // clock.quantum-limit, so render() never allocates on the audio thread.
    left_.assign(8192, 0.0f);
    right_.assign(8192, 0.0f);
    return true;
}

void FluidSynthBackend::shutdown() {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    Api &api = globalApi();
    if (!api.synth_delete) return;
    if (synth_) {
        if (activeSfont_ >= 0)
            api.synth_sfunload(static_cast<fluid_synth_t *>(synth_), activeSfont_, 1);
        api.synth_delete(static_cast<fluid_synth_t *>(synth_));
        synth_ = nullptr;
    }
    if (settings_) {
        api.settings_delete(static_cast<fluid_settings_t *>(settings_));
        settings_ = nullptr;
    }
    activeSfont_ = -1;
}

int FluidSynthBackend::loadSoundfont(const std::string &path, std::string &error) {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    if (!synth_) {
        error = "FluidSynth not initialized";
        return -1;
    }
    Api &api = globalApi();
    if (activeSfont_ >= 0) {
        api.synth_sfunload(static_cast<fluid_synth_t *>(synth_), activeSfont_, 1);
        activeSfont_ = -1;
    }
    const int id = api.synth_sfload(static_cast<fluid_synth_t *>(synth_), path.c_str(), 1);
    if (id < 0) {
        error = "fluid_synth_sfload failed for " + path;
        return -1;
    }
    activeSfont_ = id;
    if (!assignLoadedPresets(api, static_cast<fluid_synth_t *>(synth_), id)) {
        error = "no usable presets in " + path;
        api.synth_sfunload(static_cast<fluid_synth_t *>(synth_), id, 1);
        activeSfont_ = -1;
        return -1;
    }
    return id;
}

void FluidSynthBackend::unloadSoundfont(int id) {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    if (!synth_ || id < 0) return;
    Api &api = globalApi();
    api.synth_sfunload(static_cast<fluid_synth_t *>(synth_), id, 1);
    if (activeSfont_ == id) activeSfont_ = -1;
}

void FluidSynthBackend::selectPreset(int sfontId, int bank, int preset) {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    if (!synth_ || sfontId < 0) return;
    Api &api = globalApi();
    for (int ch = 0; ch < 16; ++ch) {
        if (ch == 9 && bank != 128) continue;
        api.synth_program_select(static_cast<fluid_synth_t *>(synth_), ch, sfontId, bank,
                                 preset);
    }
}

void FluidSynthBackend::handleMidiBytes(const uint8_t *data, std::size_t len) {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    if (!synth_ || !data || len == 0) return;
    Api &api = globalApi();
    fluid_midi_event_t *ev = api.midi_event_new();
    if (!ev) return;

    uint8_t status = 0;
    uint8_t d1 = 0;
    int expect = 0;

    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (b >= 0xF8) continue;
        if (b & 0x80) {
            status = b;
            d1 = 0;
            const int type = status & 0xF0;
            if (type == kProgramChange)
                expect = 1;
            else if (type >= 0xC0 && type <= 0xDF)
                expect = 0;
            else
                expect = 2;
            if (expect == 0)
                feedStatusByte(api, static_cast<fluid_synth_t *>(synth_), ev, status, 0, 0);
            continue;
        }
        if (!status) continue;
        if (expect == 1) {
            feedStatusByte(api, static_cast<fluid_synth_t *>(synth_), ev, status, b, 0);
            status = 0;
            expect = 0;
            continue;
        }
        if (expect == 2 && d1 == 0) {
            d1 = b;
            continue;
        }
        feedStatusByte(api, static_cast<fluid_synth_t *>(synth_), ev, status, d1, b);
        status = 0;
        expect = 0;
    }
    api.midi_event_delete(ev);
}

void FluidSynthBackend::render(float *out, int frames) {
    std::unique_lock<std::mutex> lock(g_instanceMutex, std::try_to_lock);
    if (!lock.owns_lock() || !synth_ || !out || frames <= 0) {
        if (out && frames > 0) std::memset(out, 0, static_cast<size_t>(frames) * sizeof(float));
        return;
    }
    const size_t need = static_cast<size_t>(frames);
    if (left_.size() < need) {
        // Only reached if the graph quantum grew past what init() reserved.
        left_.resize(need);
        right_.resize(need);
    }

    Api &api = globalApi();
    // Distinct destinations. Passing the same buffer for both channels makes
    // the right write land on top of the left one sample for sample, so the
    // "mono" output is really the synth's right channel with the left thrown
    // away.
    api.synth_write_float(static_cast<fluid_synth_t *>(synth_), frames,
                          left_.data(), 0, 1, right_.data(), 0, 1);

    // -6 dB on the sum keeps a hard-panned note at the level it had before and
    // stops a centred one clipping the mono port.
    for (size_t i = 0; i < need; ++i) out[i] = 0.5f * (left_[i] + right_[i]);
}

}  // namespace waveline
