"""Inter-arrival spacing study and second-host timing replication.

Runs, on whatever host executes it, five sequential bench_t2t repeats with --gap-sweep and the
matched book baseline. Output goes to results/spacing/<label>/ so the primary-host results in
results/ are never overwritten. Usage: python research/run_spacing_study.py <host-label>
Requires the release build (cmake --preset release && cmake --build --preset release).
"""
from pathlib import Path
import hashlib, json, platform, subprocess, sys

root = Path(__file__).resolve().parent
label = sys.argv[1] if len(sys.argv) > 1 else 'host'
out = root / 'results' / 'spacing' / label
out.mkdir(parents=True, exist_ok=True)
cfg = json.loads((root / 'config.json').read_text())
repo = root.parent


def run(args, name):
    with (out / (name + '.txt')).open('w') as f:
        subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, check=True, cwd=repo)


compile_cmd = ['g++', '-std=c++20', '-O3', '-march=native', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wshadow',
               '-Wconversion', '-Iinclude', '-Itests/support', 'research/book_baseline.cpp', '-o', 'build/release/bench/research_book_baseline']
run(compile_cmd, 'baseline_compile')
for rep in range(cfg['benchmark_repeats']):
    name = f'benchmark_{rep}'
    run(['./build/release/bench/bench_t2t', '--requests', str(cfg['benchmark_requests']), '--gap-sweep', '--json',
         str(out / (name + '.json'))], name)
    txt = (out / (name + '.txt')).read_text()
    assert 'verify         replay' in txt and ': OK' in txt, 'replay verification failed'
run(['./build/release/bench/research_book_baseline'], 'book_baseline')

cpu = next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name')), 'unknown')
meta = dict(label=label, cpu=cpu, python=platform.python_version(), kernel=platform.release(),
            compiler=subprocess.run(['g++', '--version'], capture_output=True, text=True).stdout.splitlines()[0],
            cpus_online=__import__('os').cpu_count(),
            binary_hashes={p: hashlib.sha256((repo / p).read_bytes()).hexdigest()
                           for p in ['build/release/bench/bench_t2t', 'build/release/bench/research_book_baseline']},
            output_hashes={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.glob('*.json'))})
(out / 'metadata.json').write_text(json.dumps(meta, indent=2))
print(json.dumps(meta, indent=2))
