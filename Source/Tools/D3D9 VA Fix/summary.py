"""Condensed view of d3d9_memlog.log: one row per changed state, plus peaks.  python summary.py <log> [--all]"""
import re
import sys

# Mirrors the tick line written by Report() in d3d9_memlog.cpp.
PAT = re.compile(
    r'^(?P<time>\S+) (?P<event>.{13}).*VA commit\s+(?P<commit>\d+) private\s+(?P<private>\d+)'
    r' free\s+(?P<free>\d+) largest\s+(?P<largest>\d+)'
    r'.*tex MANAGED\s+(?P<mtex_n>\d+)\s+(?P<mtex_mb>\d+) MB\s+DEFAULT\s+(?P<dtex_n>\d+)\s+(?P<dtex_mb>\d+) MB'
    r' \(RT/DS\s+(?P<rt_mb>\d+)\).*buf MANAGED\s+(?P<mbuf_mb>\d+)'
    r'.*locks M (?P<locks>\d+) relock (?P<relock>\d+) RO (?P<ro>\d+) partial (?P<partial>\d+)'
    r'.*\| resets (?P<resets>\d+) fails (?P<fails>\d+)')
rows = [m.groupdict() for m in map(PAT.match, open(sys.argv[1], errors='replace')) if m]
show_all = '--all' in sys.argv
print("time     event         commit  priv  free largest | Mtex N    MB | Dtex N    MB  RT/DS | Mbuf | locksM relock    RO partial | resets")
prev = None
for i, r in enumerate(rows):
    state = (r['commit'], r['largest'], r['mtex_n'], r['mtex_mb'], r['resets'])
    if show_all or i in (0, len(rows) - 1) or state != prev or 'Reset' in r['event']:
        print(f"{r['time']} {r['event']} {r['commit']:>6} {r['private']:>5} {r['free']:>5} {r['largest']:>6} | "
              f"{r['mtex_n']:>6} {r['mtex_mb']:>5} | {r['dtex_n']:>6} {r['dtex_mb']:>5} {r['rt_mb']:>6} | "
              f"{r['mbuf_mb']:>4} | {r['locks']:>6} {r['relock']:>6} {r['ro']:>5} {r['partial']:>7} | {r['resets']}")
    prev = state
if rows:
    peak = lambda key: max(int(r[key]) for r in rows)
    low = lambda key: min(int(r[key]) for r in rows)
    print(f"peak commit {peak('commit')} MB, min free {low('free')} MB, min largest {low('largest')} MB, "
          f"peak MANAGED tex {peak('mtex_mb')} MB, peak MANAGED buf {peak('mbuf_mb')} MB, "
          f"resets {rows[-1]['resets']}, fails {rows[-1]['fails']}")
