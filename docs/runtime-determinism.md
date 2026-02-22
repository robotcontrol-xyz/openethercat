# Runtime Determinism Guide (Phase 2)

This guide documents practical Linux runtime settings for stable EtherCAT cycle timing and distributed-clock behavior.

## Goals

- Reduce cycle wakeup jitter.
- Reduce scheduler preemption latency.
- Avoid page-fault stalls during cyclic execution.
- Produce repeatable KPI evidence on x86 and RPi-class hardware.

## 1) Use a PREEMPT_RT kernel when available

Check kernel:

```bash
uname -a
```

Prefer a PREEMPT_RT kernel for final KPI campaigns. Standard kernels can be used for development but show higher jitter tails.

## 2) Pin process to isolated CPU core(s)

Use `taskset` to isolate your cyclic app:

```bash
sudo taskset -c 2 ./build/dc_soak_demo linux:enp2s0 600 1000
```

Recommendations:

- Reserve one core for EtherCAT cyclic thread(s) and IRQs.
- Keep non-realtime workloads off that core.

## 3) Run with realtime scheduler priority

Use `chrt`:

```bash
sudo chrt -f 80 taskset -c 2 ./build/dc_soak_demo linux:enp2s0 600 1000
```

Notes:

- `SCHED_FIFO` (`-f`) is typical for hard cyclic loops.
- Start with medium-high priorities and validate starvation risk for other services.

## 4) Lock memory to avoid page faults

Set memlock limits (shell/session):

```bash
ulimit -l unlimited
```

For permanent setup, configure `/etc/security/limits.conf` (or systemd unit limits):

- `memlock unlimited` for the runtime user/service.

## 5) NIC and power-management considerations

- Disable aggressive CPU frequency scaling for benchmark runs.
- Keep NIC on a stable link, avoid shared heavy traffic paths.
- Prefer wired dedicated interface for EtherCAT master traffic.

## 6) Collect KPI evidence with `dc_soak_demo`

Text mode:

```bash
sudo OEC_DC_CLOSED_LOOP=1 OEC_DC_SYNC_MONITOR=1 \
  ./build/dc_soak_demo linux:enp2s0 600 1000
```

JSON mode (for ingestion in CI/log pipelines):

```bash
sudo OEC_SOAK_JSON=1 OEC_DC_CLOSED_LOOP=1 OEC_DC_SYNC_MONITOR=1 \
  ./build/dc_soak_demo linux:enp2s0 600 1000
```

Key output fields:

- `runtime_p95_us`, `runtime_p99_us`, `runtime_max_us`
- `wake_jitter_p95_ns`, `wake_jitter_p99_ns`, `wake_jitter_max_ns`
- `dc_jitter_p95_ns`, `dc_jitter_p99_ns`, `dc_jitter_max_ns`
- `lock_duty`, `dc_lock_acq`, `dc_lock_loss`, `dc_policy_triggers`

## 7) Recommended Phase 2 evidence set

Run and archive:

1. x86 reference chain: 10 min, 1 h, 24 h
2. RPi reference chain: 10 min, 1 h, 24 h
3. Two periods minimum (example: `1000us`, `2000us`)
4. Same workload profile across runs

For each run, store:

- command line
- kernel/version info
- hardware/NIC details
- JSON summary line
- pass/fail against your target thresholds
