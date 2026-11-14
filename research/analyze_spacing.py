"""Aggregate the inter-arrival spacing study and cross-host timing; writes CSV, figure, table, macros."""
from pathlib import Path
import json, statistics as st
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
root = Path(__file__).resolve().parent
res = root / 'results'; paper = root / 'paper'
hostdir = next((res / 'spacing').iterdir())
runs = [json.loads((hostdir / f'benchmark_{i}.json').read_text()) for i in range(5)]
meta = json.loads((hostdir / 'metadata.json').read_text())
gaps = [r['gap_us'] for r in runs[0]['gap_sweep']]
rows = []
for k, g in enumerate(gaps):
    pts = [r['gap_sweep'][k] for r in runs]
    ratio_idle = [p['idle_p50'] / p['idle_ctl_p50'] for p in pts]
    ratio_act = [p['act_p50'] / p['act_ctl_p50'] for p in pts]
    rows.append(dict(gap_us=g, n_inputs=pts[0]['n_inputs'], n_idle=pts[0]['n_idle'], n_act=pts[0]['n_act'],
                     idle_ctl_p50=st.median(p['idle_ctl_p50'] for p in pts), idle_p50=st.median(p['idle_p50'] for p in pts),
                     idle_p50_min=min(p['idle_p50'] for p in pts), idle_p50_max=max(p['idle_p50'] for p in pts),
                     idle_p99=st.median(p['idle_p99'] for p in pts),
                     act_ctl_p50=st.median(p['act_ctl_p50'] for p in pts), act_p50=st.median(p['act_p50'] for p in pts),
                     idle_ratio=st.median(ratio_idle), idle_ratio_min=min(ratio_idle), idle_ratio_max=max(ratio_idle),
                     act_ratio=st.median(ratio_act),
                     idle_warm_p50=st.median(p.get('idle_warm_p50', 0.0) for p in pts), act_warm_p50=st.median(p.get('act_warm_p50', 0.0) for p in pts),
                     idle_warm_ratio=st.median((p.get('idle_warm_p50', 0.0) / p['idle_ctl_p50']) for p in pts)))
import csv
with (res / 'spacing_summary.csv').open('w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
# figure
fig, ax = plt.subplots(figsize=(3.4, 2.5))
xs = [r['gap_us'] for r in rows[1:]]
ax.errorbar(xs, [r['idle_p50'] for r in rows[1:]], yerr=[[r['idle_p50'] - r['idle_p50_min'] for r in rows[1:]], [r['idle_p50_max'] - r['idle_p50'] for r in rows[1:]]], marker='o', label='no-order tick, p50')
ax.plot(xs, [r['act_p50'] for r in rows[1:]], marker='s', label='order tick, p50')
ax.plot([r['gap_us'] for r in rows if r['gap_us'] >= 5], [r['idle_warm_p50'] for r in rows if r['gap_us'] >= 5], marker='^', ls='--', label='no-order tick, warmed')
ax.axhline(rows[0]['idle_p50'], ls=':', color='gray', label='back-to-back no-order p50')
ax.set(xscale='log', yscale='log', xlabel='Idle gap before each tick (\\textmu s)', ylabel='Latency (ns)'); ax.legend(fontsize=6); fig.tight_layout()
fig.savefig(paper / 'figures' / 'spacing.pdf'); plt.close(fig)
# table
L = ['\\begin{tabular}{rrrrrr}\\hline Gap (\\textmu s) & $N$ & No-order p50 (ns) & vs.\\ control & Warmed (ns) & Order p50 (ns)\\\\\\hline']
for r in rows[1:]:
    warm = f"{r['idle_warm_p50']:.0f}" if r['gap_us'] >= 5 else '--'
    L.append(f"{r['gap_us']:.0f} & {r['n_inputs']} & {r['idle_p50']:.0f} & {r['idle_ratio']:.1f}$\\times$ & {warm} & {r['act_p50']:.0f}\\\\")
L.append('\\hline\\end{tabular}'); (paper / 'spacing_table.tex').write_text('\n'.join(L))
# cross-host hot timings
A = json.loads((res / 'summary.json').read_text())
B = dict(p50=[r['t2t_hot']['p50'] for r in runs], p99=[r['t2t_hot']['p99'] for r in runs], thr=[1000 / r['ns_per_input'] for r in runs])
import pandas as pd
bb = pd.read_csv(hostdir / 'book_baseline.txt'); 
assert bb.groupby('seed').digest.nunique().eq(1).all()
rr = bb.pivot(index='seed', columns='method', values='ns_per_event'); sp = (rr.map_list_reference / rr.flat)
macros = {
 'SpacingMaxRatio': f"{max(r['idle_ratio'] for r in rows):.0f}", 'SpacingOneMsRatio': f"{[r for r in rows if r['gap_us']==1000][0]['idle_ratio']:.0f}",
 'SpacingHundredRatio': f"{[r for r in rows if r['gap_us']==100][0]['idle_ratio']:.1f}",
 'HostBHotMedianRange': f"{min(B['p50']):.1f}--{max(B['p50']):.1f}", 'HostBHotPninetyRange': f"{min(B['p99']):.1f}--{max(B['p99']):.1f}",
 'HostBThroughputRange': f"{min(B['thr']):.2f}--{max(B['thr']):.2f}", 'HostBBookSpeedup': f"{sp.median():.2f}",
 'HostBCpu': meta['cpu'].replace('(R)','').replace('Processor','').strip(),
}
(paper / 'spacing_numbers.tex').write_text(''.join(f"\\newcommand{{\\{k}}}{{{v}}}\n" for k, v in macros.items()))
(res / 'spacing_cross_host.json').write_text(json.dumps(dict(hostA_summary=A['p50_ns'], hostA_p99=A['p99_ns'], hostB=B, hostB_book_speedup_median=float(sp.median()), hostB_meta=meta), indent=1))
print(json.dumps(macros, indent=1)); print(csv_text := (res / 'spacing_summary.csv').read_text())
