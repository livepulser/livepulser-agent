# Metric definitions

All metrics describe the host the agent runs on, as seen from the host namespace. A report covers one window from the previous measurement to the current one. CPU and network are rates over that window. Memory, swap and disk are taken at the end of the window.

Each window is timed with the monotonic clock. `observed_at` is the UTC wall-clock time at the end of the window.

## CPU — `cpu_percent`

The rate is computed from the aggregate `cpu` line of `/proc/stat`:

```
total = user + nice + system + idle + iowait + irq + softirq + steal
idle  = idle + iowait
cpu   = 100 × (Δtotal − Δidle) / Δtotal
```

- `guest` and `guest_nice` are already counted inside `user` and `nice`, so they are not added a second time.
- The value is utilization across all CPUs, from 0 to 100. It is not the load average.
- The window is reported as unavailable when the number of `cpuN` lines changes (CPU hotplug), when any counter except `iowait` decreases, or when the aggregate total or idle decreases.
- The kernel documents that `iowait` can decrease, so a decrease in `iowait` alone does not invalidate the window.

## Memory — `memory_percent`

`100 × (MemTotal − MemAvailable) / MemTotal` from `/proc/meminfo`. `MemFree` is never used as a substitute. The value is unavailable when `MemAvailable` is missing, which happens on kernels older than 3.14.

## Swap — `swap_percent`

`100 × (SwapTotal − SwapFree) / SwapTotal`. On hosts without swap the value is reported as unavailable, not 0%.

## Disk — `disk_percent`

`statvfs(disk_path)`, where `disk_path` is set in the dashboard and defaults to `/`:

```
disk = 100 × (f_blocks − f_bfree) / f_blocks
```

This counts allocated blocks, including blocks reserved for root. It can therefore read slightly lower than the `Use%` column of `df`, which leaves reserved blocks out of its denominator. The value is unavailable when the path is invalid, inaccessible, or reports zero capacity.

## Network — `network_tx_bytes_per_second`, `network_rx_bytes_per_second`

These are byte counters from `/proc/net/dev` for exactly one interface, divided by the window length in seconds. Transmit is upload and receive is download, and both are in **bytes** per second.

Choosing the interface:

1. When `network_interface` is set in the dashboard, that interface is used.
2. Otherwise the agent picks the interface of the lowest-metric IPv4 default route in `/proc/net/route`. If there is no IPv4 default route, it uses the IPv6 default route in `/proc/net/ipv6_route`. Loopback and reject routes are ignored.
3. If two different interfaces share the best default route, the choice is ambiguous. Both network metrics are then reported as unavailable until an interface is configured.

The selection is rechecked every five minutes and whenever the configuration changes. A window is unavailable when the interface disappears, is replaced (its ifindex changes), or its counters go backwards.

## Discarded windows

A whole window is discarded, and a fresh baseline taken, in any of these cases:

- its length is outside 0.5×–1.5× the configured interval, or longer than one hour;
- the host was suspended during it (`CLOCK_BOOTTIME` advanced more than `CLOCK_MONOTONIC`);
- the wall clock jumped by more than five seconds relative to the monotonic clock;
- the interval, disk path, interface or paused state changed.
