from pathlib import Path
import csv,json,hashlib
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from analysis import paired,holm
root=Path(__file__).resolve().parent;out=root/'results';paper=root/'paper'
(paper/'figures').mkdir(parents=True,exist_ok=True)
def read(n):return json.loads((out/(n+'.json')).read_text())
rows=[]
for study in ['primary','confirmation']:
    data=read(study);vs=data['variants'];base=vs[0]
    family=[]
    for v in vs:
        np.testing.assert_allclose(np.asarray(v['pnl']),np.asarray(v['edge'])+np.asarray(v['inventory'])-np.asarray(v['fees']),atol=3e-6)
        if v is base:continue
        stats=paired(v['pnl'],base['pnl']);stats.update(study=study,variant=v['name'],pnl_mean=float(np.mean(v['pnl'])),max_position=max(v['max_pos']))
        assert abs(stats['mean_difference']-v['vs_base']['mean'])<3e-6
        assert abs(stats['sign_p']-v['vs_base']['sign_p'])<1e-6
        family.append(stats)
    for row,adjusted in zip(family,holm([x['sign_p'] for x in family])):
        row['holm_sign_p']=float(adjusted);rows.append(row)
with (out/'paired_statistics.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=rows[0].keys(),lineterminator='\n');w.writeheader();w.writerows(rows)
# Preserve all regime/sensitivity outcomes, with a prespecified 28-test regime family.
robust=[]
regimes=read('regimes')['regimes']
regime_tests=[]
for regime in regimes:
    vs=regime['variants']
    for v in vs[1:]:
        stats=paired(v['pnl'],vs[0]['pnl']);stats.update(regime=regime['name'],variant=v['name'])
        regime_tests.append(stats)
for row,padj in zip(regime_tests,holm([r['sign_p'] for r in regime_tests])):row['holm_sign_p']=float(padj)
pd.DataFrame(regime_tests).to_csv(out/'regime_statistics.csv',index=False)
for study,key,axis in [('latency','latencies','us'),('queue','queue_penalties','pct')]:
    for item in read(study)[key]:
        for v in item['variants']:
            robust.append(dict(study=study,axis=item[axis],variant=v['name'],pnl=np.mean(v['pnl']),edge=np.mean(v['edge']),inventory=np.mean(v['inventory']),max_position=max(v['max_pos'])))
for fee in [1,5]:
    for v in read(f'fee_{fee}')['variants']:
        robust.append(dict(study='fee_bps',axis=fee,variant=v['name'],pnl=np.mean(v['pnl']),edge=np.mean(v['edge']),inventory=np.mean(v['inventory']),max_position=max(v['max_pos'])))
pd.DataFrame(robust).to_csv(out/'sensitivity_statistics.csv',index=False)
primary=read('primary')['variants']
fig,ax=plt.subplots(figsize=(4.4,3))
ax.bar([v['name'] for v in primary],[np.mean(v['pnl']) for v in primary],yerr=[np.std(v['pnl'],ddof=1)/4 for v in primary],capsize=3)
ax.set(ylabel='Synthetic PnL mean ± SE',xlabel='Prespecified variant');ax.tick_params(axis='x',labelrotation=20);fig.tight_layout();fig.savefig(paper/'figures/ablation.pdf');plt.close(fig)
lat=read('latency');print('latency keys',lat.keys())
# Sweep JSON names the array 'latencies'; each slice carries variants.
items=lat['latencies']
fig,ax=plt.subplots(figsize=(4.4,3))
for index in [0,1,3]:
    ax.plot([x['us'] for x in items],[np.mean(x['variants'][index]['pnl']) for x in items],'o-',label=items[0]['variants'][index]['name'])
ax.set(xlabel='Delay on each of three hops (µs)',ylabel='Synthetic mean PnL');ax.legend();fig.tight_layout();fig.savefig(paper/'figures/latency.pdf');plt.close(fig)
lines=['\\begin{tabular}{lrrr}\\hline Variant & Paired gain & 95\\% CI & Holm $p$\\\\\\hline']
for r in rows[:4]:
    lines.append(f"{r['variant']} & {r['mean_difference']:.2f} & [{r['t_ci_low']:.2f},{r['t_ci_high']:.2f}] & {r['holm_sign_p']:.4g}\\\\")
lines.append('\\hline\\end{tabular}');(paper/'paired_table.tex').write_text('\n'.join(lines))
bench=[read(f'benchmark_{i}') for i in range(5)]
baseline=pd.read_csv(out/'book_baseline.txt')
assert baseline.groupby('seed').digest.nunique().eq(1).all()
ratios=baseline.pivot(index='seed',columns='method',values='ns_per_event')
ratios['speedup']=ratios.map_list_reference/ratios.flat
ratios.to_csv(out/'book_baseline_summary.csv')
summary=dict(book_speedup_median=float(ratios.speedup.median()),p50_ns=[x['t2t_hot']['p50'] for x in bench],p99_ns=[x['t2t_hot']['p99'] for x in bench],throughput_million=[1000/x['ns_per_input'] for x in bench],primary=rows[:4],confirmation=rows[4:])
(out/'summary.json').write_text(json.dumps(summary,indent=2))
(paper/'numbers.tex').write_text(f"\\newcommand{{\\BookSpeedup}}{{{summary['book_speedup_median']:.2f}}}\n\\newcommand{{\\HotMedianRange}}{{{min(summary['p50_ns']):.1f}--{max(summary['p50_ns']):.1f}}}\n\\newcommand{{\\HotPninetyRange}}{{{min(summary['p99_ns']):.1f}--{max(summary['p99_ns']):.1f}}}\n\\newcommand{{\\ThroughputRange}}{{{min(summary['throughput_million']):.2f}--{max(summary['throughput_million']):.2f}}}\n")
print(json.dumps(summary,indent=2))

# The derived summary is written after run_experiments records raw-file hashes.
# Refresh its hash only after generation, and identify this executed report code.
metadata_path=out/'metadata.json'
metadata=json.loads(metadata_path.read_text())
metadata['output_hashes']['summary.json']=hashlib.sha256((out/'summary.json').read_bytes()).hexdigest()
metadata['source_hashes']['research/build_report.py']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
metadata_path.write_text(json.dumps(metadata,indent=2))
