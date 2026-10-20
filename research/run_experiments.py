"""Prespecified HFT experiments; requires release binaries, no exchange connection."""
from pathlib import Path
import hashlib,json,os,platform,subprocess,sys,time
root=Path(__file__).resolve().parent;out=root/'results';out.mkdir(exist_ok=True)
cfg=json.loads((root/'config.json').read_text());commands=[]
def run(args,name):
    commands.append(args)
    with (out/(name+'.txt')).open('w') as f:subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,check=True)

compile_cmd=['g++','-std=c++20','-O3','-march=native','-DNDEBUG','-Wall','-Wextra','-Wpedantic','-Werror','-Wshadow','-Wconversion','-Iinclude','-Itests/support','research/book_baseline.cpp','-o','build/release/bench/research_book_baseline']
run(compile_cmd,'baseline_compile')
# These timed runs are sequential, before loading the CPU with statistical lab work.
for rep in range(cfg['benchmark_repeats']):
    name=f'benchmark_{rep}'
    run(['./build/release/bench/bench_t2t','--requests',str(cfg['benchmark_requests']),'--json',str(out/(name+'.json'))],name)
    assert 'verify         replay' in (out/(name+'.txt')).read_text()
    assert ': OK' in (out/(name+'.txt')).read_text()
    data=json.loads((out/(name+'.json')).read_text())
    for k,v in data.items():
        if not isinstance(v,dict) or 'bins' not in v:continue
        assert sum(b['count'] for b in v['bins'])==v['n']>0
        # The histogram stores upper edges; quantiles are clipped to exact max.
        for q,key in [(.5,'p50'),(.99,'p99'),(.999,'p999')]:
            import math
            target=math.ceil(v['n']*q);count=0
            for b in v['bins']:
                count+=b['count']
                if count>=target:
                    estimate=min(b['upper_ticks']/v['ticks_per_ns'],v['max'])
                    assert abs(estimate-v[key])<.2
                    break

variants=['--variant','noalpha:alpha16=0','--variant','wide:half_spread16=32','--variant','noskew:skew16=0','--variant','strongskew:skew16=4']
def lab(name,extra,primary=False):
    args=['./build/release/hft_lab','--requests',str(cfg['requests'] if primary else cfg['robustness_requests']),'--markets',str(cfg['markets'] if primary else cfg['robustness_markets']),*extra,'--json',str(out/(name+'.json'))]
    run(args,name)
lab('primary',['--seed',str(cfg['primary_seed']),*variants],True)
lab('confirmation',['--seed',str(cfg['confirmation_seed']),*variants],True)
lab('regimes',['--seed','15000','--regimes','default,volatile,quiet,informed,uninformed,thin,slow',*variants])
lab('latency',['--seed','25000','--latency-sweep','0,5,20,50,100','--latency-axis','all',*variants])
lab('queue',['--seed','35000','--queue-sweep','0,50,100',*variants])
for fee in [1,5]:lab(f'fee_{fee}',['--seed','45000','--maker-fee-bps',str(fee),*variants])
lab('selection',['--seed','55000','--grid','alpha16=0,8,16;skew16=0,2','--top','2','--risk-penalty','1','--test-seed','155000','--test-markets','8'])
# Byte-exact deterministic repeats must reproduce the entire per-market JSON.
lab('primary_repeat',['--seed',str(cfg['primary_seed']),*variants],True)
assert json.loads((out/'primary.json').read_text())==json.loads((out/'primary_repeat.json').read_text())
run(['./build/release/bench/research_book_baseline'],'book_baseline')
meta=dict(config=cfg,commands=commands,python=sys.version,platform=platform.platform(),cpuinfo=Path('/proc/cpuinfo').read_text().split('\n\n')[0],compiler=subprocess.check_output(['g++','--version'],text=True),git_head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
meta['source_hashes']={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ['include','src','bench','tests','research'] for p in sorted(Path(folder).rglob('*')) if p.is_file() and p.suffix in ['.cpp','.hpp','.py']}
meta['binary_hashes']={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path('build/release/hft_lab'),Path('build/release/bench/bench_t2t'),Path('build/release/bench/research_book_baseline')]}
meta['output_hashes']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.glob('*.json')) if p.name!='metadata.json'}
meta['cmake_cache']=Path('build/release/CMakeCache.txt').read_text()
for n in ['cpu.max','memory.max']:meta[n]=Path('/sys/fs/cgroup',n).read_text().strip()
(out/'metadata.json').write_text(json.dumps(meta,indent=2))
print('All benchmarks, lab experiments, histogram checks and deterministic repeat passed.')
