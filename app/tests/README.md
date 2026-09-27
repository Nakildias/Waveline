# Audio recovery regression checks

Run `cmake -S app -B /tmp/waveline-build`, build, then run
`ctest --test-dir /tmp/waveline-build --output-on-failure`.

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
