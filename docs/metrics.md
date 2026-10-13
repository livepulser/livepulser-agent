# Metric definitions

All metrics describe the host the agent runs on. A report covers one window, from the previous measurement to the current one:

- CPU and network are rates over the window.
- Memory, swap and disk are snapshots taken at the end of the window.

Windows are timed with a monotonic clock that stops while the host sleeps. `observed_at` is the UTC wall-clock time at the end of the window.

## CPU — `cpu_percent`

```
cpu = 100 × (Δtotal − Δidle) / Δtotal
```

This is utilization across all CPUs, from 0 to 100. It is not the load average.

| Platform | Counters |
| --- | --- |
| Linux | Aggregate `cpu` line of `/proc/stat`. `total` = user + nice + system + idle + iowait + irq + softirq + steal, and `idle` = idle + iowait. `guest` and `guest_nice` are already counted in `user` and `nice`, so they are not added again. |
| macOS | Sum of per-CPU user, nice, system and idle ticks from `host_processor_info(PROCESSOR_CPU_LOAD_INFO)`. |
| Windows | `GetSystemTimes()`: `total` = kernel + user (kernel time already includes idle), and `idle` = idle. |

A window is reported as unavailable in any of these cases:

- the number of CPUs changes;
- any counter decreases (on macOS this also covers 32-bit tick wrap-around);
- the aggregate total or idle decreases.

On Linux, a decrease in `iowait` alone is tolerated, because the kernel documents that it can go backwards.

## Memory — `memory_percent`

| Platform | Definition |
| --- | --- |
| Linux | `100 × (MemTotal − MemAvailable) / MemTotal` from `/proc/meminfo`. `MemFree` is never used as a substitute, and the value is unavailable without `MemAvailable` (kernels before 3.14). |
| macOS | `100 × (app + wired + compressed) / hw.memsize`, where app = internal − purgeable pages. This matches "Memory Used" in Activity Monitor. |
| Windows | `100 × (TotalPhys − AvailPhys) / TotalPhys` from `GlobalMemoryStatusEx()`, matching "In use" in Task Manager. |

## Swap — `swap_percent`

| Platform | Definition |
| --- | --- |
| Linux | `100 × (SwapTotal − SwapFree) / SwapTotal`. Hosts without swap report it as unavailable, not 0%. |
| macOS | `100 × used / total` from `vm.swapusage`. macOS creates swap files on demand, so an empty swap area reports 0%. |
| Windows | The `\Paging File(_Total)\% Usage` performance counter. The value is unavailable when no paging file is configured. |

## Disk — `disk_percent`

The value is used capacity of one filesystem, chosen by `disk_path` in the dashboard (default `/`).

| Platform | Definition |
| --- | --- |
| Linux | `statvfs()`: `100 × (f_blocks − f_bfree) / f_blocks`. This counts allocated blocks, including blocks reserved for root, so it can read slightly differently from the `Use%` column of `df`. |
| macOS | `statfs()` with the same formula. On APFS this describes the whole container, because all volumes share its free space. |
| Windows | `GetDiskFreeSpaceExW()`: `100 × (total − total free) / total`. `/` means the system drive; `D:\` or `D:/data` selects another volume. |

The value is unavailable when the path is invalid, inaccessible, or reports zero capacity.

## Network — `network_tx_bytes_per_second`, `network_rx_bytes_per_second`

These are byte counters for exactly one interface, divided by the window length in seconds. Transmit is upload and receive is download, and both are in **bytes** per second.

| Platform | Counters | Interface names |
| --- | --- | --- |
| Linux | `/proc/net/dev` | kernel names such as `eth0` |
| macOS | `if_data64` from `sysctl(NET_RT_IFLIST2)` | BSD names such as `en0` |
| Windows | `GetIfEntry2()` `InOctets` / `OutOctets` | interface aliases such as `Ethernet` |

Choosing the interface:

1. When `network_interface` is set in the dashboard, that interface is used.
2. Otherwise the agent picks the interface of the primary IPv4 default route, falling back to IPv6 when there is no IPv4 default route. Loopback interfaces and reject routes are ignored. How "primary" is decided depends on the platform:
   - **Linux:** the lowest-metric route in the main routing table.
   - **macOS:** the unscoped default route, since scoped defaults exist for every additional network service.
   - **Windows:** the lowest sum of route metric and interface metric, which is how Windows chooses.
3. If two different interfaces are equally preferred, the choice is ambiguous. Both network metrics are then reported as unavailable until an interface is configured.

The selection is rechecked every five minutes and whenever the configuration changes. A window is unavailable when the interface disappears or is replaced, or when its counters go backwards.

## Discarded windows

A whole window is discarded, and a fresh baseline taken, in any of these cases:

- its length is outside 0.5×–1.5× the configured interval, or longer than one hour;
- the host slept during it;
- the wall clock jumped by more than five seconds relative to the monotonic clock;
- the interval, disk path, interface or paused state changed.
