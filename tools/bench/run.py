#!/usr/bin/env python3
"""The standard benchmark (GAME-GOALS, "Remove the overhead"): a game run on its fixed scripted
route, with frame times, CPU per thread, GPU time and power recorded, on a quiet machine.

    run.py --out DIR [--secs N] [--skip S] [--cwd DIR] [--env K=V ...] -- GAME [ARGS...]

The game supplies its route (its pad script in --env, as its check does). Writes DIR/ft.csv (the
runtime's RECOMP_TRACE=frametime), DIR/run.log, DIR/samples.csv (CPU per thread and watts, twice
a second) and DIR/summary.json, and prints the summary line for the handoff doc. Frames before
--skip seconds (start-up, loading) aren't counted.

Power: amdgpu's PPT (the whole chip: CPU and GPU) where there is one, and the battery's draw when it
is discharging (on AC it reads 0, so a run for battery watts goes unplugged). GPU time is the
renderer's own, from ft.csv's gpu_ms (0 where a renderer doesn't report it)."""
import argparse, csv, glob, json, os, statistics, subprocess, sys, time

def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None

def ppt_path():
    for h in glob.glob('/sys/class/hwmon/hwmon*'):
        if read(h + '/name') == 'amdgpu':
            for k in ('power1_average', 'power1_input'):
                if os.path.exists(f'{h}/{k}'):
                    return f'{h}/{k}'
    return None

def battery_w():
    for b in glob.glob('/sys/class/power_supply/BAT*'):
        if read(b + '/status') != 'Discharging':
            return None
        p = read(b + '/power_now')
        if p:
            return int(p) / 1e6
        i, v = read(b + '/current_now'), read(b + '/voltage_now')
        if i and v:
            return int(i) * int(v) / 1e12
    return None

def thread_ticks(pid):
    out = {}
    for t in glob.glob(f'/proc/{pid}/task/*'):
        st = read(t + '/stat')
        if not st:
            continue
        name = st[st.index('(') + 1:st.rindex(')')]
        f = st[st.rindex(')') + 2:].split()
        out[(int(os.path.basename(t)), name)] = int(f[11]) + int(f[12])
    return out

def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100.0 * (len(xs) - 1))))]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--secs', type=float, default=120)
    ap.add_argument('--skip', type=float, default=20)
    ap.add_argument('--cwd')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('cmd', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == '--' else a.cmd
    if not cmd:
        sys.exit('no game command')
    os.makedirs(a.out, exist_ok=True)
    ft = os.path.abspath(os.path.join(a.out, 'ft.csv'))
    env = dict(os.environ)
    for kv in a.env:
        k, v = kv.split('=', 1)
        env[k] = v
    trace = env.get('RECOMP_TRACE', '')
    env['RECOMP_TRACE'] = (trace + ',' if trace else '') + 'frametime=' + ft
    hz = os.sysconf('SC_CLK_TCK')
    ppt = ppt_path()
    log = open(os.path.join(a.out, 'run.log'), 'w')
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, cwd=a.cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
    samples, first, last, watts, bat = [], None, None, [], []
    with open(os.path.join(a.out, 'samples.csv'), 'w', newline='') as sf:
        w = csv.writer(sf)
        w.writerow(['t', 'ppt_w', 'battery_w', 'cpu_pct'])
        while p.poll() is None and time.monotonic() - t0 < a.secs:
            time.sleep(0.5)
            t = time.monotonic() - t0
            ticks = thread_ticks(p.pid)
            pw = int(read(ppt)) / 1e6 if ppt and read(ppt) else None
            bw = battery_w()
            if t >= a.skip:
                if first is None:
                    first = (t, ticks)
                last = (t, ticks)
                if pw is not None:
                    watts.append(pw)
                if bw is not None:
                    bat.append(bw)
            cpu = sum(ticks.values()) / hz
            w.writerow([f'{t:.1f}', pw, bw, f'{cpu:.2f}'])
    if p.poll() is None:
        p.send_signal(2)
        try:
            p.wait(20)
        except subprocess.TimeoutExpired:
            p.kill()
    log.close()
    # Frames after the skip.
    frames, late, gpu = [], 0, []
    try:
        with open(ft) as f:
            for r in csv.DictReader(f):
                if float(r.get('t_ms', 0)) < a.skip * 1000:
                    continue
                frames.append(float(r['frame_ms']))
                g = float(r.get('gpu_ms') or 0)
                if g > 0:
                    gpu.append(g)
    except (OSError, KeyError, ValueError) as e:
        print(f'bench: no frame times ({e})', file=sys.stderr)
    # A late frame: half as long again as the run's usual one (ft.csv's own `late` is the swap
    # missing its next vblank, which a 30 fps title does every frame by design).
    usual = statistics.median(frames) if frames else 0
    late = sum(1 for f in frames if f > usual * 1.5)
    threads = []
    if first and last and last[0] > first[0]:
        span = last[0] - first[0]
        for k, v in last[1].items():
            d = (v - first[1].get(k, v)) / hz / span * 100
            if d >= 1:
                threads.append((round(d, 1), k[1]))
        threads.sort(reverse=True)
    s = {
        'frames': len(frames),
        'frame_ms_mean': round(statistics.mean(frames), 2) if frames else None,
        'frame_ms_p95': round(pct(frames, 95), 2),
        'frame_ms_p99': round(pct(frames, 99), 2),
        'frame_ms_max': round(max(frames), 2) if frames else None,
        'frame_ms_usual': round(usual, 2),
        'late_pct': round(100.0 * late / len(frames), 1) if frames else None,
        'cpu_pct_total': round(sum(d for d, _ in threads), 1),
        'cpu_pct_threads': [f'{n} {d}' for d, n in threads[:8]],
        'gpu_ms_mean': round(statistics.mean(gpu), 2) if gpu else None,
        'ppt_w_mean': round(statistics.mean(watts), 1) if watts else None,
        'battery_w_mean': round(statistics.mean(bat), 1) if bat else None,
        'secs_counted': round(last[0] - first[0], 1) if first and last else 0,
    }
    json.dump(s, open(os.path.join(a.out, 'summary.json'), 'w'), indent=1)
    print(f"bench: {s['frames']} frames, mean {s['frame_ms_mean']} ms, p95 {s['frame_ms_p95']}, "
          f"p99 {s['frame_ms_p99']}, worst {s['frame_ms_max']}, late {s['late_pct']}% (over 1.5x {s['frame_ms_usual']}); "
          f"CPU {s['cpu_pct_total']}% ({', '.join(s['cpu_pct_threads'][:4])}); "
          f"GPU {s['gpu_ms_mean']} ms; chip {s['ppt_w_mean']} W, battery {s['battery_w_mean']} W")

main()
