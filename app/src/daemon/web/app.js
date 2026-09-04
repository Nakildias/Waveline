// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The companion page.
//
// The daemon pushes; this never polls. Two message types arrive: a full state
// document whenever anything a person can change did change (and on a slow tick
// for the application list, which follows PipeWire rather than any setting), and
// a meters-only document at 20 Hz.
//
// The one rule that matters here is that a control being touched is never
// written to from a push. State arrives up to twenty times a second and the
// round trip through the daemon is not instant, so a fader that accepted every
// document mid-drag would fight the finger holding it -- visibly, as a stutter
// backwards. Every control therefore owns its value while it is being used and
// for a moment afterwards, and only then goes back to mirroring the daemon.

'use strict';

// ---------------------------------------------------------------- palette
//
// Ported from src/ui/theme.cpp. Duplicated rather than served from the daemon
// because these are the *defaults*: what the user has actually chosen arrives
// in every state document under `looks`, and the daemon only stores the cards
// somebody has picked a colour for.

const CHANNEL_COLORS = {
    mic: '#5b6cf0', system: '#2f86e8', voice: '#c9d43a', music: '#e0479e',
    video: '#22d3ee', browser: '#9d4ae0', game: '#e8455a', sfx: '#c2410c',
};
const CHANNEL_ICONS = {
    mic: 'microphone', system: 'system', voice: 'voice', music: 'music',
    video: 'video', browser: 'browser', game: 'game', sfx: 'sfx',
};
const BUS_PALETTE = ['#5b6cf0', '#10b981', '#f97316', '#06b6d4', '#a96df5',
                     '#ec4899', '#f59e0b', '#84cc16', '#3b82f6'];

// 24x24 stroke glyphs, in the shape of the Tabler set the desktop uses.
const ICONS = {
    microphone: 'M12 2a3 3 0 0 1 3 3v5a3 3 0 0 1 -6 0v-5a3 3 0 0 1 3 -3M5 10a7 7 0 0 0 14 0M8 21h8M12 17v4',
    music: 'M3 17a3 3 0 1 0 6 0a3 3 0 0 0 -6 0M13 17a3 3 0 1 0 6 0a3 3 0 0 0 -6 0M9 17v-13h10v13M9 8h10',
    game: 'M4 6h16a2 2 0 0 1 2 2v8a2 2 0 0 1 -2 2h-16a2 2 0 0 1 -2 -2v-8a2 2 0 0 1 2 -2M6 12h4M8 10v4M15 11v.01M18 13v.01',
    system: 'M4 4h16a1 1 0 0 1 1 1v10a1 1 0 0 1 -1 1h-16a1 1 0 0 1 -1 -1v-10a1 1 0 0 1 1 -1M7 20h10M9 16v4M15 16v4',
    voice: 'M4 14v-3a8 8 0 1 1 16 0v3M18 19c0 1.657 -2.686 3 -6 3M4 14h3a2 2 0 0 1 2 2v3a2 2 0 0 1 -2 2h-1a2 2 0 0 1 -2 -2zM20 14h-3a2 2 0 0 0 -2 2v3a2 2 0 0 0 2 2h1a2 2 0 0 0 2 -2z',
    video: 'M15 10l4.553 -2.276a1 1 0 0 1 1.447 .894v6.764a1 1 0 0 1 -1.447 .894l-4.553 -2.276zM5 6h8a2 2 0 0 1 2 2v8a2 2 0 0 1 -2 2h-8a2 2 0 0 1 -2 -2v-8a2 2 0 0 1 2 -2',
    browser: 'M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0M3.6 9h16.8M3.6 15h16.8M11.5 3a17 17 0 0 0 0 18M12.5 3a17 17 0 0 1 0 18',
    sfx: 'M16 18a2 2 0 0 1 2 2a2 2 0 0 1 2 -2a2 2 0 0 1 -2 -2a2 2 0 0 1 -2 2M10 8a4 4 0 0 1 4 4a4 4 0 0 1 4 -4a4 4 0 0 1 -4 -4a4 4 0 0 1 -4 4M5 17a1.5 1.5 0 0 1 1.5 1.5a1.5 1.5 0 0 1 1.5 -1.5a1.5 1.5 0 0 1 -1.5 -1.5a1.5 1.5 0 0 1 -1.5 1.5',
    piano: 'M5 4h14a1 1 0 0 1 1 1v14a1 1 0 0 1 -1 1h-14a1 1 0 0 1 -1 -1v-14a1 1 0 0 1 1 -1M8 4v10M12 4v10M16 4v10M4 14h16',
    headphones: 'M4 15a2 2 0 0 1 2 -2h1a2 2 0 0 1 2 2v3a2 2 0 0 1 -2 2h-1a2 2 0 0 1 -2 -2l0 -3M15 15a2 2 0 0 1 2 -2h1a2 2 0 0 1 2 2v3a2 2 0 0 1 -2 2h-1a2 2 0 0 1 -2 -2l0 -3M4 15v-3a8 8 0 0 1 16 0v3',
    'headphones-off': 'M3 3l18 18M4 15a2 2 0 0 1 2 -2h1a2 2 0 0 1 2 2v3a2 2 0 0 1 -2 2h-1a2 2 0 0 1 -2 -2l0 -3M17 13h1a2 2 0 0 1 2 2v1m-.589 3.417c-.361 .36 -.86 .583 -1.411 .583h-1a2 2 0 0 1 -2 -2v-3M4 15v-3c0 -2.21 .896 -4.21 2.344 -5.658m2.369 -1.638a8 8 0 0 1 11.287 7.296v3',
    stream: 'M18.364 19.364a9 9 0 1 0 -12.728 0M15.536 16.536a5 5 0 1 0 -7.072 0M11 13a1 1 0 1 0 2 0a1 1 0 1 0 -2 0',
    'stream-off': 'M18.364 19.364a9 9 0 0 0 -9.721 -14.717m-2.488 1.509a9 9 0 0 0 -.519 13.208M15.536 16.536a5 5 0 0 0 -3.536 -8.536m-3 1a5 5 0 0 0 -.535 7.536M12 12a1 1 0 1 0 1 1M3 3l18 18',
    speaker: 'M15 8a5 5 0 0 1 0 8M17.7 5a9 9 0 0 1 0 14M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2l3.5 -4.5a.8 .8 0 0 1 1.5 .5v14a.8 .8 0 0 1 -1.5 -.5z',
    'speaker-muted': 'M15 8a5 5 0 0 1 1.912 4.36M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2l3.5 -4.5a.8 .8 0 0 1 1.5 .5v3M11 15v3.5a.8 .8 0 0 1 -1.5 .5l-3.5 -4.5M3 3l18 18',
    'microphone-off': 'M3 3l18 18M9 5a3 3 0 0 1 6 0v5a3 3 0 0 1 -.13 .874m-2 2a3 3 0 0 1 -3.87 -2.872v-1M5 10a7 7 0 0 0 10.846 5.85m2 -2a6.967 6.967 0 0 0 1.152 -3.85M8 21l8 0M12 17l0 4',
    // The desktop's heartbeat.svg, which is the effects button on every card.
    heartbeat: 'M3 12h4.5l1.5 -6l4 12l2 -9l1.5 3h4.5',
    ear: 'M6 10a7 7 0 1 1 13 3.6a10 10 0 0 1 -2 2a8 8 0 0 0 -2 3a4.5 4.5 0 0 1 -6.8 1.4M10 10a3 3 0 1 1 5 2.2',
    'ear-off': 'M6 10c0 -1.146 .277 -2.245 .78 -3.219m1.792 -2.208a7 7 0 0 1 10.428 9.027a10 10 0 0 1 -.633 .762m-2.045 1.96a8 8 0 0 0 -1.322 2.278a4.5 4.5 0 0 1 -6.8 1.4M11.42 7.414a3 3 0 0 1 4.131 4.13M3 3l18 18',
    // equal.svg: the two mix faders move together.
    equal: 'M5 10h14M5 14h14',
    wave: 'M3 12h2l3 8l4 -16l3 8h6',
    check: 'M5 12l5 5l9 -9',
};

// ------------------------------------------------------------------ prefs
//
// How this page looks, as opposed to what the mixer is doing. Kept on the
// device rather than in the daemon on purpose: a phone showing two cards at a
// time wants the scrollbar and a desktop browser showing the whole row does
// not, and a setting stored centrally would make one of them wrong every time
// the other changed it. Nothing here reaches the audio, so there is nothing to
// keep in sync.

const PREFS_KEY = 'waveline.companion.prefs';

const prefs = (() => {
    // `links` is here rather than in the daemon for the same reason: it changes
    // nothing about the audio until a fader is next touched, and the desktop
    // keeps its own copy per card too. `collapsed*` is the same argument again --
    // a phone in landscape wants the bands folded and the machine driving the
    // stream does not.
    const defaults = {
        easyScroll: true,
        reduceMotion: false,
        headerCollapsed: false,
        footerCollapsed: false,
        links: {},
    };
    try {
        const raw = localStorage.getItem(PREFS_KEY);
        return raw ? Object.assign(defaults, JSON.parse(raw)) : defaults;
    } catch (e) {
        // Private browsing, or storage turned off. The defaults are perfectly
        // usable and a page that refuses to load over a preference is not.
        return defaults;
    }
})();

function savePrefs() {
    try { localStorage.setItem(PREFS_KEY, JSON.stringify(prefs)); }
    catch (e) { /* see above */ }
}

// -------------------------------------------------------------- utilities

const $ = (sel) => document.querySelector(sel);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

function el(tag, className, html) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (html !== undefined) node.innerHTML = html;
    return node;
}

function icon(name, size) {
    const d = ICONS[name] || ICONS.wave;
    const px = size || 20;
    return '<svg viewBox="0 0 24 24" width="' + px + '" height="' + px + '" fill="none" ' +
           'stroke="currentColor" stroke-width="2" stroke-linecap="round" ' +
           'stroke-linejoin="round" aria-hidden="true"><path d="' + d + '"/></svg>';
}

// Ported from meterPosition() in src/ui/levelmeter.cpp. A linear bar is useless
// for audio: ordinary speech sits low enough that the meter looks broken, and
// scaling it up to compensate pegs it. Same -60 dB floor as the desktop, so the
// two agree about what "loud" looks like.
function meterPosition(amplitude) {
    if (!(amplitude > 0)) return 0;
    const db = 20 * Math.log10(amplitude);
    return clamp((db + 60) / 60, 0, 1);
}

const pct = (v) => Math.round(v * 100) + '%';

// Mute buttons swap their glyph rather than only their colour. Colour alone
// asks the user to remember which way round it is, and on a phone held at arm's
// length during a stream that is exactly the wrong thing to have to remember.
//
// Which pair of glyphs is the button's own business -- headphones on the Monitor
// column, the broadcast ring on Stream, the microphone on the third -- so it
// carries them, the way the desktop's IconToggle is built with both.
function muteButton(liveGlyph, mutedGlyph, tip) {
    const button = el('button', 'icon-btn', icon(liveGlyph, 17));
    button.type = 'button';
    button.dataset.live = liveGlyph;
    button.dataset.muted = mutedGlyph;
    button.dataset.glyph = liveGlyph;
    button.dataset.tip = tip || '';
    return button;
}

function setMuteButton(button, muted) {
    const want = muted ? button.dataset.muted : button.dataset.live;
    if (button.dataset.glyph !== want) {
        button.dataset.glyph = want;
        button.innerHTML = icon(want, 17);
    }
    button.classList.toggle('is-on', !!muted);
    const verb = muted ? 'Unmute' : 'Mute';
    button.title = button.dataset.tip ? verb + ' — ' + button.dataset.tip : verb;
}

// Same idea as the mute buttons: the glyph carries the state, not the colour
// alone. Crossed-out ear means you are not hearing yourself.
function setEarButton(button, on, input) {
    const want = on ? 'ear' : 'ear-off';
    if (button.dataset.glyph !== want) {
        button.dataset.glyph = want;
        button.innerHTML = icon(want, 17);
    }
    button.classList.toggle('is-on', !!on);
    button.title = input
        ? (on ? 'You are hearing this input device in the Monitor mix. Tap to stop.'
              : 'Hear this input device in the Monitor mix. Software only — it '
                + 'routes the microphone into your headphones.')
        : (on ? "You are hearing this channel's microphone in the Monitor mix. "
                + 'Tap to stop.'
              : "Hear this channel's published microphone in the Monitor mix.\n"
                + 'Software only: a channel microphone has no hardware monitor.');
}

// ------------------------------------------------------------------ Fader
//
// Hand-built rather than <input type="range">: the vertical case still needs a
// per-browser incantation, and every part of this one (bar handle, tinted fill,
// muted look) is painted from the card's own colour.

const KNOB_PX = 12;   // handle length along the axis; see .fader-knob

function Fader(orientation, onInput) {
    const vertical = orientation === 'v';
    const node = el('div', 'fader fader-' + (vertical ? 'v' : 'h'));
    node.innerHTML = '<div class="fader-track"><div class="fader-fill"></div></div>' +
                     '<div class="fader-knob"></div>';
    const fill = node.querySelector('.fader-fill');
    const knob = node.querySelector('.fader-knob');

    let value = 1;
    let dragging = false;
    let touchedAt = 0;
    let pending = null;
    let sendTimer = 0;

    function paint() {
        const v = value;
        // Travel is the track minus the handle, expressed against the element's
        // own size so it needs no measurement and survives a resize.
        const along = 'calc(' + (v * 100) + '% + ' + (KNOB_PX / 2 - KNOB_PX * v) + 'px)';
        const at = vertical
            ? 'calc(' + ((1 - v) * 100) + '% - ' + ((1 - v) * KNOB_PX) + 'px)'
            : 'calc(' + (v * 100) + '% - ' + (v * KNOB_PX) + 'px)';
        if (vertical) { fill.style.height = along; knob.style.top = at; }
        else { fill.style.width = along; knob.style.left = at; }
    }

    // Coalesced to ~25 Hz. A drag fires pointermove per frame, and each message
    // is a volume write plus a state document back out to every client.
    function emit() {
        pending = value;
        if (sendTimer) return;
        sendTimer = setTimeout(() => {
            sendTimer = 0;
            const v = pending;
            pending = null;
            onInput(v);
        }, 40);
    }

    function valueAt(ev) {
        const r = node.getBoundingClientRect();
        if (vertical) {
            const travel = Math.max(1, r.height - KNOB_PX);
            return 1 - clamp(ev.clientY - r.top - KNOB_PX / 2, 0, travel) / travel;
        }
        const travel = Math.max(1, r.width - KNOB_PX);
        return clamp(ev.clientX - r.left - KNOB_PX / 2, 0, travel) / travel;
    }

    function grab(ev) {
        if (node.dataset.disabled === '1') return;
        dragging = true;
        touchedAt = Date.now();
        node.setPointerCapture(ev.pointerId);
        value = valueAt(ev);
        paint();
        emit();
        ev.preventDefault();
    }

    function move(ev) {
        if (!dragging) return;
        value = valueAt(ev);
        touchedAt = Date.now();
        paint();
        emit();
    }

    function release() {
        if (!dragging) return;
        dragging = false;
        touchedAt = Date.now();
        // The coalescer may be holding the last position; make sure the value
        // the finger left the fader at is the one the daemon ends up with.
        if (sendTimer) { clearTimeout(sendTimer); sendTimer = 0; }
        onInput(value);
    }

    node.addEventListener('pointerdown', grab);
    node.addEventListener('pointermove', move);
    node.addEventListener('pointerup', release);
    node.addEventListener('pointercancel', release);

    paint();
    return {
        node,
        // Whatever this control mirrors, telling it so. The grace window is why
        // this is not a plain setter: the daemon's echo of a drag can still be
        // in flight when the finger lifts, and applying it would snap the fader
        // back to where it was two documents ago.
        setExternal(v) {
            if (dragging || Date.now() - touchedAt < 600) return;
            if (Math.abs(v - value) < 0.0005) return;
            value = v;
            paint();
        },
        // Dragged by its partner rather than by a finger or by the daemon. It
        // has to move now -- the whole point of the link is that both handles
        // are in the same place -- so it does not take the grace window's
        // advice, and it takes the touch stamp so the echo of its own write
        // cannot pull it back afterwards.
        setLinked(v) {
            if (dragging) return;
            value = v;
            touchedAt = Date.now();
            paint();
        },
        setMuted(muted) { node.classList.toggle('is-muted', !!muted); },
        setDisabled(off) { node.dataset.disabled = off ? '1' : '0'; node.style.opacity = off ? 0.45 : 1; },
        get value() { return value; },
    };
}

// ------------------------------------------------------------------ Meter

function Meter(orientation) {
    const vertical = orientation === 'v';
    const node = el('div', 'meter meter-' + (vertical ? 'v' : 'h'),
                    '<div class="meter-fill"></div>');
    const fill = node.querySelector('.meter-fill');
    let last = -1;
    return {
        node,
        set(v) {
            const t = clamp(v, 0, 1);
            if (Math.abs(t - last) < 0.004) return;
            last = t;
            fill.style.transform = (vertical ? 'scaleY(' : 'scaleX(') + t + ')';
        },
    };
}

// --------------------------------------------------------------- reconcile
//
// Rebuilding the strips on every document would lose focus, scroll position and
// any drag in progress. So elements are matched to items by a stable key, and
// only the values inside them are written.

function reconcile(container, items, keyFn, create) {
    const existing = new Map();
    for (const child of Array.from(container.children))
        existing.set(child.dataset.key, child);

    const ordered = [];
    items.forEach((item, i) => {
        const key = keyFn(item, i);
        let node = existing.get(key);
        if (node) existing.delete(key);
        else {
            node = create(item, i);
            node.dataset.key = key;
        }
        ordered.push(node);
        if (node.update) node.update(item, i);
    });

    for (const stale of existing.values()) stale.remove();
    ordered.forEach((node, i) => {
        if (container.children[i] !== node)
            container.insertBefore(node, container.children[i] || null);
    });
}

// ------------------------------------------------------------- connection

const conn = {
    socket: null,
    open: false,
    attempts: 0,
    timer: 0,
};

let state = null;
let levels = {};
// Soundboard progress rides the same 20 Hz document as the meters (see
// buildLevels() on the daemon side) but is kept apart from `levels` itself:
// every meter reader below indexes straight into `levels` by node id, and
// folding a nested object in beside those flat entries would be one more
// thing each of those lookups had to know to skip past.
let sbLevels = {};

function wsUrl() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return proto + '//' + location.host + '/ws';
}

function connect() {
    if (conn.timer) { clearTimeout(conn.timer); conn.timer = 0; }
    setConnection('wait', 'Connecting…');

    let socket;
    try {
        socket = new WebSocket(wsUrl());
    } catch (e) {
        scheduleReconnect();
        return;
    }
    conn.socket = socket;

    socket.onopen = () => {
        conn.open = true;
        conn.attempts = 0;
        setConnection('up', 'Connected');
        $('#veil').hidden = true;
    };

    socket.onmessage = (ev) => {
        let msg;
        try { msg = JSON.parse(ev.data); } catch (e) { return; }
        if (msg.t === 'state') { state = msg; renderState(); }
        else if (msg.t === 'levels') {
            levels = msg.peaks || {};
            sbLevels = msg.soundboard || {};
            renderLevels();
        }
    };

    socket.onclose = () => {
        conn.open = false;
        conn.socket = null;
        scheduleReconnect();
    };
    socket.onerror = () => { try { socket.close(); } catch (e) { /* onclose handles it */ } };
}

function scheduleReconnect() {
    setConnection('down', 'Disconnected');
    $('#veil').hidden = false;
    $('#veil-title').textContent = 'Reconnecting…';
    $('#veil-body').textContent =
        'The mixer is not answering. This page retries on its own.';
    // Backs off to two seconds and stays there. A tablet left on a shelf should
    // pick the daemon back up when it returns without having given up hours ago.
    const delay = Math.min(2000, 250 * Math.pow(2, Math.min(conn.attempts, 3)));
    conn.attempts += 1;
    conn.timer = setTimeout(connect, delay);
}

function setConnection(kind, text) {
    const dot = $('#conn-dot');
    dot.className = 'dot is-' + kind;
    $('#conn-text').textContent = text;
    $('#conn-host').textContent = location.host;
}

function send(msg) {
    if (!conn.open || !conn.socket) return;
    conn.socket.send(JSON.stringify(msg));
}

// ------------------------------------------------------------------ looks

function lookFor(key) {
    return (state && state.looks && state.looks[key]) || null;
}

function inputColor(id, index) {
    const look = lookFor('master:' + id);
    if (look && look.color) return look.color;
    return BUS_PALETTE[index % BUS_PALETTE.length];
}

function inputIcon(id, busType) {
    const look = lookFor('master:' + id);
    if (look && look.icon) return look.icon;
    return busType === 'midi' ? 'piano' : 'microphone';
}

function channelColor(id) {
    const look = lookFor('channel:' + id);
    if (look && look.color) return look.color;
    return CHANNEL_COLORS[id] || '#6a6a76';
}

function channelIcon(id) {
    const look = lookFor('channel:' + id);
    if (look && look.icon) return look.icon;
    return CHANNEL_ICONS[id] || 'speaker';
}

// ------------------------------------------------------------------ strip
//
// One card, whether it is an input device or a channel. The two differ in what
// their controls call and in one extra button, which is not enough difference
// to justify two of these.

// The microphone column says something different on each kind of card, and the
// difference matters enough to spell out in the tooltip: on an input device it
// is the device's own capture level, which everything recording from it hears,
// and on a channel it is the gain of the microphone that channel publishes.
const MIC_TIPS = {
    input: 'the microphone itself — the level your system sound settings show, ' +
           'so everything recording from this device is affected, not just the mixer',
    channel: "this channel's own published microphone — what an application " +
             'recording from it hears',
};

function buildStrip(kind) {
    const node = el('div', 'strip');

    const head = el('div', 'flex items-center gap-2');
    const tile = el('div', 'tile');
    const names = el('div', 'min-w-0 flex-1');
    const title = el('div', 'text-sm font-semibold truncate');
    const sub = el('div', 'text-[10px] text-faint truncate');
    names.append(title, sub);

    // Beside the name, as on the desktop: whether this card's microphone is in
    // the Monitor mix -- that is, whether you hear yourself. Software only.
    // Hidden rather than greyed on a channel with no microphone, because there
    // is no such thing to hear and the desktop hides it too.
    const ear = el('button', 'icon-btn ear-btn', icon('ear-off', 17));
    ear.type = 'button';
    ear.dataset.glyph = 'ear-off';

    head.append(tile, names, ear);

    // Above the faders, centred over the card, the way the desktop places it.
    // Switching it on moves nothing by itself -- see setLinked() -- so it can
    // never silently change what an audience is hearing.
    const linkRow = el('div', 'link-row');
    const link = el('button', 'link-btn', icon('equal', 17));
    link.type = 'button';
    link.title = 'Keep the Monitor and Stream faders equal: move either one and ' +
                 'the other follows. Switching it on does not move anything by ' +
                 'itself — they match from the next time you touch one.';
    linkRow.append(link);

    // flex-1 + min-h-0: the body takes whatever the strip has left after the
    // heading and the FX button, and the faders inside it stretch to fill it.
    // A fixed fader height was what left dead space under the cards.
    const body = el('div', 'strip-body flex items-stretch flex-1 min-h-0');
    // Two meters, flanking the faders, which is the desktop's pair of bars
    // turned on their side. Left is what you hear from this card; right is its
    // microphone, before any mix gets hold of it.
    const meter = Meter('v');
    body.append(meter.node);

    function faderColumn(liveGlyph, mutedGlyph, tip) {
        const col = el('div', 'mix-col');
        const fader = Fader('v', () => {});
        const readout = el('div', 'text-[10px] text-dim tabular-nums', '100%');
        const mute = muteButton(liveGlyph, mutedGlyph, tip);
        col.append(fader.node, readout, mute);
        return { col, fader, readout, mute };
    }

    const monitor = faderColumn('headphones', 'headphones-off',
                                'the Monitor mix, which is what you hear. Your ' +
                                'audience is unaffected.');
    const stream = faderColumn('stream', 'stream-off',
                               'the Stream mix, which is what your audience ' +
                               'hears. You still hear it.');
    const mic = faderColumn('microphone', 'microphone-off', MIC_TIPS[kind]);
    const micMeter = Meter('v');
    micMeter.node.classList.add('meter-mic');
    body.append(monitor.col, stream.col, mic.col, micMeter.node);

    const fxRow = el('div', 'fx-row');
    const fx = el('button', 'fx-btn', icon('heartbeat', 18));
    fx.type = 'button';
    fxRow.append(fx);

    node.append(head, linkRow, body, fxRow);
    node.refs = { tile, title, sub, ear, link, meter, micMeter,
                  monitor, stream, mic, fx };
    return node;
}

function applyFxLook(button, mode) {
    button.classList.toggle('is-on', mode !== 'off');
    button.classList.toggle('is-monitor', mode === 'monitor');
    button.title = mode === 'off'
        ? 'Effects bypassed. Tap to switch them on.'
        : mode === 'on'
            ? 'Effects on. Tap to hear them in the Monitor mix as well.'
            : 'Effects on and audible in the Monitor mix. Tap to switch them off.';
}

// Forward only. The desktop cycles backwards on right-click, which a tablet has
// no equivalent of, and three states are short enough to walk around.
function nextFx(mode) {
    return mode === 'off' ? 'on' : mode === 'on' ? 'monitor' : 'off';
}

// The divider between the two halves of the row. Nothing to update: it says
// the same thing whatever the mixer is doing.
function buildDivider() {
    const node = el('div', 'rail-divider');
    const top = el('div', 'grp');
    top.append(el('span', 'vcap is-up', 'Input Devices'));
    top.insertAdjacentHTML('beforeend',
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ' +
        'stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M5 12l6 6M5 12l6-6"/></svg>');
    const bottom = el('div', 'grp');
    bottom.insertAdjacentHTML('beforeend',
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ' +
        'stroke-linecap="round" stroke-linejoin="round"><path d="M19 12H5M19 12l-6 6M19 12l-6-6"/></svg>');
    bottom.append(el('span', 'vcap', 'Channels'));
    node.append(top, bottom);
    return node;
}

// Every strip in the row, by key, so the meters can be updated without walking
// the DOM twenty times a second.
const stripNodes = new Map();

// One reconcile over the whole row. Two would mean two containers, and the
// point of the divider is that there is only one.
function renderRail() {
    const items = state.inputs.map((data, index) => ({ kind: 'input', id: data.id, data, index }))
        .concat([{ kind: 'divider', id: '-' }])
        .concat(state.channels.map((data) => ({ kind: 'channel', id: data.id, data, index: 0 })));

    stripNodes.clear();
    reconcile($('#rail'), items, (it) => it.kind + ':' + it.id, (it) => {
        if (it.kind === 'divider') return buildDivider();
        const input = it.kind === 'input';
        const node = buildStrip(it.kind);
        const r = node.refs;
        let id = it.id;

        const volumeCmd = input ? 'input.volume' : 'channel.volume';
        const muteCmd = input ? 'input.mute' : 'channel.mute';
        const fxCmd = input ? 'input.fx' : 'channel.fx';
        // The third fader is the same idea on both kinds of card and a
        // different control underneath: an input device's is its own capture
        // level, a channel's is the gain of the microphone it publishes.
        const micVolumeCmd = input ? 'input.inputVolume' : 'channel.micSend';
        const micMuteCmd = input ? 'input.gate' : 'channel.micMute';

        const linkKey = it.kind + ':' + it.id;
        let linked = !!prefs.links[linkKey];

        // Equalising, not offsetting: the "=" on the button is what it does.
        function moveMix(mix, v) {
            send({ cmd: volumeCmd, id: id, mix: mix, value: v });
            if (!linked) return;
            const other = mix === 'monitor' ? r.stream : r.monitor;
            if (Math.abs(other.fader.value - v) < 0.0005) return;
            other.fader.setLinked(v);
            other.readout.textContent = pct(v);
            send({
                cmd: volumeCmd, id: id,
                mix: mix === 'monitor' ? 'stream' : 'monitor', value: v,
            });
        }

        r.monitor.fader = replaceFader(r.monitor, 'monitor', (v) => moveMix('monitor', v));
        r.stream.fader = replaceFader(r.stream, 'stream', (v) => moveMix('stream', v));
        r.mic.fader = replaceFader(r.mic, 'mic', (v) =>
            send({ cmd: micVolumeCmd, id: id, value: v }));
        r.monitor.mute.onclick = () => send({
            cmd: muteCmd, id: id, mix: 'monitor',
            on: !r.monitor.mute.classList.contains('is-on'),
        });
        r.stream.mute.onclick = () => send({
            cmd: muteCmd, id: id, mix: 'stream',
            on: !r.stream.mute.classList.contains('is-on'),
        });
        r.mic.mute.onclick = () => send({
            cmd: micMuteCmd, id: id,
            on: !r.mic.mute.classList.contains('is-on'),
        });
        r.ear.onclick = () => send({
            cmd: input ? 'input.monitor' : 'channel.micMonitor', id: id,
            on: !r.ear.classList.contains('is-on'),
        });

        // Nothing to echo: the daemon has no opinion about this, and applying
        // it later would be the one behaviour the tooltip promises it does not
        // have -- moving a fader on its own.
        r.link.classList.toggle('is-on', linked);
        r.link.onclick = () => {
            linked = !linked;
            prefs.links[linkKey] = linked;
            savePrefs();
            r.link.classList.toggle('is-on', linked);
        };

        r.fx.onclick = () => {
            const mode = nextFx(node.dataset.fx || 'off');
            node.dataset.fx = mode;
            applyFxLook(r.fx, mode);   // optimistic: the echo is a round trip away
            send({ cmd: fxCmd, id: id, mode: mode });
        };

        node.update = (item) => {
            const d = item.data;
            id = d.id;
            // What the microphone column is showing, and whether it reaches
            // anything at all. An unplugged device has no capture level to set,
            // and a channel publishes no microphone until one is turned on for
            // it in the effects panel -- in both cases the column is greyed
            // rather than hidden, so the card keeps its shape and the row does
            // not reflow the moment a microphone appears.
            let micLive;
            let micVolume;
            let micMuted;
            let monitoring;
            if (input) {
                node.style.setProperty('--tint', inputColor(d.id, item.index));
                r.tile.innerHTML = icon(inputIcon(d.id, d.busType));
                r.sub.textContent = d.connected
                    ? (d.deviceLabel || (d.busType === 'midi' ? 'MIDI' : 'Input device'))
                    : 'Not connected';
                node.classList.toggle('is-offline', !d.connected);
                micLive = d.connected;
                micVolume = d.inputVolume || 0;
                micMuted = d.inputMuted;
                monitoring = !!d.monitoring;
            } else {
                node.style.setProperty('--tint', channelColor(d.id));
                r.tile.innerHTML = icon(channelIcon(d.id));
                r.sub.textContent = 'Channel';
                micLive = !!d.micSource;
                micVolume = d.micSend || 0;
                micMuted = d.micMuted;
                monitoring = !!d.micMonitor;
            }
            r.title.textContent = d.name || d.id;

            r.meter.node.title = input
                ? (monitoring
                    ? (d.monitorMuted
                        ? 'Muted in the Monitor mix, so there is nothing to hear '
                          + 'from this input device. Your audience is unaffected.'
                        : 'What you hear from this input device: its send into '
                          + "the Monitor mix, at this card's Monitor fader.")
                    : 'Not monitored, so there is nothing to hear from this '
                      + 'input device. Tap the ear to listen.')
                : 'Signal reaching this channel.';

            r.monitor.fader.setExternal(d.monitorVolume);
            r.stream.fader.setExternal(d.streamVolume);
            r.monitor.fader.setMuted(d.monitorMuted);
            r.stream.fader.setMuted(d.streamMuted);
            r.monitor.readout.textContent = pct(d.monitorVolume);
            r.stream.readout.textContent = pct(d.streamVolume);
            setMuteButton(r.monitor.mute, d.monitorMuted);
            setMuteButton(r.stream.mute, d.streamMuted);

            r.mic.col.classList.toggle('is-inert', !micLive);
            r.mic.col.title = micLive
                ? ''
                : input
                    ? 'This device is not connected.'
                    : 'This channel publishes no microphone. Turn one on in the ' +
                      'desktop mixer’s effects panel.';
            r.mic.fader.setExternal(micVolume);
            r.mic.fader.setMuted(micMuted);
            r.mic.readout.textContent = pct(micVolume);
            setMuteButton(r.mic.mute, micMuted);

            // An input device always has a microphone to hear, so its ear is
            // always there. A channel's appears with the microphone it
            // publishes and goes with it.
            r.ear.hidden = !input && !micLive;
            setEarButton(r.ear, monitoring, input);
            r.micMeter.node.classList.toggle('is-inert', !micLive);
            r.micMeter.node.title = micLive
                ? (input
                    ? 'This microphone, after noise suppression and EQ — what ' +
                      'the mixes receive.'
                    : "This channel's microphone, after its noise suppression " +
                      'and EQ.')
                : r.mic.col.title;
            if (!micLive) r.micMeter.set(0);

            // The optimistic look above stands until the daemon agrees; once it
            // does, the daemon is authoritative again.
            node.dataset.fx = d.fx;
            applyFxLook(r.fx, d.fx);
        };
        return node;
    });

    for (const it of items) {
        if (it.kind === 'divider') continue;
        const node = $('#rail').querySelector('[data-key="' + it.kind + ':' + CSS.escape(it.id) + '"]');
        if (node) stripNodes.set(it.kind + ':' + it.id, node);
    }
    railView.sync();
    // Again after layout. The rail's own box does not change when the strips
    // inside it do, so the ResizeObserver never fires for a card being added --
    // and sync() called synchronously here measures the row as it was before
    // the new one was laid out.
    requestAnimationFrame(() => railView.sync());
}

// buildStrip() cannot know what a fader will send -- the card it belongs to has
// not been identified yet -- so the placeholder it creates is swapped for one
// wired to the right channel here.
function replaceFader(column, kind, onInput) {
    const fader = Fader('v', onInput);
    column.fader.node.replaceWith(fader.node);
    column.col.dataset.mix = kind;
    return fader;
}


// --------------------------------------------------------------- rail view
//
// The one row: input devices, the divider, then channels. It scrolls sideways
// by swipe always, and -- when Desktop Controls is on -- by a scrollbar under
// it as well. Both drive the same scrollLeft, and either one moving updates
// the other.

// A real scrollbar rather than a slider. The thumb's *width* is the share of
// the row that fits on screen, which is the whole point: it says how much there
// is left to reach, which a fixed handle on a track cannot.
function makeScrollbar(rail, host) {
    const track = el('div', 'scrollbar');
    const thumb = el('div', 'scrollbar-thumb');
    track.append(thumb);
    host.append(track);

    // Below this a thumb is not draggable with a finger, however little of the
    // row is on screen.
    const MIN_THUMB = 40;
    let dragging = false;
    let grabDx = 0;   // where in the thumb the finger landed

    function metrics() {
        const trackW = track.clientWidth;
        const maxScroll = Math.max(0, rail.scrollWidth - rail.clientWidth);
        const visible = rail.scrollWidth > 0 ? rail.clientWidth / rail.scrollWidth : 1;
        const thumbW = clamp(Math.round(visible * trackW), MIN_THUMB, trackW);
        return { trackW, maxScroll, thumbW, travel: Math.max(1, trackW - thumbW) };
    }

    function paint() {
        const m = metrics();
        thumb.style.width = m.thumbW + 'px';
        const at = m.maxScroll > 0 ? (rail.scrollLeft / m.maxScroll) * m.travel : 0;
        thumb.style.transform = 'translateX(' + at + 'px)';
    }

    function scrollToThumbLeft(left) {
        const m = metrics();
        rail.scrollLeft = (clamp(left, 0, m.travel) / m.travel) * m.maxScroll;
    }

    track.addEventListener('pointerdown', (ev) => {
        const t = thumb.getBoundingClientRect();
        const r = track.getBoundingClientRect();
        const onThumb = ev.clientX >= t.left && ev.clientX <= t.right;
        // Landing on the track jumps the thumb under the finger and then drags
        // from there, so a tap ahead of the thumb goes where it was aimed.
        grabDx = onThumb ? ev.clientX - t.left : metrics().thumbW / 2;
        dragging = true;
        track.classList.add('is-dragging');
        rail.classList.add('is-scrubbing');
        track.setPointerCapture(ev.pointerId);
        scrollToThumbLeft(ev.clientX - r.left - grabDx);
        paint();
        ev.preventDefault();
    });

    track.addEventListener('pointermove', (ev) => {
        if (!dragging) return;
        const r = track.getBoundingClientRect();
        scrollToThumbLeft(ev.clientX - r.left - grabDx);
        paint();
    });

    function release() {
        if (!dragging) return;
        dragging = false;
        track.classList.remove('is-dragging');
        // Snap comes back here, which is also when a released row should settle
        // onto a card rather than stopping between two.
        rail.classList.remove('is-scrubbing');
    }
    track.addEventListener('pointerup', release);
    track.addEventListener('pointercancel', release);

    return { paint, isDragging: () => dragging };
}

function makeRailView(rail, scrollHost) {
    const bar = makeScrollbar(rail, scrollHost);

    const view = {
        sync() {
            const scrollable = rail.scrollWidth - rail.clientWidth > 8;
            const show = scrollable && prefs.easyScroll;
            scrollHost.hidden = !show;
            if (show) bar.paint();
        },
    };

    // Recomputed from the live scroll position rather than tracked in a
    // variable: momentum scrolling on a phone does not report where it stopped.
    rail.addEventListener('scroll', () => view.sync(), { passive: true });
    // A rotated tablet changes how many cards fit, which changes whether there
    // is anything left to scroll to at all.
    if (window.ResizeObserver) new ResizeObserver(() => view.sync()).observe(rail);
    return view;
}

const railView = makeRailView($('#rail'), $('#rail-scroll'));

// ------------------------------------------------- application settings

function switchRow(label, help, onToggle) {
    const row = el('div', 'row');
    const text = el('div', 'min-w-0 flex-1');
    const name = el('div', 'text-sm font-medium', label);
    const hint = el('div', 'text-xs text-faint', help);
    text.append(name, hint);
    const sw = el('button', 'switch');
    sw.type = 'button';
    sw.onclick = () => onToggle(!sw.classList.contains('is-on'));
    row.append(text, sw);
    row.setOn = (on) => sw.classList.toggle('is-on', !!on);
    return row;
}

let deviceSettingsBuilt = false;

// Set from the Reduce Motion switch below. The bands read it when they fold, so
// turning it on takes effect on the very next tap rather than at the next load.
function applyMotionPref() {
    document.body.classList.toggle('no-motion', !!prefs.reduceMotion);
}

// Set from the Desktop Controls switch below (see its own description for
// why the dots belong to the same setting as the mixer row's scrollbar):
// both are mouse/trackpad-friendly stand-ins for a gesture a touchscreen
// does not need spelling out. A phone leaves this off and swipes the header
// with nothing to tap sitting in the way of it.
function applyDesktopControlsPref() {
    $('#view-dots').hidden = !prefs.easyScroll;
}

// This device's own preferences. Nothing here is sent anywhere, so it is drawn
// once and never refreshed: no push can change it.
function renderDeviceSettings() {
    if (deviceSettingsBuilt) return;
    deviceSettingsBuilt = true;

    const scroll = switchRow(
        'Desktop Controls',
        'Adds a scrollbar under the mixer row and a Mixer/Soundboard dot ' +
        'you can tap instead of swiping the header. Touch gestures still ' +
        'work either way — these are second ways to do the same things, ' +
        'for a mouse or trackpad rather than a finger.',
        (on) => {
            prefs.easyScroll = on;
            savePrefs();
            scroll.setOn(on);
            railView.sync();
            applyDesktopControlsPref();
        });
    scroll.setOn(prefs.easyScroll);

    // An older tablet can spend long enough laying the mixer out that the fold
    // animation drops to a few frames, which reads worse than no animation at
    // all -- and there is nothing to be learnt from watching it, since the tab
    // that did it is still under the finger.
    const motion = switchRow(
        'Reduce Motion',
        'Hides and shows the header and the output mixes instantly, with no ' +
        'sliding. Worth turning on if the fold stutters on this device.',
        (on) => {
            prefs.reduceMotion = on;
            savePrefs();
            motion.setOn(on);
            applyMotionPref();
        });
    motion.setOn(prefs.reduceMotion);

    $('#device-settings').append(scroll, motion);
}

let settingsBuilt = false;
let routingRow = null;
let sharingRow = null;

function renderAppSettings() {
    // Nothing is visible, so nothing needs drawing. The state document lands
    // several times a second and rebuilding a hidden application list on each
    // one is work nobody sees -- which is now also true whenever the sheet is
    // open on the other tab.
    if ($('#settings-modal').hidden || $('#app-settings').hidden) return;

    const host = $('#app-settings');
    if (!settingsBuilt) {
        settingsBuilt = true;
        routingRow = switchRow(
            'Auto Routing',
            'Send each application to the channel it belongs to.',
            (on) => send({ cmd: 'routing.enabled', on: on }));
        sharingRow = switchRow(
            'Audio Sharing',
            'Let chosen applications join a microphone, so others hear them.',
            (on) => send({ cmd: 'sharing.enabled', on: on }));
        const list = el('div', 'flex flex-col gap-2');
        list.id = 'app-list';
        host.append(routingRow, sharingRow, list);
    }
    routingRow.setOn(state.routing);
    sharingRow.setOn(state.sharing);

    const list = $('#app-list');
    if (!state.apps.length) {
        if (!list.dataset.empty) {
            list.innerHTML = '';
            const empty = el('div', 'text-xs text-faint px-2 py-3',
                             'Nothing is playing audio right now.');
            empty.dataset.key = '__empty';
            list.append(empty);
            list.dataset.empty = '1';
        }
        return;
    }
    if (list.dataset.empty) { list.innerHTML = ''; delete list.dataset.empty; }

    reconcile(list, state.apps, (app) => 'app:' + app.nodeId, (app) => {
        const node = el('div', 'row');
        node.style.flexWrap = 'wrap';
        let nodeId = app.nodeId;

        const name = el('div', 'text-sm font-medium truncate min-w-0 flex-1');
        // The daemon takes 0..1.5 (100% is unity, with headroom above it for a
        // quiet application); the fader's own travel is 0..1 of that range.
        const volume = Fader('h', (v) =>
            send({ cmd: 'app.volume', nodeId: nodeId, value: v * 1.5 }));
        const volumeLabel = el('div', 'text-[10px] text-faint tabular-nums', '100%');

        const channel = el('select');
        channel.onchange = () =>
            send({ cmd: 'app.channel', nodeId: nodeId, channel: channel.value });

        const share = el('select');
        share.onchange = () =>
            send({ cmd: 'app.share', nodeId: nodeId, target: share.value });

        const top = el('div', 'flex items-center gap-2 w-full');
        top.append(name, volumeLabel);
        const mid = el('div', 'flex items-center gap-2 w-full');
        mid.append(volume.node);
        const bottom = el('div', 'flex items-center gap-2 w-full');
        const chLabel = el('div', 'text-[10px] text-faint', 'Channel');
        const shLabel = el('div', 'text-[10px] text-faint', 'Share to');
        const chWrap = el('div', 'flex flex-col gap-1 flex-1 min-w-0');
        chWrap.append(chLabel, channel);
        const shWrap = el('div', 'flex flex-col gap-1 flex-1 min-w-0');
        shWrap.append(shLabel, share);
        bottom.append(chWrap, shWrap);
        node.append(top, mid, bottom);

        node.update = (item) => {
            nodeId = item.nodeId;
            name.textContent = item.name;
            volume.setExternal(clamp(item.volume / 1.5, 0, 1));
            volumeLabel.textContent = pct(item.volume);

            syncOptions(channel, [{ value: '', label: 'Unassigned' }].concat(
                state.channels.map((c) => ({ value: c.id, label: c.name || c.id }))),
                item.channel || '');
            syncOptions(share, [{ value: '', label: 'Not shared' }].concat(
                state.shareTargets.map((t) => ({ value: t.id, label: t.label }))),
                item.shareTarget || '');
            share.disabled = !state.sharing;
            share.style.opacity = state.sharing ? 1 : 0.5;
        };
        return node;
    });
}

// Rewrites a <select> only when its options actually changed. Rebuilding it
// every document would close the native picker on a phone the instant it opened.
function syncOptions(select, options, value) {
    const sig = options.map((o) => o.value + '' + o.label).join('');
    if (select.dataset.sig !== sig) {
        select.dataset.sig = sig;
        select.innerHTML = '';
        for (const o of options) {
            const opt = document.createElement('option');
            opt.value = o.value;
            opt.textContent = o.label;
            select.append(opt);
        }
    }
    if (document.activeElement !== select && select.value !== value)
        select.value = value;
}

// ----------------------------------------------------------------- footer
//
// The mixes, which is what a person watches while everything else is set. Every
// monitor output gets its own row because each has its own level and mute; which
// device it plays through is not settable from here on purpose -- picking an
// output is a setup decision, and the tablet is for performance.

function outputRow(iconName) {
    const node = el('div', 'out-row');
    const tile = el('div', 'text-dim flex-none', icon(iconName, 18));
    const name = el('div', 'out-name min-w-0');
    const title = el('div', 'text-sm font-medium truncate');
    const sub = el('div', 'text-[10px] text-faint truncate');
    name.append(title, sub);
    const meter = Meter('h');
    const fader = Fader('h', () => {});
    const readout = el('div', 'text-[10px] text-dim tabular-nums flex-none',
                       '100%');
    readout.style.width = '34px';
    readout.style.textAlign = 'right';
    const mute = muteButton('speaker', 'speaker-muted', 'this output');
    node.append(tile, name, meter.node, fader.node, readout, mute);
    node.refs = { title, sub, meter, fader, readout, mute };
    return node;
}

function renderFooter() {
    const host = $('#out-rows');
    const monitors = state.outputs.monitor;
    const rows = monitors.map((m, i) => ({ kind: 'monitor', index: i, data: m }))
        .concat([{ kind: 'stream', index: 0, data: state.outputs.stream }]);

    reconcile(host, rows, (row) => row.kind + ':' + row.index, (row) => {
        const node = outputRow(row.kind === 'stream' ? 'stream' : 'headphones');
        const r = node.refs;
        const index = row.index;
        const kind = row.kind;

        const fader = Fader('h', (v) => send({
            cmd: kind === 'stream' ? 'streamMix.volume' : 'monitorOut.volume',
            index: index, value: v,
        }));
        r.fader.node.replaceWith(fader.node);
        r.fader = fader;

        r.mute.onclick = () => send({
            cmd: kind === 'stream' ? 'streamMix.mute' : 'monitorOut.mute',
            index: index, on: !r.mute.classList.contains('is-on'),
        });

        node.update = (item) => {
            const d = item.data;
            const total = state.outputs.monitor.length;
            if (item.kind === 'stream') {
                node.style.setProperty('--tint', '#3dd68c');
                r.title.textContent = 'Stream mix';
                r.sub.textContent = (state.brand || 'Waveline') + ' Stream Mix';
            } else {
                node.style.setProperty('--tint', '#5b6cf0');
                r.title.textContent =
                    total <= 1 ? 'Monitor mix' : 'Monitor mix #' + (item.index + 1);
                r.sub.textContent = d.connected ? d.description : d.description + ' — offline';
                node.classList.toggle('is-offline', !d.connected);
            }
            r.fader.setExternal(d.volume);
            r.fader.setMuted(d.muted);
            r.readout.textContent = pct(d.volume);
            setMuteButton(r.mute, d.muted);
        };
        return node;
    });
}

// ------------------------------------------------------- view switching
//
// Two views, Mixer and Soundboard, swiped between on the header -- the one
// band that stays on screen regardless of which view is showing, so it is
// the one surface a gesture to switch views can always reach. #view-track is
// the thing that actually moves; its own CSS transition (suspended only by
// .is-dragging, for the span of an active finger-drag -- see app.css) is
// what animates the slide, deliberately kept even when Reduce Motion is on:
// this is how you get to the Soundboard at all, not decoration on the way.

const viewTrack = $('#view-track');
const header = $('#header');
let currentView = 'mixer'; // 'mixer' | 'soundboard'

function setView(view) {
    currentView = view;
    viewTrack.classList.remove('is-dragging');
    viewTrack.style.transform = 'translateX(' + (view === 'soundboard' ? '-50%' : '0%') + ')';
    for (const dot of document.querySelectorAll('#view-dots .vdot')) {
        const on = dot.dataset.view === view;
        dot.classList.toggle('is-active', on);
        dot.setAttribute('aria-selected', on ? 'true' : 'false');
    }
}

for (const dot of document.querySelectorAll('#view-dots .vdot'))
    dot.onclick = () => setView(dot.dataset.view);

(() => {
    let active = false;
    let pointerId = null;
    let startX = 0;
    let startY = 0;
    let dx = 0;
    let decided = null; // null | 'h' | 'v' -- sticks once the gesture picks a direction
    // A light low-pass filter over recent pointermove samples, in px/ms.
    // A single sample is noisy enough (touch input jitters between events)
    // that a flick judged on the last delta alone reads as inconsistent --
    // smoothing a few samples together is what makes "flick a bit" reliable.
    let velocity = 0;
    let lastX = 0;
    let lastT = 0;

    function trackHalfWidth() { return viewTrack.getBoundingClientRect().width / 2; }

    header.addEventListener('pointerdown', (ev) => {
        // A drag that starts on one of the header's own buttons must not
        // also register as the first pixel of a swipe.
        if (ev.target.closest('button')) return;
        active = true;
        pointerId = ev.pointerId;
        decided = null;
        startX = ev.clientX;
        startY = ev.clientY;
        dx = 0;
        velocity = 0;
        lastX = ev.clientX;
        lastT = ev.timeStamp;
    });

    header.addEventListener('pointermove', (ev) => {
        if (!active || ev.pointerId !== pointerId) return;
        const moveX = ev.clientX - startX;
        const moveY = ev.clientY - startY;
        if (!decided) {
            if (Math.abs(moveX) < 8 && Math.abs(moveY) < 8) return;
            decided = Math.abs(moveX) > Math.abs(moveY) ? 'h' : 'v';
            if (decided === 'h') {
                header.setPointerCapture(ev.pointerId);
                viewTrack.classList.add('is-dragging');
            }
        }
        if (decided !== 'h') return;
        dx = moveX;
        const dt = ev.timeStamp - lastT;
        if (dt > 0) {
            const instant = (ev.clientX - lastX) / dt;
            velocity = velocity * 0.7 + instant * 0.3;
        }
        lastX = ev.clientX;
        lastT = ev.timeStamp;
        // Rubber band past either end -- there are only two views, and
        // dragging past the second one should feel like a wall, not like
        // there is a third view somewhere past it.
        const w = trackHalfWidth();
        const base = currentView === 'soundboard' ? -50 : 0;
        let pct = base + (dx / w) * 50;
        if (pct > 0) pct *= 0.35;
        if (pct < -50) pct = -50 + (pct + 50) * 0.35;
        viewTrack.style.transform = 'translateX(' + pct + '%)';
    });

    // Below this speed a short drag is just a short drag, not a flick --
    // ~350px in a second, which is a brisk finger flick and nowhere near an
    // accidental brush of the header.
    const FLICK_SPEED = 0.35; // px/ms

    function release(ev) {
        if (!active || ev.pointerId !== pointerId) return;
        active = false;
        pointerId = null;
        if (decided !== 'h') return;
        const w = trackHalfWidth();
        const distanceThreshold = w * 0.22;
        let next = currentView;
        if (Math.abs(velocity) > FLICK_SPEED) {
            // A flick commits in whichever direction it was moving, however
            // little ground it actually covered -- that is the whole point
            // of "flick a bit" over dragging most of the way across.
            next = velocity < 0 ? 'soundboard' : 'mixer';
        } else if (dx < -distanceThreshold) {
            next = 'soundboard';
        } else if (dx > distanceThreshold) {
            next = 'mixer';
        }
        setView(next);
    }
    header.addEventListener('pointerup', release);
    header.addEventListener('pointercancel', release);
})();

// ------------------------------------------------------------ soundboard
//
// Play and reorder only -- no add, no edit, no delete; see the comment on
// #view-soundboard in index.html for why. Sounds arrive in
// state.soundboard.sounds, already in play/display order (the same order
// soundboard.reorder writes back), and progress for whichever ones are
// currently playing arrives on the fast 20 Hz channel, in sbLevels.

const sbPagesEl = $('#sb-pages');
const sbEmptyEl = $('#sb-empty');
const sbDotsEl = $('#sb-dots');

// A pad's target cell, in CSS px. Not exact -- the grid stretches every pad
// to fill its row and column evenly -- just what decides how many fit on a
// page. 128x64 was the ask; the slack either side is what "can adapt" meant.
const SB_CELL_W = 128;
const SB_CELL_H = 64;
const SB_GAP = 10;

let sbCols = 1;
let sbRows = 1;
let sbPerPage = 1;
// The order this device is showing right now. Mirrors the daemon's except
// mid-drag, where the array the finger is building is the one that must not
// be clobbered by a state document landing underneath it -- the same rule
// every other control on this page follows for a value it currently owns.
let sbOrder = [];
// id -> {id, name, durationMs}, so a reorder (which only ever moves ids
// around) never has to wait on the next state document to know what to draw.
let sbInfo = new Map();
let sbPage = 0;
let sbDragging = false;
let sbPlaying = new Set();

function sbMeasure() {
    const r = sbPagesEl.getBoundingClientRect();
    sbCols = Math.max(1, Math.floor((r.width + SB_GAP) / (SB_CELL_W + SB_GAP)));
    sbRows = Math.max(1, Math.floor((r.height + SB_GAP) / (SB_CELL_H + SB_GAP)));
    sbPerPage = sbCols * sbRows;
}

function sbPageCount() {
    return sbOrder.length ? Math.max(1, Math.ceil(sbOrder.length / sbPerPage)) : 0;
}

function sbToggle(id) {
    send({ cmd: sbPlaying.has(id) ? 'soundboard.stop' : 'soundboard.play', id: id });
}

function sbSendReorder() {
    send({ cmd: 'soundboard.reorder', order: sbOrder.slice() });
}

function sbGoToPage(p) {
    const page = sbPagesEl.children[p];
    if (!page) return;
    sbPagesEl.scrollTo({ left: page.offsetLeft, behavior: prefs.reduceMotion ? 'auto' : 'smooth' });
}

function sbBuildDots(pageCount) {
    sbDotsEl.hidden = pageCount <= 1;
    sbDotsEl.innerHTML = '';
    for (let p = 0; p < pageCount; p++) {
        const dot = el('button', 'vdot' + (p === sbPage ? ' is-active' : ''));
        dot.type = 'button';
        dot.setAttribute('aria-label', 'Page ' + (p + 1));
        dot.onclick = () => sbGoToPage(p);
        sbDotsEl.append(dot);
    }
}

function sbApplyPlayingLooks() {
    const data = sbLevels || {};
    for (const pad of sbPagesEl.querySelectorAll('.sb-pad')) {
        const id = pad.dataset.id;
        const playing = sbPlaying.has(id);
        pad.classList.toggle('is-playing', playing);
        if (playing) {
            const rect = pad.querySelector('.pad-ring rect');
            if (rect) rect.style.strokeDashoffset = String(1 - clamp(data[id] || 0, 0, 1));
        }
    }
}

// Long-press-activated drag, the iOS homescreen gesture: hold a pad and it
// lifts into a floating avatar that follows the finger 1:1, while the
// original -- invisible but still occupying its grid cell, so nothing else
// jumps around it -- is the thing actually moved through the DOM as other
// pads are hovered, so the grid it belongs to is always the live layout.
function sbWirePad(btn) {
    let pressTimer = 0;
    let pointerId = null;
    let startX = 0;
    let startY = 0;
    let longPressed = false;
    let moved = false;
    let clone = null;
    let offsetX = 0;
    let offsetY = 0;
    let edgeTimer = 0;

    function cancelPress() {
        if (pressTimer) { clearTimeout(pressTimer); pressTimer = 0; }
    }

    function startDrag(ev) {
        longPressed = true;
        const r = btn.getBoundingClientRect();
        offsetX = startX - r.left;
        offsetY = startY - r.top;

        clone = btn.cloneNode(true);
        clone.classList.add('is-dragging');
        clone.style.left = r.left + 'px';
        clone.style.top = r.top + 'px';
        clone.style.width = r.width + 'px';
        clone.style.height = r.height + 'px';
        clone.style.pointerEvents = 'none';
        document.body.append(clone);

        btn.classList.add('is-drag-source');
        sbDragging = true;
        sbPagesEl.classList.add('is-reordering');
        if (navigator.vibrate) navigator.vibrate(12);

        // Listening on document, rather than on btn or via setPointerCapture,
        // is deliberate: btn is the thing physically relocated through the
        // DOM on every hover-swap below (insertBefore), and a captured
        // element does not reliably keep that capture across a reparent --
        // some engines silently drop it, which was the actual cause of the
        // drag going solid after exactly one swap: the *next* move landed on
        // whatever pad the finger was now over, btn's own listener never
        // saw it, and nothing moved again. document is never reparented, so
        // it keeps hearing every move for the rest of the gesture no matter
        // how many times btn gets relocated underneath it.
        document.addEventListener('pointermove', onDocMove);
        document.addEventListener('pointerup', onDocUp);
        document.addEventListener('pointercancel', onDocUp);
        moveDrag(ev);
    }

    function onDocMove(ev) {
        if (ev.pointerId !== pointerId) return;
        moveDrag(ev);
    }

    function onDocUp(ev) {
        if (ev.pointerId !== pointerId) return;
        endDrag();
        pointerId = null;
    }

    function moveDrag(ev) {
        if (!clone) return;
        clone.style.left = (ev.clientX - offsetX) + 'px';
        clone.style.top = (ev.clientY - offsetY) + 'px';

        // Edge-hold paging, the way an iOS drag flips a home screen: hover
        // near either edge of the visible page for a moment and it turns.
        const host = sbPagesEl.getBoundingClientRect();
        const nearLeft = ev.clientX - host.left < 36;
        const nearRight = host.right - ev.clientX < 36;
        if ((nearLeft && sbPage > 0) || (nearRight && sbPage < sbPageCount() - 1)) {
            if (!edgeTimer) edgeTimer = setTimeout(() => {
                edgeTimer = 0;
                sbGoToPage(sbPage + (nearLeft ? -1 : 1));
            }, 550);
        } else if (edgeTimer) {
            clearTimeout(edgeTimer);
            edgeTimer = 0;
        }

        const under = document.elementFromPoint(ev.clientX, ev.clientY);
        const targetPad = under && under.closest('.sb-pad:not(.is-drag-source)');
        if (!targetPad) return;
        const targetPage = targetPad.closest('.sb-page');
        if (!targetPage || targetPad === btn) return;
        const sourcePage = btn.closest('.sb-page');
        const rect = targetPad.getBoundingClientRect();
        const before = (ev.clientX - rect.left) < rect.width / 2;
        targetPage.classList.add('is-reflowing');
        if (sourcePage) sourcePage.classList.add('is-reflowing');
        targetPage.insertBefore(btn, before ? targetPad : targetPad.nextSibling);
    }

    function endDrag() {
        document.removeEventListener('pointermove', onDocMove);
        document.removeEventListener('pointerup', onDocUp);
        document.removeEventListener('pointercancel', onDocUp);
        if (edgeTimer) { clearTimeout(edgeTimer); edgeTimer = 0; }
        if (clone) { clone.remove(); clone = null; }
        btn.classList.remove('is-drag-source');
        sbPagesEl.classList.remove('is-reordering');
        for (const page of sbPagesEl.querySelectorAll('.sb-page.is-reflowing'))
            page.classList.remove('is-reflowing');
        sbDragging = false;

        // The DOM order across every page is now the order that matters --
        // walk it once and that is the list soundboard.reorder gets.
        const order = [];
        for (const pad of sbPagesEl.querySelectorAll('.sb-pad')) order.push(pad.dataset.id);
        sbOrder = order;
        sbSendReorder();
        // A drop can leave one page over its own capacity and another under
        // it (a pad crossed a page boundary); rebuild so each page holds
        // exactly sbPerPage again.
        sbBuildPages();
    }

    btn.addEventListener('pointerdown', (ev) => {
        if (ev.button !== undefined && ev.button !== 0) return;
        pointerId = ev.pointerId;
        startX = ev.clientX;
        startY = ev.clientY;
        moved = false;
        longPressed = false;
        cancelPress();
        pressTimer = setTimeout(() => { pressTimer = 0; startDrag(ev); }, 450);
    });

    // Pre-drag only: once longPressed flips true, document's own listeners
    // (added in startDrag) are what drive the rest of the gesture, and these
    // step aside so the drop is never handled twice.
    btn.addEventListener('pointermove', (ev) => {
        if (ev.pointerId !== pointerId || longPressed) return;
        if (Math.abs(ev.clientX - startX) > 10 || Math.abs(ev.clientY - startY) > 10) {
            moved = true;
            cancelPress();
        }
    });

    function finish(ev) {
        if (ev.pointerId !== pointerId || longPressed) return;
        cancelPress();
        if (!moved) sbToggle(btn.dataset.id);
        pointerId = null;
    }
    btn.addEventListener('pointerup', finish);
    btn.addEventListener('pointercancel', () => {
        if (longPressed) return;
        cancelPress();
        pointerId = null;
    });
}

function sbBuildPad(id) {
    const info = sbInfo.get(id);
    const btn = el('button', 'sb-pad');
    btn.type = 'button';
    btn.dataset.id = id;
    btn.innerHTML =
        '<svg class="pad-ring" viewBox="0 0 100 50" preserveAspectRatio="none" aria-hidden="true">' +
        '<rect x="1.4" y="1.4" width="97.2" height="47.2" rx="9" pathLength="1"/></svg>' +
        '<span class="sb-pad-name"></span>';
    btn.querySelector('.sb-pad-name').textContent = (info && info.name) || id;
    sbWirePad(btn);
    return btn;
}

function sbBuildPages() {
    const pageCount = sbPageCount();
    sbEmptyEl.hidden = sbOrder.length > 0;
    sbPagesEl.hidden = sbOrder.length === 0;

    // Rebuilt wholesale rather than reconciled: the grid's shape -- how many
    // pages, how many pads on each -- changes with the order and with the
    // viewport, and a partial patch here would have to re-derive that shape
    // to know what changed anyway.
    sbPagesEl.innerHTML = '';
    for (let p = 0; p < pageCount; p++) {
        const page = el('div', 'sb-page');
        page.style.gridTemplateColumns = 'repeat(' + sbCols + ', 1fr)';
        page.style.gridTemplateRows = 'repeat(' + sbRows + ', 1fr)';
        for (const id of sbOrder.slice(p * sbPerPage, (p + 1) * sbPerPage))
            page.append(sbBuildPad(id));
        sbPagesEl.append(page);
    }
    sbPage = Math.min(sbPage, Math.max(0, pageCount - 1));
    sbBuildDots(pageCount);
    sbApplyPlayingLooks();
}

function renderSoundboard() {
    const sounds = (state.soundboard && state.soundboard.sounds) || [];
    sbInfo = new Map(sounds.map((s) => [s.id, s]));
    if (sbDragging) return;

    const daemonOrder = sounds.map((s) => s.id);
    const changed = daemonOrder.length !== sbOrder.length ||
        daemonOrder.some((id, i) => id !== sbOrder[i]);
    if (changed) {
        sbOrder = daemonOrder;
        sbBuildPages();
        return;
    }
    // Same order: refresh names in place rather than rebuilding, so a rename
    // made on the desktop mid-swipe does not interrupt the page it landed on.
    for (const pad of sbPagesEl.querySelectorAll('.sb-pad')) {
        const info = sbInfo.get(pad.dataset.id);
        if (info) pad.querySelector('.sb-pad-name').textContent = info.name || pad.dataset.id;
    }
}

function renderSoundboardLevels() {
    const data = sbLevels || {};
    sbPlaying = new Set(Object.keys(data));
    sbApplyPlayingLooks();
}

sbMeasure();
if (window.ResizeObserver) {
    new ResizeObserver(() => {
        if (sbDragging) return;
        const prev = sbPerPage;
        sbMeasure();
        if (sbPerPage !== prev) sbBuildPages();
    }).observe(sbPagesEl);
}
sbPagesEl.addEventListener('scroll', () => {
    if (sbDragging) return;
    const w = sbPagesEl.clientWidth || 1;
    const p = Math.round(sbPagesEl.scrollLeft / w);
    if (p === sbPage) return;
    sbPage = p;
    for (const [i, dot] of Array.from(sbDotsEl.children).entries())
        dot.classList.toggle('is-active', i === sbPage);
}, { passive: true });

// --------------------------------------------------------------- profiles

function renderProfiles() {
    // The button carries no label any more, so which profile is loaded is said
    // in its tooltip and, where it actually matters, by the tick in the list.
    $('#profile-btn').title = state.activeProfile
        ? 'Profiles — ' + state.activeProfile + ' is loaded'
        : 'Profiles';
    const list = $('#profile-list');
    if ($('#profile-modal').hidden) return;

    const items = state.profiles.length ? state.profiles : [];
    if (!items.length) {
        list.innerHTML = '<div class="text-sm text-faint">' +
            'No saved profiles yet. Create them in the desktop mixer.</div>';
        return;
    }
    reconcile(list, items, (name) => 'p:' + name, (name) => {
        const node = el('button', 'profile-item');
        node.type = 'button';
        const label = el('span', 'truncate');
        const tick = el('span', 'text-accent flex-none');
        node.append(label, tick);
        let current = name;
        node.onclick = () => {
            send({ cmd: 'profile.load', name: current });
            $('#profile-modal').hidden = true;
        };
        node.update = (item) => {
            current = item;
            label.textContent = item;
            const active = item === state.activeProfile;
            node.classList.toggle('is-active', active);
            tick.innerHTML = active ? icon('check', 16) : '';
        };
        return node;
    });
}

// ----------------------------------------------------------------- render

function renderState() {
    $('#version').textContent = 'v' + state.version;
    renderRail();
    renderAppSettings();
    renderFooter();
    renderProfiles();
    renderSoundboard();
}

function renderLevels() {
    if (!state) return;

    // By key rather than by position: the strips share one container with a
    // divider in the middle of it, so an index into that container is not an
    // index into either list.
    //
    // Both bars come from the desktop, and the pair only means something
    // because the two taps differ.
    //
    // Right bar, input devices: the device's own published output rather than
    // the noise filter's. Shared application audio joins the bus downstream of
    // the filter, so this is the only tap that shows the device as its
    // listeners actually hear it.
    //
    // Left bar, input devices: this card's send into the Monitor mix -- what
    // you hear from *this* microphone and nothing else. That send is a loopback
    // carrying the same signal at the card's Monitor fader and silenced while
    // the ear is off, so it is that formula rather than a second probe. Showing
    // the whole Monitor mix here would put an identical bar on every input card
    // and move it for audio that card had no part in.
    for (const item of state.inputs) {
        const node = stripNodes.get('input:' + item.id);
        if (!node) continue;
        const src = meterPosition(levels[item.id + '-src'] || 0);
        node.refs.micMeter.set(item.connected ? src : 0);
        const heard = item.monitoring && !item.monitorMuted && item.connected;
        node.refs.meter.set(heard ? src * item.monitorVolume : 0);
    }
    // Channels keep the bus itself on the left -- everything routed here,
    // which is what the card is for -- and the microphone it publishes on the
    // right, tapped after that microphone's own noise suppression and EQ.
    for (const item of state.channels) {
        const node = stripNodes.get('channel:' + item.id);
        if (!node) continue;
        node.refs.meter.set(meterPosition(levels[item.id] || 0));
        node.refs.micMeter.set(
            item.micSource ? meterPosition(levels[item.id + '-mic'] || 0) : 0);
    }

    // monitor-mix is the shared bus, upstream of every output's own fader and
    // mute, so each row scales it by its own to show what leaves that device.
    const monitorMix = meterPosition(levels['monitor-mix'] || 0);
    const rows = $('#out-rows').children;
    state.outputs.monitor.forEach((m, i) => {
        const node = rows[i];
        if (!node || !node.refs) return;
        node.refs.meter.set(m.muted || !m.connected ? 0 : monitorMix * m.volume);
    });

    // The Stream mix's probe already hears post-fader audio, because the level
    // is a channel volume on the sink rather than a loopback gain. Undo the
    // fader before the log curve, then reapply it, or the two mixes disagree
    // about what the same signal looks like.
    const streamRow = rows[state.outputs.monitor.length];
    if (streamRow && streamRow.refs) {
        const s = state.outputs.stream;
        const fader = s.muted ? 0 : s.volume;
        if (fader <= 0) streamRow.refs.meter.set(0);
        else {
            const post = levels['stream-mix'] || 0;
            streamRow.refs.meter.set(meterPosition(Math.min(1, post / fader)) * fader);
        }
    }

    renderSoundboardLevels();
}

// ------------------------------------------------------- collapsing bands
//
// The mixer row is the only part of this page anybody watches while a stream is
// running; the wordmark and the output mixes are reference. On a phone held
// sideways they are most of the screen, so each folds away behind a tab on the
// edge facing the mixer -- which is where the height it gives back appears.
//
// Remembered on the device for the same reason the scrollbar setting is: a
// tablet on a stand and the machine driving the stream want different answers,
// and neither reaches the audio.

function makeBand(band, button, prefKey, labels) {
    function apply(collapsed, animate) {
        // The first application is the page loading into a remembered state,
        // not a fold. Animating it would play a 240ms collapse at every load of
        // a page whose bands were already meant to be shut.
        if (!animate) band.classList.add('no-transition');
        band.classList.toggle('is-collapsed', collapsed);
        button.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
        button.setAttribute('aria-label', collapsed ? labels.expand : labels.collapse);
        button.title = collapsed ? labels.expand : labels.collapse;
        if (!animate) {
            // Two frames: one for the class to land, one for the layout it
            // caused to settle before transitions are allowed again.
            requestAnimationFrame(() => requestAnimationFrame(() =>
                band.classList.remove('no-transition')));
        }
    }

    button.onclick = () => {
        prefs[prefKey] = !prefs[prefKey];
        savePrefs();
        apply(prefs[prefKey], true);
        // The rail is taller or shorter than it was, so how much of the row
        // fits has changed -- and with it whether the scrollbar is needed.
        railView.sync();
    };
    apply(!!prefs[prefKey], false);
}

applyMotionPref();
applyDesktopControlsPref();
makeBand($('#header'), $('#header-toggle'), 'headerCollapsed',
         { collapse: 'Hide the header', expand: 'Show the header' });
makeBand($('#footer'), $('#footer-toggle'), 'footerCollapsed',
         { collapse: 'Hide the output mixes', expand: 'Show the output mixes' });

// ------------------------------------------------------------------- boot

$('#profile-btn').onclick = () => {
    $('#profile-modal').hidden = false;
    if (state) renderProfiles();
};
$('#profile-close').onclick = () => { $('#profile-modal').hidden = true; };
$('#profile-modal').onclick = (ev) => {
    if (ev.target === $('#profile-modal')) $('#profile-modal').hidden = true;
};

// The settings sheet's two halves. Switching to Applications draws that list
// for the first time if the sheet was opened on the device tab -- until then
// there was nothing visible for renderAppSettings() to do, and it says so.
function showSettingsTab(which) {
    const apps = which === 'apps';
    $('#tab-device').classList.toggle('is-active', !apps);
    $('#tab-apps').classList.toggle('is-active', apps);
    $('#tab-device').setAttribute('aria-selected', apps ? 'false' : 'true');
    $('#tab-apps').setAttribute('aria-selected', apps ? 'true' : 'false');
    $('#device-settings').hidden = apps;
    $('#app-settings').hidden = !apps;
    if (apps && state) renderAppSettings();
}

$('#tab-device').onclick = () => showSettingsTab('device');
$('#tab-apps').onclick = () => showSettingsTab('apps');

$('#settings-btn').onclick = () => {
    $('#settings-modal').hidden = false;
    renderDeviceSettings();
    // Always on the device tab: it is the half somebody came looking for, and
    // the applications list is a click away rather than a scroll away.
    showSettingsTab('device');
};
$('#settings-close').onclick = () => { $('#settings-modal').hidden = true; };
$('#settings-modal').onclick = (ev) => {
    if (ev.target === $('#settings-modal')) $('#settings-modal').hidden = true;
};

// Both panels are dismissed by the same key, and only the one on top.
document.addEventListener('keydown', (ev) => {
    if (ev.key !== 'Escape') return;
    if (!$('#settings-modal').hidden) $('#settings-modal').hidden = true;
    else if (!$('#profile-modal').hidden) $('#profile-modal').hidden = true;
});

// A page left open on a shelf gets its socket closed by the phone when the
// screen goes off. Reconnect on the way back rather than waiting out the
// backoff, so picking the tablet up shows live meters immediately.
document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && !conn.open) {
        conn.attempts = 0;
        connect();
    }
});

connect();
