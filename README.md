# turboq

[![C++](https://img.shields.io/badge/C++-23-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/github/license/ksergey/turboq)](LICENSE)
[![CMake](https://img.shields.io/badge/build-CMake-informational.svg)](https://cmake.org)

**High-performance, low-latency message queue library in C++**

TurboQ is a lightweight C++ library for building low-latency message queues, designed for high-performance applications where every microsecond matters.

## Features

- **Ultra-low latency** - nanosecond-range queue operations
- **Lock-free algorithms** - maximum throughput with no locks
- **Multiple queue types** - SPSC, MPSC, MPMC queues
- **Zero-copy operations** - minimal memory overhead

## Quick Start

### Dependencies

- C++23 compiler
- CMake 3.24+

### Integration

Add to `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    turboq
    GIT_REPOSITORY https://github.com/ksergey/turboq.git
    GIT_TAG main
)
FetchContent_MakeAvailable(turboq)

target_link_libraries(your_app PRIVATE turboq::turboq)

```

### Usage Examples

TODO

## Benchmarking with `latency_bench`

`tools/latency_bench.cpp` is a single binary that acts as either a producer or a consumer for any
of the three queue types, one role per run -- launch it twice (as two separate processes) to
measure real inter-process latency.

```bash
# spsc
./latency_bench --role producer --type spsc --name q --count 10000000 &
./latency_bench --role consumer --type spsc --name q --count 10000000

# mpsc -- run --role producer more than once against the same --name to add producers;
# give each instance a distinct --producer-id so the consumer can tell them apart
./latency_bench --role producer --type mpsc --name q --producer-id 0 --count 5000000 &
./latency_bench --role producer --type mpsc --name q --producer-id 1 --count 5000000 &
./latency_bench --role consumer --type mpsc --name q --producers 2 --count 5000000

# multicast -- the consumer can be started (and restarted) independently any number of
# times; it only sees messages published after it attaches
./latency_bench --role producer --type multicast --name q --count 10000000 &
./latency_bench --role consumer --type multicast --name q --count 10000000
```

The producer must be started first (it creates the backing queue); run `--help` for the full list
of flags (message size, rate limiting, warmup, idle timeout, etc).

Numbers from an unpinned run on a shared/virtualized/oversubscribed machine are close to
meaningless for a library built around single-digit-microsecond latency -- OS scheduling jitter
alone will dominate everything the library itself does. The next section covers what's needed to
get numbers that actually reflect the queue rather than the scheduler.

## Performance tuning (Linux)

Two independent things affect latency the most: whether the producer/consumer threads can be
preempted by the rest of the system, and whether the memory backing the queue is paged in ahead of
time on huge (2 MiB/1 GiB) rather than regular (4 KiB) pages, which cuts TLB misses on the hot
path. Both need one-time system setup; examples below are for Arch Linux, but the mechanism (a set
of kernel boot parameters plus systemd mount units) is the same on any systemd-based distribution.

### CPU isolation

`isolcpus` removes CPUs from the kernel's general SMP scheduler, so ordinary processes/threads are
never placed on them -- only threads explicitly pinned there (e.g. via `taskset`) run on them.
On its own it doesn't stop the periodic timer tick or IRQ handling on those CPUs, which is usually
paired with:

- `nohz_full=<cpus>` -- stop the periodic scheduler tick on those CPUs when only one runnable task
  is present, removing a recurring source of jitter.
- `rcu_nocbs=<cpus>` -- move RCU callback processing off those CPUs onto the housekeeping ones.
- `irqaffinity=<housekeeping cpus>` -- keep the default IRQ affinity off the isolated CPUs (you can
  also set individual IRQs' affinity by hand via `/proc/irq/<n>/smp_affinity_list`).

Example: isolating CPUs 2 and 3, keeping 0-1 for everything else (adjust to your core count and
topology -- check `lscpu` first, and prefer isolating CPUs on the same NUMA node the queue's memory
and NIC/disk I/O live on if that matters for your workload):

```bash
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1
```

On Arch with **GRUB**, add these to `/etc/default/grub`:

```bash
# /etc/default/grub
GRUB_CMDLINE_LINUX_DEFAULT="... isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1"
```

then regenerate the config and reboot:

```bash
sudo grub-mkconfig -o /boot/grub/grub.cfg
sudo reboot
```

On Arch with **systemd-boot**, add the same parameters to the `options` line of the relevant entry
in `/boot/loader/entries/*.conf` (or to `/etc/kernel/cmdline` if you're using
`mkinitcpio`'s unified kernel image generation), then reboot -- no separate regen step needed for
`systemd-boot` itself, though a unified-image setup will need `mkinitcpio -P` first.

Verify after reboot:

```bash
cat /proc/cmdline                        # confirm the parameters actually took
cat /sys/devices/system/cpu/isolated     # should list 2,3
```

### Huge pages

1 GiB pages must be reserved at boot: they need physically contiguous memory, which is rarely
available once the system has been running for a while and memory gets fragmented. 2 MiB pages are
more forgiving and can usually be reserved later at runtime too. Reserve both up front on the
kernel command line, alongside the `isolcpus` parameters above:

```bash
default_hugepagesz=2M hugepagesz=1G hugepages=4 hugepagesz=2M hugepages=512
```

This reserves 4 GiB as 1 GiB pages and 1 GiB as 2 MiB pages. Add it the same way as the `isolcpus`
parameters above (GRUB's `GRUB_CMDLINE_LINUX_DEFAULT`, or systemd-boot's entry `options`/
`/etc/kernel/cmdline`), then reboot.

To reserve (or top up) 2 MiB pages at runtime instead, without rebooting:

```bash
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

Arch's systemd mounts the default-size hugetlbfs at `/dev/hugepages` automatically
(`dev-hugepages.mount`, part of systemd itself). If `default_hugepagesz` above is `2M`, that mount
already serves 2 MiB pages. For 1 GiB pages (or to have both sizes mounted at once) add an explicit
mount, e.g. via `/etc/fstab`:

```
# /etc/fstab
hugetlbfs  /dev/hugepages1G  hugetlbfs  pagesize=1G,mode=1770,gid=users  0  0
```

```bash
sudo mkdir -p /dev/hugepages1G
sudo systemctl daemon-reload
sudo mount /dev/hugepages1G
```

(`gid=users` above just needs to be a group your user is in, so `latency_bench` can create files
there without running as root -- adjust to whatever group setup you actually use.)

Verify:

```bash
grep -i huge /proc/meminfo    # HugePages_Total / HugePages_Free / Hugepagesize
mount | grep huge             # confirm both mounts are present, if using both sizes
```

### Running `latency_bench` pinned, with huge pages

`--hugepages auto|none|2m|1g` selects where the queue's backing memory is allocated (see
[`DefaultMemorySource`](code/turboq/MemorySource.h) -- `auto` prefers 1 GiB, falls back to 2 MiB,
then regular pages). **Producer and consumer must be given the same `--hugepages` value** -- they
independently resolve it to a mount point/page size, and if those don't match the consumer simply
won't find the file the producer created.

```bash
# spsc, pinned to isolated cores 2 (producer) and 3 (consumer), backed by 1 GiB pages
taskset -c 2 ./latency_bench --role producer --type spsc --name q --count 10000000 --hugepages 1g &
taskset -c 3 ./latency_bench --role consumer --type spsc --name q --count 10000000 --hugepages 1g

# mpsc, three producers each pinned to their own core, consumer on a fourth
taskset -c 2 ./latency_bench --role producer --type mpsc --name q --producer-id 0 --count 3000000 --hugepages 2m &
taskset -c 3 ./latency_bench --role producer --type mpsc --name q --producer-id 1 --count 3000000 --hugepages 2m &
taskset -c 4 ./latency_bench --role producer --type mpsc --name q --producer-id 2 --count 3000000 --hugepages 2m &
taskset -c 5 ./latency_bench --role consumer --type mpsc --name q --producers 3 --count 3000000 --hugepages 2m

# multicast, two independent consumers on their own cores
taskset -c 2 ./latency_bench --role producer --type multicast --name q --count 10000000 --hugepages 1g &
taskset -c 3 ./latency_bench --role consumer --type multicast --name q --count 10000000 --hugepages 1g &
taskset -c 4 ./latency_bench --role consumer --type multicast --name q --count 10000000 --hugepages 1g
```

If `--hugepages` fails with "No such file or directory", the reservation/mount steps above weren't
completed (or don't cover the requested size) -- `latency_bench` reports which `--hugepages` value
it couldn't satisfy.

### License

Distributed under the MIT License. See LICENSE for details.

