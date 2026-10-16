# Latency budget

Every number below was measured in this repository on one machine: a shared 2-vCPU Xeon at 2.8 GHz (32K L1d, 1M L2 per core), gcc 13.3, `perf` preset (`-O3 -march=x86-64-v3`). It is a virtual machine on shared hardware and the data is a synthetic market, so these are indicative, not a bare-metal claim. Reproduce with the commands at the bottom.

## Headline

Tick-to-trade is packet bytes handed to the pipeline until the last reaction frame is staged. Timer overhead (13.9 ns per rdtsc+lfence pair) is subtracted.

| Case (hot caches, packets back to back) | p50 | p99 | p99.9 |
|---|---|---|---|
| tick-to-trade, first frame a replace (7200 of 7233) | 128-160 ns | 234-380 ns | 500-560 ns |
| tick-to-trade, first frame a new order (33) | 305 ns | 845 ns | |
| tick-to-decision, no order sent | 72 ns | 144-151 ns | 370 ns |
| report-to-decision | 58 ns | 208 ns | 525 ns |
| duplicate packet from line B dropped | 24 ns | 46 ns | 135 ns |
| sustained, no probes | 74-78 ns per input | | |

Ranges are the spread over runs on the same VM. The first runs of the session gave the low end (128 ns p50); later runs, with the release and perf builds back to back, gave about 157-161 ns p50, p99 340-380 ns, so the VM itself moved by about 25%. A short run (100,000 requests, 2414 order ticks) gave 248 ns p50 and should not be used. Compare builds only back to back, as the A/B tables in DESIGN.md do.

| Cold L1 and L2 (4 MiB walk before each tick, data in L3) | p50 | p99 |
|---|---|---|
| tick-to-trade | about 0.97 us (one run 1.4 us) | 2.1-2.9 us |
| tick-to-decision | 405 ns | about 1.0 us |

Use the cold row for a tick that follows quiet time. The hot rows are a best case.

## Per-stage split

Staged run, ticks that sent an order. The probes cost 50-100 ns in total (the staged end-to-end p50 minus the unprobed p50, different each run), so read the stages as proportions.

| Stage | p50 probed | share | cycles at 2.8 GHz, scaled to the 128 ns headline |
|---|---|---|---|
| feed handler: parse, arbitrate, book | 66 ns | 34% | about 120 |
| strategy: signal and quote | 67 ns | 34% | about 120 |
| gateway: risk, OMS, encode, stage | 53 ns | 27% | about 97 |
| second wire side | 21 ns | | |

Instruction share per input (callgrind, 797.8 instructions per input on the gate workload): book 32.5%, strategy 30.3%, feed handler 23.9%, pipeline glue 7.2%, gateway/risk/OMS/codec 3.0%, memory and clock 2.8%.

## Cache and branch (SIMULATED)

No hardware counters in the sandbox. These are callgrind's model (L1 32K 8-way, LL 1 MiB 16-way standing in for L2), per input: 2.95 branch mispredicts, 2.26 D1 misses, 1.27 L2-model misses. About 60% of the L2-model misses are the first touch of the packet bytes, which no software change removes.

## What is not in the number

NIC receive and DMA, kernel, PCIe, the exchange, the wire, TCP/UDP stack, and software outside the pipeline (logging, risk dashboards). An end-to-end figure including these would be much larger and has not been measured.

## Noise

A 1 s spin loop on this VM is interrupted for more than 1 us about 2700 times per second. The p99.9 and max columns are dominated by that, not by the code.

## Book capacity is a latency parameter

Same session, same wire bytes, hot caches:

| capacity | id table | t2t p50 | t2t p99 |
|---|---|---|---|
| 1024 | 32 KiB | 127 ns | 254 ns |
| 4096 | 128 KiB | 128 ns | 265 ns |
| 16384 | 512 KiB | 128 ns | 288 ns |
| 65536 | 2 MiB | 138 ns | 388 ns |
| 262144 | 8 MiB | 160 ns | 474 ns |

Sizing rule used by the bench: capacity = 3 x peak resting orders, rounded up to a power of two, minimum 1024.

## Regression gate

CI compares callgrind instructions per input with `bench/baselines/t2t_instr.json` (797.80 now), failing above +2%. Instruction counts are deterministic, so the gate does not flake the way a wall-clock gate would on a shared runner.

## Reproduce

```
cmake --preset perf && cmake --build --preset perf
./build/perf/bench/bench_t2t --requests 300000 --cpu 1
python3 scripts/perf_gate.py --bench build/perf/bench/bench_t2t --strict
```
