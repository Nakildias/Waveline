#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#
# Phase 1: capture good vs robotic startup states for Waveline capture devices.
#
# Usage:
#   ./scripts/capture-startup-diag.sh record         # label each mic interactively
#   ./scripts/capture-startup-diag.sh record mic=good master-4=robotic master-3=good
#   ./scripts/capture-startup-diag.sh trial          # restart wavelined, then label each mic
#   ./scripts/capture-startup-diag.sh batch 20       # 20 controlled restarts
#   ./scripts/capture-startup-diag.sh summary        # compare good vs robotic
#   ./scripts/capture-startup-diag.sh snapshot       # print one snapshot (no log)
#
# Logs: see init_log_dir() — defaults under ~/.local/share/waveline/… with
# fallbacks when that tree is not writable (e.g. root-owned from an old install).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR=""
LOG_FILE=""
SNAP_DIR=""

init_log_dir() {
	[[ -n "$LOG_DIR" ]] && return 0
	local candidate reason
	for candidate in \
		"${WAVELINE_DIAG_DIR:-}" \
		"${XDG_DATA_HOME:-$HOME/.local/share}/waveline/capture-startup-diag" \
		"${XDG_CACHE_HOME:-$HOME/.cache}/waveline/capture-startup-diag" \
		"$ROOT/capture-startup-diag-data"; do
		[[ -n "$candidate" ]] || continue
		if mkdir -p "$candidate/snapshots" 2>/dev/null && [[ -w "$candidate" ]]; then
			LOG_DIR="$candidate"
			LOG_FILE="$LOG_DIR/trials.jsonl"
			SNAP_DIR="$LOG_DIR/snapshots"
			if [[ "$candidate" != "${XDG_DATA_HOME:-$HOME/.local/share}/waveline/capture-startup-diag" ]]; then
				reason="${WAVELINE_DIAG_DIR:+WAVELINE_DIAG_DIR set}"
				reason="${reason:-$(printf '%s is not writable' \
					"${XDG_DATA_HOME:-$HOME/.local/share}/waveline")}"
				echo "note: using $LOG_DIR ($reason)" >&2
			fi
			return 0
		fi
	done
	echo "cannot create a writable log directory (tried share, cache, and $ROOT/capture-startup-diag-data)" >&2
	echo "fix: sudo chown -R \"\$USER\":\"\$USER\" \"${XDG_DATA_HOME:-$HOME/.local/share}/waveline\"" >&2
	exit 1
}

need_cmd() {
	for c in "$@"; do
		command -v "$c" >/dev/null 2>&1 || {
			echo "missing required command: $c" >&2
			exit 1
		}
	done
}

trial_count() {
	if [[ ! -f "$LOG_FILE" ]]; then
		echo 0
		return
	fi
	jq -s 'length' "$LOG_FILE" 2>/dev/null || echo 0
}

wait_wavelined_ready() {
	local deadline=$((SECONDS + 90))
	while (( SECONDS < deadline )); do
		if busctl --user call org.waveline.Mixer /org/waveline/Mixer \
			org.waveline.Mixer Levels >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.5
	done
	echo "wavelined did not become ready within 90s" >&2
	return 1
}

metadata_value() {
	local key="$1"
	pw-metadata -n settings 0 "$key" 2>/dev/null \
		| rg -o "value:'[^']+'" \
		| head -1 \
		| cut -d"'" -f2
}

clock_source() {
	pw-dump 2>/dev/null | jq -c '
		([.[] | select(.type=="PipeWire:Interface:Node")
		  | select(.info.props["node.driver-id"] != null)
		  | (.info.props["node.driver-id"] | tonumber? // empty)]
		| map(select(. != null))
		| group_by(.) | max_by(length) | .[0] // null) as $id
		| if $id == null then null else
		    ([.[] | select(.type=="PipeWire:Interface:Node") | select(.id == $id)
		      | {id, name:.info.props["node.name"]}] | first) end'
}

graph_clock() {
	local quantum rate driver_json driver_name
	quantum="$(metadata_value clock.quantum)"
	rate="$(metadata_value clock.rate)"
	driver_json="$(clock_source)"
	driver_name="$(printf '%s' "$driver_json" | jq -r '.name // empty')"
	printf '%s\t%s\t%s' "${quantum:-unknown}" "${rate:-unknown}" "${driver_name:-unknown}"
}

graph_driver() {
	clock_source
}

node_field() {
	local name="$1" field="$2"
	pw-dump 2>/dev/null | jq -r --arg n "$name" --arg f "$field" '
		[.[] | select(.type=="PipeWire:Interface:Node")
		 | select(.info.props["node.name"] == $n)
		 | .info.props[$f] // empty]
		| last // empty'
}

pick_node_by_prefix() {
	local prefix="$1"
	pw-dump 2>/dev/null | jq -r --arg p "$prefix" '
		[.[] | select(.type=="PipeWire:Interface:Node")
		 | select(.info.props["node.name"] // "" | startswith($p))
		 | {id: .id, name: .info.props["node.name"]}]
		| sort_by(.id) | last | .name // empty'
}

fetch_mic_roster() {
	local raw
	if ! raw="$(busctl --user call org.waveline.Mixer /org/waveline/Mixer \
		org.waveline.Mixer MasterBuses 2>/dev/null)"; then
		jq -n '[]'
		return
	fi
	# busctl emits literal \t inside quoted strings; names may contain spaces.
	printf '%s' "$raw" | jq -R -s '
		[ scan("\"([^\"]*)\"") | .[0] | gsub("\\\\t"; "\t") | split("\t") ]
		| map(select(length >= 3))
		| to_entries
		| map({
		    index: (.key + 1),
		    id: .value[0],
		    name: .value[1],
		    capture_match: .value[2]
		  })'
}

prompt_trial_labels() {
	local roster="$1" auto_save="${2:-0}"
	local count labels='{}' idx name id label ans
	count="$(jq 'length' <<<"$roster")"
	if [[ "$count" -eq 0 ]]; then
		echo "no input devices from wavelined MasterBuses (is wavelined running?)" >&2
		return 1
	fi
	{
		echo
		echo "=== Rate this startup — all mics ==="
		jq -r '.[] | "  Mic #\(.index): \(.name)"' <<<"$roster"
		echo
		echo "Listen to each mic, then answer good or robotic for every one."
	} >&2
	for (( i=0; i<count; ++i )); do
		idx="$(jq -r ".[$i].index" <<<"$roster")"
		name="$(jq -r ".[$i].name" <<<"$roster")"
		id="$(jq -r ".[$i].id" <<<"$roster")"
		while true; do
			printf '\nMic #%s (%s) — 1=good  2=robotic: ' "$idx" "$name" >&2
			read -r ans </dev/tty
			case "$ans" in
				1|good|g|G) label=good; break ;;
				2|robotic|r|R) label=robotic; break ;;
				*) echo "  type 1 or 2" >&2 ;;
			esac
		done
		labels="$(jq -n --argjson base "$labels" --arg id "$id" --arg label "$label" \
			'$base + {($id): $label}')"
	done
	{
		echo
		echo "Summary for this startup:"
		jq -rn --argjson roster "$roster" --argjson labels "$labels" '
		  $roster[] | "  Mic #\(.index) \(.name): \($labels[.id])"
		'
		if [[ "$auto_save" != 1 ]]; then
			printf 'Skip saving this trial? [y/N] '
		fi
	} >&2
	if [[ "$auto_save" == 1 ]]; then
		echo "$labels"
		return 0
	fi
	read -r ans </dev/tty
	if [[ "$ans" =~ ^[yY]$ ]]; then
		echo skip >&2
		return 1
	fi
	echo "$labels"
}

parse_label_args() {
	local labels='{}' arg key val
	for arg in "$@"; do
		[[ "$arg" == *"="* ]] || continue
		key="${arg%%=*}"
		val="${arg#*=}"
		labels="$(jq -n --argjson base "$labels" --arg key "$key" --arg val "$val" \
			'$base + {($key): $val}')"
	done
	echo "$labels"
}

labels_slug() {
	jq -r 'to_entries | sort_by(.key) | map("\(.key)-\(.value)") | join("_")' <<<"$1"
}

device_snapshot_named() {
	local key="$1" name="$2"
	local follows_driver state rate headroom period_size period_num latency clock priority node_id clock_id is_clock_source
	if [[ -z "$name" ]]; then
		jq -n --arg k "$key" '{key:$k, present:false}'
		return
	fi
	follows_driver="$(node_field "$name" "node.driver-id")"
	state="$(pw-dump 2>/dev/null | jq -r --arg n "$name" '
		[.[] | select(.type=="PipeWire:Interface:Node")
		 | select(.info.props["node.name"] == $n) | .info.state]
		| last // empty')"
	node_id="$(pw-dump 2>/dev/null | jq -r --arg n "$name" '
		[.[] | select(.type=="PipeWire:Interface:Node")
		 | select(.info.props["node.name"] == $n) | .id]
		| last // empty')"
	clock_id="$(clock_source | jq -r '.id // empty')"
	is_clock_source=false
	[[ -n "$clock_id" && "$node_id" == "$clock_id" ]] && is_clock_source=true
	rate="$(node_field "$name" "audio.rate")"
	headroom="$(node_field "$name" "api.alsa.headroom")"
	period_size="$(node_field "$name" "api.alsa.period-size")"
	period_num="$(node_field "$name" "api.alsa.period-num")"
	latency="$(node_field "$name" "node.max-latency")"
	clock="$(node_field "$name" "clock.name")"
	priority="$(node_field "$name" "priority.driver")"
	jq -n \
		--arg key "$key" \
		--arg name "$name" \
		--arg follows_driver "$follows_driver" \
		--arg node_id "$node_id" \
		--arg state "$state" \
		--arg rate "$rate" \
		--arg headroom "$headroom" \
		--arg period_size "$period_size" \
		--arg period_num "$period_num" \
		--arg latency "$latency" \
		--arg clock "$clock" \
		--arg priority "$priority" \
		--argjson is_clock_source "$is_clock_source" \
		'{key:$key, present:true, name:$name, node_id:($node_id|tonumber? // $node_id),
		  is_clock_source:$is_clock_source,
		  follows_driver_id:($follows_driver|tonumber? // $follows_driver),
		  state:$state, rate:($rate|tonumber? // $rate),
		  headroom:($headroom|tonumber? // $headroom),
		  period_size:($period_size|tonumber? // $period_size),
		  period_num:($period_num|tonumber? // $period_num),
		  latency:$latency, clock:$clock,
		  priority:($priority|tonumber? // $priority)}'
}

build_mics_array() {
	local labels_json="$1"
	local roster="$2"
	local mics='[]' count i
	count="$(jq 'length' <<<"$roster")"
	for (( i=0; i<count; ++i )); do
		local index id name capture label node snap entry
		index="$(jq -r ".[$i].index" <<<"$roster")"
		id="$(jq -r ".[$i].id" <<<"$roster")"
		name="$(jq -r ".[$i].name" <<<"$roster")"
		capture="$(jq -r ".[$i].capture_match" <<<"$roster")"
		label="$(jq -r --arg id "$id" '.[$id] // "unknown"' <<<"$labels_json")"
		node="$(pick_node_by_prefix "$capture")"
		snap="$(device_snapshot_named "$id" "$node")"
		entry="$(jq -n \
			--argjson index "$index" \
			--arg id "$id" \
			--arg name "$name" \
			--arg capture_match "$capture" \
			--arg label "$label" \
			--argjson device "$snap" \
			'{index:$index, id:$id, name:$name, capture_match:$capture_match,
			  label:$label, device:$device}')"
		mics="$(jq -n --argjson arr "$mics" --argjson e "$entry" '$arr + [$e]')"
	done
	echo "$mics"
}

waveline_masters() {
	pw-dump 2>/dev/null | jq -c '
		[.[] | select(.type=="PipeWire:Interface:Node")
		 | select(.info.props["node.name"] // "" | test("^waveline-(mic|master-[0-9]+)-capture-selector$"))
		 | {name:.info.props["node.name"], state:.info.state,
		    driver_id:(.info.props["node.driver-id"] // null)}]'
}

capture_links() {
	pw-link -l 2>/dev/null | rg -n \
		'alsa_input\.(usb-Elgato|usb-Antlion|usb-Hercules)|waveline-(mic|master-[0-9]+)-capture-selector' \
		|| true
}

resync_lines() {
	local since="${1:-90 seconds ago}"
	journalctl --user -u pipewire.service --since "$since" --no-pager 2>/dev/null \
		| rg -i 'resync|hw:[0-9]+c:|follower avail|resample:' || true
}

levels_row() {
	busctl --user call org.waveline.Mixer /org/waveline/Mixer \
		org.waveline.Mixer Levels 2>/dev/null || echo "(wavelined not ready)"
}

write_full_snapshot() {
	local out="$1"
	{
		printf '=== capture-startup-diag full snapshot ===\n'
		date -u '+%Y-%m-%d %H:%M:%S UTC'
		printf '\n--- wavelined levels ---\n'
		levels_row
		printf '\n--- graph clock ---\n'
		graph_clock | awk -F'\t' '{printf "quantum=%s rate=%s driver=%s\n",$1,$2,$3}'
		printf '\n--- graph driver node ---\n'
		graph_driver | jq .
		printf '\n--- pw-top (capture nodes) ---\n'
		timeout 2 pw-top -b -n 1 2>/dev/null \
			| rg 'alsa_input\.|waveline-.*capture-selector|waveline-.*-gain|Dummy-Driver' || true
		printf '\n--- capture links ---\n'
		capture_links
		printf '\n--- resync (last 90s) ---\n'
		resync_lines "90 seconds ago"
		printf '\n--- pw-dump nodes (filtered) ---\n'
		pw-dump 2>/dev/null | jq '[.[] | select(.type=="PipeWire:Interface:Node")
			| select(.info.props["node.name"] // "" |
				test("alsa_input\\.(usb-Elgato|usb-Antlion|usb-Hercules)|^waveline-(mic|master-[0-9]+)-capture-selector$"))
			| {id, state:.info.state, name:.info.props["node.name"],
			   props:(.info.props | with_entries(select(.key |
			     test("^(node\\.|audio\\.|clock\\.|priority\\.|api\\.alsa\\.)"))))}]'
	} >"$out"
}

build_record_json() {
	local trial="$1" labels_json="$2" restart_ms="${3:-0}"
	local quantum rate driver_name clock_line rest roster mics
	clock_line="$(graph_clock)"
	quantum="${clock_line%%$'\t'*}"
	rest="${clock_line#*$'\t'}"
	rate="${rest%%$'\t'*}"
	driver_name="${rest#*$'\t'}"
	roster="$(fetch_mic_roster)"
	mics="$(build_mics_array "$labels_json" "$roster")"
	local masters links resync driver
	masters="$(waveline_masters)"
	driver="$(graph_driver)"
	links="$(capture_links | jq -Rs 'split("\n") | map(select(length>0))')"
	resync="$(resync_lines "90 seconds ago" | jq -Rs 'split("\n") | map(select(length>0))')"
	jq -cn \
		--arg ts "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
		--argjson labels "$labels_json" \
		--argjson trial "$trial" \
		--argjson restart_ms "$restart_ms" \
		--arg quantum "$quantum" \
		--arg rate "$rate" \
		--arg driver_name "$driver_name" \
		--argjson driver "$driver" \
		--argjson mics "$mics" \
		--argjson masters "$masters" \
		--argjson links "$links" \
		--argjson resync "$resync" \
		'{
		  trial: $trial,
		  ts: $ts,
		  labels: $labels,
		  mics: $mics,
		  wavelined_restart_ms: $restart_ms,
		  graph: {quantum: ($quantum|tonumber? // $quantum), rate: ($rate|tonumber? // $rate),
		          driver_name: $driver_name, driver: $driver},
		  waveline_selectors: $masters,
		  capture_links: $links,
		  resync_last_90s: $resync
		}'
}

save_trial() {
	local trial="$1" labels_json="$2" restart_ms="${3:-0}"
	local slug snap record
	slug="$(labels_slug "$labels_json")"
	snap="$SNAP_DIR/trial-$(printf '%03d' "$trial")-${slug}.txt"
	echo "saving trial $trial (snapshot + graph capture, ~5s)..."
	write_full_snapshot "$snap"
	record="$(build_record_json "$trial" "$labels_json" "$restart_ms")"
	printf '%s\n' "$record" >>"$LOG_FILE"
	echo "recorded trial $trial (${restart_ms}ms to ready)"
	echo "  labels:  $(printf '%s\n' "$record" | jq -r '[.mics[] | "Mic #\(.index) \(.name)=\(.label)"] | join(", ")')"
	echo "  log:     $LOG_FILE"
	echo "  snapshot $snap"
	printf '%s\n' "$record" | jq '{trial, labels, mics: [.mics[] | {index, name, label,
	  follows: .device.follows_driver_id, headroom: .device.headroom}], resyncs:(.resync_last_90s|length)}'
}

record() {
	need_cmd jq pw-dump busctl
	init_log_dir
	local labels_json roster
	roster="$(fetch_mic_roster)"
	if [[ $# -gt 0 ]]; then
		labels_json="$(parse_label_args "$@")"
	else
		labels_json="$(prompt_trial_labels "$roster")" || return 0
	fi
	local trial=$(( $(trial_count) + 1 ))
	save_trial "$trial" "$labels_json" 0
}

trial_once() {
	local labels_json="${1:-}" auto_save="${2:-0}"
	need_cmd jq pw-dump busctl systemctl
	init_log_dir
	local trial=$(( $(trial_count) + 1 ))
	local t0=$SECONDS roster
	echo "trial $trial: restarting wavelined..."
	systemctl --user restart wavelined.service
	wait_wavelined_ready
	local restart_ms=$(( (SECONDS - t0) * 1000 ))
	# Let the first graph cycles settle before sampling.
	sleep 2
	roster="$(fetch_mic_roster)"
	if [[ -z "$labels_json" ]]; then
		labels_json="$(prompt_trial_labels "$roster" "$auto_save")" || {
			echo "skipped trial $trial"
			return 0
		}
	fi
	save_trial "$trial" "$labels_json" "$restart_ms"
}

batch() {
	local n="${1:-20}"
	local i
	for (( i = 1; i <= n; ++i )); do
		echo "======== batch $i / $n ========"
		trial_once "" 1 || true
		echo
	done
	summary
}

summary() {
	need_cmd jq
	init_log_dir
	if [[ ! -f "$LOG_FILE" ]]; then
		echo "no trials logged yet at $LOG_FILE"
		exit 0
	fi
	echo "log: $LOG_FILE"
	echo
	echo "Per-mic failure rates:"
	jq -s '
	  [.[] | select(.mics != null) | .mics[] |
	    {name, id, label,
	     follows: .device.follows_driver_id,
	     headroom: .device.headroom,
	     is_clock: .device.is_clock_source}]
	  | group_by(.name)
	  | map({
	      mic: .[0].name,
	      id: .[0].id,
	      good: ([.[] | select(.label=="good")] | length),
	      robotic: ([.[] | select(.label=="robotic")] | length),
	      robotic_follows: ([.[] | select(.label=="robotic") | .follows] | unique),
	      good_follows: ([.[] | select(.label=="good") | .follows] | unique),
	      robotic_headroom: ([.[] | select(.label=="robotic") | .headroom] | unique)
	    })
	  | sort_by(.mic)
	' "$LOG_FILE"
	echo
	echo "Per-trial table:"
	jq -s -r '
	  [.[] | select(.mics != null) | . as $t | $t.mics | sort_by(.index) | .[]
	   | [$t.trial, .index, .name, .label,
	      ($t.graph.quantum // "-"), ($t.graph.driver_name // "-"),
	      (.device.follows_driver_id // "-"), (.device.headroom // "-"),
	      (($t.resync_last_90s // []) | length)]]
	  | .[]
	  | @tsv
	' "$LOG_FILE" \
	  | column -t -s $'\t' \
	  -N 'trial,mic#,name,label,quantum,graph_driver,follows,headroom,resyncs'
}

snapshot() {
	need_cmd jq pw-dump busctl
	local roster labels
	roster="$(fetch_mic_roster)"
	labels="$(jq '[.[] | {key:.id, value:"preview"}] | from_entries' <<<"$roster")"
	build_record_json 0 "$labels" 0 | jq .
}

repair_log() {
	need_cmd jq
	init_log_dir
	if [[ ! -f "$LOG_FILE" ]]; then
		echo "no log at $LOG_FILE" >&2
		exit 1
	fi
	local count backup tmp
	count="$(jq -s 'length' "$LOG_FILE")"
	backup="${LOG_FILE}.bak.$(date +%s)"
	cp "$LOG_FILE" "$backup"
	tmp="$(mktemp)"
	jq -s 'to_entries | map(.value + {trial: (.key + 1)}) | .[]' -c "$LOG_FILE" >"$tmp"
	mv "$tmp" "$LOG_FILE"
	echo "repaired $count trials (compact JSON, trial numbers 1..$count)"
	echo "backup: $backup"
}

usage() {
	cat <<EOF
Usage: $(basename "$0") <command> [args]

Commands:
  record [id=label ...]     Label each mic (interactive if no args), then snapshot.
  trial                     Restart wavelined, then label each mic, then snapshot.
  batch [N]                 Run N trials (default 20); prompts per mic each time.
  summary                   Per-mic good vs robotic breakdown.
  repair-log                Compact log file and fix trial numbering.
  snapshot                  Print a snapshot without logging.

Log directory: \${XDG_DATA_HOME:-~/.local/share}/waveline/capture-startup-diag
             (falls back to ~/.cache/… or ./capture-startup-diag-data if not writable)

Override: WAVELINE_DIAG_DIR=/path/to/dir $0 batch 20
EOF
}

main() {
	local cmd="${1:-}"
	shift || true
	case "$cmd" in
		record) shift; record "$@" ;;
		trial) trial_once ;;
		batch) batch "${1:-20}" ;;
		summary) summary ;;
		repair-log) repair_log ;;
		snapshot) snapshot ;;
		-h|--help|help|"") usage ;;
		*) echo "unknown command: $cmd" >&2; usage; exit 1 ;;
	esac
}

main "$@"
