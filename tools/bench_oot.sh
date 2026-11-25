#!/bin/bash
# Benchmark loop for the Ocarina of Time (Switch 2) build: runs the title offscreen once per configuration and ranks the configurations.
#
# Usage: tools/bench_oot.sh [-d SECONDS] [-o OUTDIR] [-b BUILD_DIR] [-r] CONFIG...
#        tools/bench_oot.sh -s OUTDIR      (re-reads the run directories in OUTDIR, rewrites results.csv and prints the ranking)
#   CONFIG is "label VAR=value VAR=value ..." (one shell word, quote it); "base" alone means the defaults of tools/run_oot.sh.
#          Two assignments are taken by this script, not passed to the title: BUILD=<dir> (the build whose nxlinux runs) and DUR=<s>.
#   -d  seconds per run (default 300; ~600 to see more of the world load past frame ~2000)
#   -o  output directory (logs, samples, results.csv); default /tmp/bench_oot
#   -b  default build directory (default build-claude)
#   -r  re-run a configuration once when the title aborts early (before RETRY_BEFORE_S, default 60 s): it aborts right after
#       WindowInitialize in about a third of the runs, which says nothing about the configuration
# Example:
#   tools/bench_oot.sh -r base "gpu1 NX_GPU=1" "localbos RADV_PERFTEST=localbos" "dev BUILD=build-dev DUR=600"
#
# Rules it keeps (the machine is shared and has 15 GB):
#   - one nxlinux at a time: before each run it waits until no process named nxlinux exists (it never kills one it did not start);
#   - it waits for MIN_AVAIL_MB (default 8000) of MemAvailable, at most MAX_WAIT_S (default 600) seconds, then stops;
#   - every 5 s it samples memory; below KILL_AVAIL_MB (default 600) available it kills its own run (by PID): "oom-risk";
#   - a run whose stderr stops growing for HANG_S (default 40) seconds is noted in <run>/stalls; after HANG_KILL_S (default 120)
#     seconds of silence it is killed: "hang" (the title prints only a line or two a second, so a short silence alone proves little).
# Metrics, from the run's stderr (only the lines needed are kept; the full log is hundreds of MB):
#   fps_title  frames per second between TITLE_T0 and TITLE_T1 seconds (default 60..150) from the "[longframe] frame N (T s)" lines
#   fps_world  frames per second over the frames >= WORLD_FRAME (default 2000), if the run gets there
#   submit_ms  median "submit X ms" of the "replay per frame" lines (title frames / world frames); rp = median render passes a frame
#   oneshot_ms median "one-shot submissions ... X ms each"; allocs = max "N live device memory allocations"
#   peak_mem   max VmRSS+VmSwap of the process (MB), peak GTT of the AMD iGPU (MB), min MemAvailable (MB)
#   status     ok (ran the whole duration), early-abort, crash, exit:<code>, hang, oom-risk; errors counts SIGSEGV / general protection /
#              abort / vkQueueSubmit2 failed / GPUVM / "failed" in stderr and segfault / GPUVM / OOM in the kernel log of the run.
#              For anything but ok the last 30 lines of the run go to <run>/last30.log.
set -u
DUR=300; OUT=/tmp/bench_oot; B=build-claude; RETRY=0; RESUM=
while getopts "d:o:b:rs:" o; do case $o in d) DUR=$OPTARG;; o) OUT=$OPTARG;; b) B=$OPTARG;; r) RETRY=1;; s) RESUM=$OPTARG;; *) sed -n '2,15p' "$0"; exit 2;; esac; done
shift $((OPTIND - 1))
[ $# -gt 0 ] || [ -n "$RESUM" ] || { sed -n '2,15p' "$0"; exit 2; }
cd "$(dirname "$0")/.." || exit 1
MIN_AVAIL_MB=${MIN_AVAIL_MB:-8000}; MAX_WAIT_S=${MAX_WAIT_S:-600}; KILL_AVAIL_MB=${KILL_AVAIL_MB:-600}; HANG_S=${HANG_S:-40}; HANG_KILL_S=${HANG_KILL_S:-120}
TITLE_T0=${TITLE_T0:-60}; TITLE_T1=${TITLE_T1:-150}; WORLD_FRAME=${WORLD_FRAME:-2000}; RETRY_BEFORE_S=${RETRY_BEFORE_S:-60}
[ -n "$RESUM" ] && OUT=$RESUM
mkdir -p "$OUT"
CSV=$OUT/results.csv
HEADER="label,build,env,dur_s,status,run_s,last_frame,fps_title,fps_world,submit_title_ms,submit_world_ms,rp_title,rp_world,oneshot_ms,allocs_max,peak_rss_swap_mb,peak_gtt_mb,min_avail_mb,errors"
[ -n "$RESUM" ] && rm -f "$CSV"
[ -f "$CSV" ] || echo "$HEADER" > "$CSV"

# The AMD card's GTT counter (textures of the iGPU live in system RAM, so this is RAM the run takes)
GTT=; for c in /sys/class/drm/card*/device; do [ "$(cat "$c/vendor" 2>/dev/null)" = 0x1002 ] && [ -r "$c/mem_info_gtt_used" ] && GTT=$c/mem_info_gtt_used && break; done
avail_mb() { awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo; }

wait_turn() {    # no other nxlinux, then enough free memory; returns 1 after MAX_WAIT_S of waiting for memory
    while pgrep -x nxlinux >/dev/null; do sleep 10; done
    local waited=0
    while [ "$(avail_mb)" -lt "$MIN_AVAIL_MB" ]; do
        [ $waited -ge "$MAX_WAIT_S" ] && return 1
        [ $((waited % 60)) -eq 0 ] && echo "  waiting for memory: $(avail_mb) MB available, need $MIN_AVAIL_MB" >&2
        sleep 10; waited=$((waited + 10))
        while pgrep -x nxlinux >/dev/null; do sleep 10; done
    done
    return 0
}

# Lines kept from stderr: the metrics and anything that looks like a failure
KEEP='\[longframe\]|timing up to frame|replay per frame|one-shot submissions|placed|SIGSEGV|general protection|[Aa]bort|failed|GPUVM|DEVICE_LOST|fault|panic|FAIL|Killed'

run_one() {    # $1 run dir, $2 build dir, $3 env assignments, $4 duration; leaves status, run_s, samples.csv, metrics.log, tail.log there
    local d=$1 build=$2 envs=$3 dur=$4
    rm -rf "$d"; mkdir -p "$d"
    printf '%s\n' "${d##*/}" "$build" "$envs" "$dur" > "$d/config"    # label, build, env, duration: what -s reads back
    local since; since=$(date '+%Y-%m-%d %H:%M:%S')
    # Defaults of tools/run_oot.sh, offscreen, A pressed every 6 s to skip the movies; the configuration's assignments override them.
    # After every line of output the line count goes to <run>/lines (the hang check); the last 300 lines go to tail.log at the end.
    # shellcheck disable=SC2086
    env VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation NX_JIT_CACHE_MB=48 NX_HEAP_MB=12000 NX_PRESENT=sdl SDL_VIDEODRIVER=offscreen NX_HID_TAP=0:6 $envs \
        "$build/nxlinux" --manifest work/run/zelda_oot_probe/modules.txt --romfs work/run/zelda_oot_probe/extract/RomFS.img \
        --sys-resource 0x3000000 --shaders work/run/ounce_all8.nxsl \
        > >(tee >(tail -n 300 > "$d/tail.log") >(awk -v hb="$d/lines" '{ print NR > hb; close(hb) }') \
            | grep --line-buffered -aE "$KEEP" > "$d/metrics.log") 2>&1 &
    local pid=$! t0; t0=$(date +%s)
    echo "$pid" > "$d/pid"
    echo "t_s,avail_mb,rss_mb,swap_mb,gtt_mb" > "$d/samples.csv"
    local status=ok el a rss sw g n last_n=-1 last_change=0
    while :; do
        sleep 5; el=$(( $(date +%s) - t0 ))
        if ! kill -0 "$pid" 2>/dev/null; then wait "$pid"; status="exit:$?"; break; fi
        a=$(avail_mb)
        rss=$(awk '/^VmRSS:/ { print int($2 / 1024) }' "/proc/$pid/status" 2>/dev/null); sw=$(awk '/^VmSwap:/ { print int($2 / 1024) }' "/proc/$pid/status" 2>/dev/null)
        g=0; [ -n "$GTT" ] && g=$(( $(cat "$GTT") / 1048576 ))
        echo "$el,$a,${rss:-0},${sw:-0},$g" >> "$d/samples.csv"
        if [ "$a" -lt "$KILL_AVAIL_MB" ]; then kill -9 "$pid"; wait "$pid" 2>/dev/null; status=oom-risk; break; fi
        n=$(cat "$d/lines" 2>/dev/null || echo 0)
        if [ "$n" != "$last_n" ]; then last_n=$n; last_change=$el
        elif [ $((el - last_change)) -ge "$HANG_KILL_S" ]; then kill -9 "$pid"; wait "$pid" 2>/dev/null; status=hang; break
        elif [ $((el - last_change)) -ge "$HANG_S" ]; then echo "$el s: no output for $((el - last_change)) s" >> "$d/stalls"; fi
        if [ $el -ge "$dur" ]; then kill -TERM "$pid"; sleep 3; kill -0 "$pid" 2>/dev/null && kill -9 "$pid"; wait "$pid" 2>/dev/null; break; fi
    done
    echo $(( $(date +%s) - t0 )) > "$d/run_s"
    sleep 2    # let the tee/tail pipeline flush
    journalctl -k --no-pager --since "$since" 2>/dev/null | grep -aE "nxlinux|amdgpu|GPUVM|general protection|Out of memory|oom" > "$d/kernel.log"
    echo "$status" > "$d/status"
}

summarize() {    # $1 label, $2 build, $3 env, $4 run dir, $5 duration; prints a CSV row
    python3 -I - "$@" "$TITLE_T0" "$TITLE_T1" "$WORLD_FRAME" "$RETRY_BEFORE_S" <<'EOF'
import re, sys, statistics as st
label, build, env, d, dur = sys.argv[1:6]; dur = int(dur)
t0, t1, wf, early = float(sys.argv[6]), float(sys.argv[7]), int(sys.argv[8]), float(sys.argv[9])
lf, sub, osm, alloc, err = [], [], [], [], 0
cur = 0          # frame of the last "timing up to frame N" line: the replay line printed after it covers the frames up to N
pat_err = re.compile(r"SIGSEGV|general protection|[Aa]bort(?!Observer)|vkQueueSubmit2 failed|GPUVM|DEVICE_LOST|mapping failed")
for line in open(d + "/metrics.log", errors="replace"):
    m = re.search(r"\[longframe\] frame (\d+) \(([\d.]+) s\)", line)
    if m: lf.append((int(m[1]), float(m[2]))); continue
    m = re.search(r"timing up to frame (\d+)", line)
    if m: cur = int(m[1]); continue
    m = re.search(r"replay per frame: ([\d.]+) render passes.*?submit ([\d.]+) ms", line)
    if m: sub.append((cur, float(m[2]), float(m[1]))); continue
    m = re.search(r"one-shot submissions.*?([\d.]+) ms each; (\d+) live", line)
    if m: osm.append(float(m[1])); alloc.append(int(m[2])); continue
    if pat_err.search(line): err += 1
try: err += sum(1 for l in open(d + "/kernel.log", errors="replace") if re.search(r"segfault|general protection|GPUVM|Out of memory|oom-kill", l))
except OSError: pass
def fps(pts):
    if len(pts) < 2 or pts[-1][1] <= pts[0][1]: return ""
    return "%.2f" % ((pts[-1][0] - pts[0][0]) / (pts[-1][1] - pts[0][1]))
title = [p for p in lf if t0 <= p[1] <= t1]
world = [p for p in lf if p[0] >= wf]
med = lambda v: "%.1f" % st.median(v) if v else ""
peak = gtt = 0; mina = 10**9
for i, l in enumerate(open(d + "/samples.csv")):
    if i == 0: continue
    t, a, r, s, g = map(int, l.split(","))
    peak = max(peak, r + s); gtt = max(gtt, g); mina = min(mina, a)
status = open(d + "/status").read().strip(); run_s = int(open(d + "/run_s").read())
if status.startswith("exit") and run_s < dur:
    status = "early-abort" if run_s < early else ("crash" if err else status)
last = lf[-1][0] if lf else 0
row = [label, build, env.replace(",", ";"), str(dur), status, str(run_s), str(last), fps(title), fps(world),
       med([s for f, s, r in sub if f < wf]), med([s for f, s, r in sub if f >= wf]),
       med([r for f, s, r in sub if f < wf]), med([r for f, s, r in sub if f >= wf]), med(osm), str(max(alloc) if alloc else ""),
       str(peak), str(gtt), str(mina if mina < 10**9 else ""), str(err)]
print(",".join(row))
EOF
}

if [ -n "$RESUM" ]; then    # re-summarize every run directory that has a config file
    for d in "$OUT"/*/; do d=${d%/}; [ -f "$d/config" ] && [ -f "$d/status" ] || continue
        { read -r l; read -r b; read -r e; read -r u; } < "$d/config"; summarize "$l" "$b" "$e" "$d" "$u" >> "$CSV"; done
    set --
fi
for cfg in "$@"; do
    label=${cfg%% *}; rest=; [ "$cfg" != "$label" ] && rest=${cfg#* }
    build=$B; dur=$DUR; envs=
    for w in $rest; do case $w in BUILD=*) build=${w#BUILD=};; DUR=*) dur=${w#DUR=};; *) envs="$envs $w";; esac; done
    envs=${envs# }
    [ -x "$build/nxlinux" ] || { echo "skip $label: no $build/nxlinux" >&2; continue; }
    for attempt in 1 2; do
        if ! wait_turn; then echo "stopped: less than $MIN_AVAIL_MB MB available after $MAX_WAIT_S s ($(avail_mb) MB)" >&2; break 2; fi
        echo "== $label ($build; $envs) attempt $attempt, ${dur}s, $(avail_mb) MB available, $(date +%T)" >&2
        d=$OUT/$label; [ $attempt -gt 1 ] && d=$d.$attempt
        run_one "$d" "$build" "$envs" "$dur"
        row=$(summarize "$label" "$build" "$envs" "$d" "$dur")
        echo "$row" >> "$CSV"
        echo "   $row" >&2
        case $row in *,ok,*) ;; *) tail -n 30 "$d/tail.log" > "$d/last30.log";; esac
        case $row in *,early-abort,*) [ $RETRY = 1 ] && [ $attempt = 1 ] && continue;; esac
        break
    done
done

echo "== ranking by title fps (then world fps), $CSV" >&2
{ head -n 1 "$CSV"; tail -n +2 "$CSV" | sort -t, -k8,8gr -k9,9gr; } | column -s, -t
