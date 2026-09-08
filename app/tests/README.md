# Audio recovery regression checks

Run `cmake -S app -B /tmp/waveline-build`, build, then run
`ctest --test-dir /tmp/waveline-build --output-on-failure`.

The tests cover overlapping recovery requests while inputs are disconnected,
a request arriving between individual rebuilds, duplicate requests, and a new
manual rebuild after completion. The sample-continuity test exercises fixed
and changing graph quanta (including 512/480 mismatch), aliased buffers, and
reset after interrupted input. Tests do not require audio hardware.

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
