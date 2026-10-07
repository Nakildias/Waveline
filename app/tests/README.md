# Audio recovery regression checks

Run `cmake -S app -B /tmp/waveline-build`, build, then run
`ctest --test-dir /tmp/waveline-build --output-on-failure`.

`idle-client` uses a fake mixer on a private D-Bus session. It checks that an
untouched GUI client avoids full-state refreshes at 400 ms while keeping its
16 ms meter polling, responds promptly to daemon notifications, refreshes
immediately on restore, and keeps polling disabled across a hidden reconnect.
It also verifies the five-second fallback for missed notifications. This test
requires `dbus-run-session` and permission to create local sockets.

Idle optimization changes the full-state fallback from 2.5 Hz to 0.2 Hz (92%
fewer periodic full refreshes), coalesces change notifications, and resolves
channel effects meter routing on control changes instead of every meter tick
(removing 125–187.5 routing calls/second per visible channel effects window).
The daemon now coalesces external node additions, removals, and property
updates into `Changed` notifications so app lists and device pickers do not
depend on the fallback poll for launch/hotplug updates.
Audio DSP, graph scheduling, buffer sizes, and meter sampling are unchanged.
These are work-count reductions, not a measured whole-application CPU claim.
Changes delivered by `Changed` remain immediate; state without a notification
can take up to five seconds to reconcile.

For a desktop CPU comparison, use the same profile, connected hardware,
effects, graph quantum, display refresh rate, and window layout in both builds.
After startup recovery settles, sample `waveline-mixer` and `wavelined` CPU
separately for at least 30 seconds with the main window visible, with channel
effects open, and with the mixer hidden. Keep live microphone processing
enabled in both builds. Verify faders, hardware mute, playback start/stop,
device hotplug, and window restore while listening; lower GUI polling work
does not establish an audio-quality or whole-process CPU result by itself.

The tests cover overlapping recovery requests while inputs are disconnected,
a request arriving between individual rebuilds, duplicate requests, and a new
manual rebuild after completion. The sample-continuity test exercises fixed
and changing graph quanta (including 512/480 mismatch), aliased buffers, and
reset after interrupted input. Tests do not require audio hardware.

The recovery test also exercises concurrent settings publication, bounded
failed-rebuild retries, and the Wave:3 overload policy: ignore isolated xruns,
wait for five quiet seconds with callbacks still advancing, suppress recovery
during startup/manual recovery, and enforce cooldown/recovery budgets.

`stereo-graph` runs the production mixer graph against synthetic left/right
tones, checking both Stream and Monitor with output noise suppression bypassed
and enabled at zero wet mix, and with monitor FX toggled. Its Python launcher
starts a private PipeWire server, private D-Bus session, and policy-only
WirePlumber (no hardware monitors), with temporary runtime/config/state paths.
It needs PipeWire, WirePlumber 0.5+, Python, D-Bus, and permission to create local
sockets. Missing tools skip this integration test; socket restrictions fail it.
Do not run the test executable directly against your desktop audio session.

The stereo regression was also checked against the pre-fix `mixergraph.cpp`:
left-only input incorrectly produced right-channel energy and failed the test.
The corrected routing passes.

Hardware acceptance still requires listening. Keep the graph at 512/48000,
start Waveline, and allow its startup quiet/rebuild pass to complete. Check raw
Wave:3 and Tone Cable capture with effects disabled. Replug the Wave:3 while
monitoring the Tone Cable as well, including a replug during the recovery
interval. Check that both resume cleanly without pressing Rebuild. Repeat
under the user's normal CPU/GPU workload. A clean journal or a running node
alone does not prove clean audio.

Startup and assigned ALSA monitor hotplug now schedule the existing settled
capture rebuild. There is a brief capture interruption during recovery; graph
quantum and device headroom are unchanged. This is recovery from disruptive
transitions, not a claim that the ALSA/USB failure itself is eliminated.

For overload recovery acceptance, let startup settle, then reproduce CPU-load
crackling with the normal compiler/workload. Check the journal for `Wave:3
capture overload ended` after the load subsides. A sustained xrun burst may
trigger one settled rebuild (brief input interruption), never a loop during
continued xruns; two automatic recoveries per ten minutes is the limit. Verify
clean capture afterward and that other inputs stay audible. Also test a healthy
session, isolated xruns, long uptime, and a manual rebuild during overload.
Wave:3 hardware acceptance has not been performed as part of the code update.

The graph regression also creates a synthetic default microphone with no saved
`captureMatch`. It exercises warm-up and the same cleared-runtime-node state
used by the daemon's quiet phase, then verifies that Rebuild replaces the
selector, reconnects the microphone, and preserves default-following mode.
Before the issue #9 fix, the test fails at default-input warm-up; manual
recovery could also return success without rebuilding after clearing the node.
