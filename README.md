![image description](.github/logo.png)

[![C++](https://img.shields.io/badge/C++-23-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-orange)]()
[![License](https://img.shields.io/github/license/ksergey/turboq)](LICENSE)
[![CMake](https://img.shields.io/badge/build-CMake-informational.svg)](https://cmake.org)
[![CI](https://github.com/ksergey/turboq/actions/workflows/build-and-test.yml/badge.svg)](https://github.com/ksergey/turboq/actions/workflows/build-and-test.yml)

> TurboQ is a lightweight C++ library for building low-latency message queues, designed for high-performance applications where every microsecond matters.

## Features

- **Ultra-low latency** - nanosecond-range queue operations
- **Lock-free algorithms** - maximum throughput with no locks
- **Multiple queue types** - Multicast, SPSC, MPSC queues
- **Zero-copy operations** - minimal memory overhead

## Quick Start

### Dependencies

- C++23 compiler (CI covers GCC 14+ and Clang 20+)
- CMake 3.24+

The rest is fetched automatically at configure time via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) -- nothing else to install by hand, but the *first* `cmake` configure needs network access to pull these in:

- [doctest](https://github.com/doctest/doctest) -- builds and runs the unit tests (`code/turboq/*_test.cpp`); skipped entirely when `-Dturboq_BUILD_TESTS=OFF` (see below)
- [cxxopts](https://github.com/jarro2783/cxxopts) -- command-line parsing for `tools/latency_bench`

CPM itself is bootstrapped by `cmake/GetCPM.cmake`, which downloads a small `.cmake` script from
GitHub on first configure and caches it in the build directory (or under `$CPM_SOURCE_CACHE`, if
you've set that env var -- worth doing if you build from scratch often or want reproducible offline
builds). No system-wide install step is needed for any of the above.

### CMake options

| Option | Default | Effect |
|---|---|---|
| `turboq_BUILD_TESTS` | `ON` | Build `*_test.cpp` files as doctest executables and register them with `ctest`; when `turboq_PYTHON` is also `ON`, additionally registers the `python/tests` pytest suite as a `ctest` test. Set to `OFF` to skip fetching doctest and building/registering any tests entirely (e.g. for a faster, dependency-light consumer build). |
| `turboq_PYTHON` | `OFF` | Build the Python bindings under [`python/`](python/) (fetches pybind11 via CPM). |
| `turboq_TOOLS` | `ON` | Build tools under [`tools/`](tools/) (fetches cxxopts via CPM). |

```bash
cmake -B build -Dturboq_BUILD_TESTS=OFF
```

### Integration

Two equivalent ways to pull turboq into your own `CMakeLists.txt` -- pick whichever your project
already uses. Both give you a `turboq::turboq` target; neither requires installing turboq
system-wide first.

**FetchContent** (built into CMake, no extra file needed):

```cmake
include(FetchContent)
FetchContent_Declare(
    turboq
    GIT_REPOSITORY https://github.com/ksergey/turboq.git
    GIT_TAG master
)
FetchContent_MakeAvailable(turboq)

target_link_libraries(your_app PRIVATE turboq::turboq)
```

**[CPM.cmake](https://github.com/cpm-cmake/CPM.cmake)** (what turboq uses internally for its own
dependencies -- convenient if your project already has `cmake/CPM.cmake` vendored, since it adds
caching across projects via `CPM_SOURCE_CACHE` and a terser one-line syntax):

```cmake
include(cmake/GetCPM.cmake)  # or wherever your project bootstraps CPM from
CPMAddPackage("gh:ksergey/turboq#master")

target_link_libraries(your_app PRIVATE turboq::turboq)
```

Either way, pin a commit or tag instead of `master` for a reproducible build once turboq has
tagged releases; `master` here just tracks the latest commit.

> **Note:** turboq's own `turboq_BUILD_TESTS` option (default `ON`) applies transitively -- as a
> `FetchContent`/`CPM` dependency, building your project will also fetch doctest/cxxopts and build
> turboq's own test suite unless you turn it off, e.g. `set(turboq_BUILD_TESTS OFF)` before
> `FetchContent_MakeAvailable`/`CPMAddPackage`, or `-Dturboq_BUILD_TESTS=OFF` on the command line.

### Usage Examples

A minimal SPSC producer and consumer, two separate processes sharing a queue by name (`message-queue`
here) -- MPSC and Multicast follow the same `prepare()`/`commit()`/`fetch()`/`consume()` shape, see
[`tools/latency_bench.cpp`](tools/latency_bench.cpp) for a complete example of all three plus
proper error handling:

```cpp
// producer.cpp -- creates the queue if it doesn't exist yet
#include <cstring>
#include <iostream>
#include <string_view>

#include <turboq/SPSCMessageQueue.h>

int main() {
    using namespace turboq;

    auto result = SPSCMessageQueue::makeQueue("message-queue", SPSCMessageQueue::CreationOptions{.capacityHint = 1 << 20});
    if (!result) {
        std::cerr << "failed to create queue: " << result.error().message() << '\n';
        return 1;
    }

    auto queue = std::move(result).value();
    auto producer = queue.createProducer();

    std::string_view message = "hello from the producer";
    auto buffer = producer.prepare(message.size()); // reserve space, not yet visible to the consumer
    std::memcpy(buffer.data(), message.data(), message.size());
    producer.commit(); // make it visible

    std::cout << "sent: " << message << '\n';
}
```

```cpp
// consumer.cpp -- opens an existing queue only, throws if it isn't there yet
#include <iostream>

#include <turboq/SPSCMessageQueue.h>

int main() {
    using namespace turboq;

    auto result = SPSCMessageQueue::makeQueue("message-queue");
    if (!result) {
        std::cerr << "failed to open queue: " << result.error().message() << '\n';
        return 1;
    }

    auto queue = std::move(result).value();
    auto consumer = queue.createConsumer();

    for (;;) {
        auto buffer = consumer.fetch(); // never blocks; empty span means "nothing yet"
        if (!buffer.empty()) {
            std::cout << "received: "
                       << std::string_view{reinterpret_cast<char const*>(buffer.data()), buffer.size()} << '\n';
            consumer.consume();
            break;
        }
    }
}
```

A few things worth calling out:

- `fetch()`/`prepare()` never block -- a real program polls in a loop (ideally with some backoff or
  a rate limit if run continuously) rather than spinning as tightly as this toy example.
- Omitting `MemorySource` (as above) uses turboq's own default location (`/dev/shm`, falling back
  to `/tmp`); pass a `DefaultMemorySource{path}` explicitly to control where the backing file
  lives, or `AnonymousMemorySource{}` for a same-process-only queue (handy for a quick unit test).
- `SPSCMessageQueue::makeQueue(name)` (one argument, as in `consumer.cpp` above) opens an existing
  queue only and fails if it isn't there yet; passing `CreationOptions` (as in `producer.cpp`)
  creates the queue if missing or opens it if it already exists with a matching capacity.

### Python bindings

Optional pybind11-based bindings live under [`python/`](python/), covering all three queue types
(SPSC, MPSC, Multicast) with a bytes-in/bytes-out API: `producer.send(data: bytes) -> bool` and
`consumer.receive() -> bytes | None`. They're off by default; enable with `-Dturboq_PYTHON=ON`.
pybind11 is fetched automatically via CPM, same as `doctest`/`cxxopts` above -- no
`pip install pybind11` needed to configure the build.

#### Integration

The simplest option for a Python project: add a line to `requirements.txt` (or `pip install`
directly) pointing at this branch's `python/` subdirectory. `python/pyproject.toml` wraps the
CMake build with [scikit-build-core](https://scikit-build-core.readthedocs.io/), so pip drives the
whole thing -- configure, build, and install the compiled extension -- same as any other package:

```
# requirements.txt
git+https://github.com/ksergey/turboq.git@python#subdirectory=python
```

```bash
CC=gcc-14 CXX=g++-14 pip install -r requirements.txt
```

`CC`/`CXX` still need to point at a C++23-capable compiler -- pip has no way to know that on its
own, same as a manual CMake configure. Without them, pip falls back to whatever your system's
default `cc`/`c++` is, which may well be older than GCC 14/Clang 20. If you'd rather not touch
environment variables, pass the compiler directly to CMake instead:

```bash
pip install -r requirements.txt \
    --config-settings=cmake.define.CMAKE_C_COMPILER=gcc-14 \
    --config-settings=cmake.define.CMAKE_CXX_COMPILER=g++-14
```

Pin a commit instead of a branch (`@<sha>` instead of `@python`) for a reproducible build.

Alternatively, build in-tree and point `PYTHONPATH` at the result -- good for trying things out
without going through pip at all:

```bash
cmake -B build -Dturboq_PYTHON=ON -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build --target _turboq
PYTHONPATH=build/python python3 -c "import turboq; print(turboq.SPSCQueue)"
```

Or install it into a Python environment directly via CMake (equivalent to what the pip route above
does under the hood, without pip in the loop):

```bash
cmake -B build -Dturboq_PYTHON=ON -Dturboq_BUILD_TESTS=OFF \
    -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
    -DCMAKE_INSTALL_PREFIX="$(python3 -c 'import site; print(site.getsitepackages()[0])')"
cmake --build build --target _turboq
cmake --install build --component turboq_python  # or just `cmake --install build`
python3 -c "import turboq"
```

#### Usage example

Sending and receiving a message, single process (SPSC, the simplest case -- MPSC and Multicast
follow the same shape, see the docstrings/tests under [`python/`](python/) for their extras:
multiple producers for MPSC, `overrun_count` for Multicast):

```python
import turboq

queue = turboq.SPSCQueue("message-queue", capacity_hint=1 << 20, anonymous=True)
producer = queue.create_producer()
consumer = queue.create_consumer()

if producer.send(b"hello"):
    print("sent")

message = consumer.receive()  # bytes, or None if the queue is currently empty
if message is not None:
    print("received:", message)
```

Real usage is normally two separate processes sharing a queue by name rather than `anonymous=True`
in one process -- drop `anonymous` and give both sides the same `name` (and `path`, if you don't
want turboq's own `/dev/shm`/`/tmp` default):

```python
# producer.py -- creates the queue if it doesn't exist yet
import turboq

queue = turboq.SPSCQueue("message-queue", capacity_hint=1 << 20, path="/dev/shm/myapp")
producer = queue.create_producer()
producer.send(b"hello from another process")
```

```python
# consumer.py -- opens an existing queue only, raises if it isn't there yet
import turboq

queue = turboq.SPSCQueue.open("message-queue", path="/dev/shm/myapp")
consumer = queue.create_consumer()
print(consumer.receive())
```

A couple of things worth knowing:

- `receive()` never blocks -- an empty queue and a zero-length message both come back as an empty
  read at the C++ level, so `receive()` returns `None` for both; if you need to send a genuinely
  empty event, prefix it with a sentinel byte rather than relying on payload length alone.
- `SPSCQueue`/`MulticastQueue` enforce a single producer (an OS file lock rejects a second one);
  `MPSCQueue` allows any number of producers. All three raise `RuntimeError` on misuse (bad
  options, opening a queue that doesn't exist, a message too large for an MPSC slot, etc).

See [`python/tests`](python/tests) for runnable examples of all three queue types, including MPSC's
multiple producers and Multicast's broadcast-to-many-consumers/`overrun_count`.

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

