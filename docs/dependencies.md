# Dependencies

This project keeps dependencies intentionally small.

## 1) Required build dependencies

- C++17 compiler (`g++` or `clang++`)
- CMake `>= 3.16`
- Standard Linux toolchain/runtime (`build-essential`-equivalent)

## 2) Required runtime dependencies

- Linux OS (raw-socket transport implementation is Linux-specific)
- Root privileges or `CAP_NET_RAW` for physical EtherCAT raw socket access

## 3) Optional dependencies

- `doxygen` and `graphviz` for `docs` target
- Debian packaging tools (`cpack`/`dpkg` ecosystem) for `.deb` artifacts
- `ninja-build` if you prefer Ninja generator

## Ubuntu/Debian install

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  git
```

Optional:

```bash
sudo apt-get install -y \
  ninja-build \
  gdb \
  doxygen \
  graphviz \
  ccache \
  pkg-config
```

## Raspberry Pi OS install

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  git
```

Optional:

```bash
sudo apt-get install -y \
  ninja-build \
  gdb \
  doxygen \
  graphviz
```

## Raw socket capability setup (optional alternative to full root)

If you prefer not to run examples as `sudo`, grant raw socket capability:

```bash
sudo setcap cap_net_raw+ep ./build/beckhoff_io_demo
sudo setcap cap_net_raw+ep ./build/linux_raw_socket_cycle_demo
sudo setcap cap_net_raw+ep ./build/physical_topology_scan_demo
sudo setcap cap_net_raw+ep ./build/dc_hardware_sync_demo
sudo setcap cap_net_raw+ep ./build/dc_soak_demo
```

You must reapply `setcap` after rebuilding binaries.

## Build quick start

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```
