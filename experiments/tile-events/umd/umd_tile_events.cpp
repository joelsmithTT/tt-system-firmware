// tiledrive's PMM path through tt-umd: loads brisc.bin with UMD's Cluster and reads the records back
// with UMD's DeviceEventReader, as a tt-metal-like host would.
//
//   umd_tile_events send <brisc.bin> [N] [X,Y]
//       Has Tensix (X,Y) (default 1,2) publish N records (default 4) through the PMM mailbox, and checks
//       that each arrives intact through DeviceEventReader and that ARC acked it by clearing its magic.
//
//   umd_tile_events watch <seconds> [max_events]
//       Prints every record DeviceEventReader returns, like `tt-events watch`, until the time runs out
//       or max_events arrive.

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "layout.h"
#include "tt-umd-workload/cluster.hpp"
#include "tt-umd/pcie/device_event_reader.hpp"
#include "tt-umd/types/core_coordinates.hpp"
#include "tt-umd/types/risc_type.hpp"

using namespace tt;
using namespace tt::umd;

namespace {

// What brisc.cpp puts in each record.
constexpr uint16_t TYPE_TEST = 0x0000;
constexpr uint32_t BRISC_CAUSE = 0xC0FFEE;
constexpr uint32_t MARKER = 0x55AA0000;

constexpr int PCI_DEVICE = 0;
constexpr ChipId CHIP = 0;

// Tensix tiles present on every Blackhole SKU. A NOC access to a fused-off tile can hang the host.
bool tile_on_every_sku(uint32_t x, uint32_t y) {
    return ((x >= 1 && x <= 7) || (x >= 10 && x <= 14)) && y >= 2 && y <= 11;
}

void print_event(const DeviceEvent& e) {
    std::printf(
        "seq %" PRIu64 " type 0x%04x source 0x%04x (%u,%u) cause 0x%08x data 0x%08x 0x%08x 0x%08x 0x%08x "
        "device_time %" PRIu64 " host_time_ns %" PRIu64 "\n",
        e.seq,
        e.type,
        e.source,
        e.source & 0x3Fu,
        e.source >> 6,
        e.cause,
        e.data[0],
        e.data[1],
        e.data[2],
        e.data[3],
        e.device_time,
        e.host_time_ns);
}

int watch(int seconds, size_t max_events) {
    DeviceEventReader reader(PCI_DEVICE);
    std::printf("firmware attached: %s\n", reader.is_firmware_attached() ? "yes" : "no");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    size_t seen = 0;
    while (seen < max_events && std::chrono::steady_clock::now() < deadline) {
        DeviceEventBatch batch = reader.read(64, std::chrono::milliseconds(500));
        if (batch.device_reset) {
            std::printf("device reset\n");
        }
        if (batch.lost) {
            std::printf("lost %u\n", batch.lost);
        }
        for (const DeviceEvent& e : batch.events) {
            print_event(e);
            seen++;
        }
        std::fflush(stdout);
    }
    std::printf("%zu events\n", seen);
    return 0;
}

int send(const std::string& bin_path, uint32_t n, uint32_t x, uint32_t y) {
    std::ifstream bin_file(bin_path, std::ios::binary);
    if (!bin_file) {
        std::fprintf(stderr, "cannot open %s\n", bin_path.c_str());
        return 2;
    }
    std::vector<uint8_t> program((std::istreambuf_iterator<char>(bin_file)), std::istreambuf_iterator<char>());

    // Constructed first: a reader sees only records that arrive after it exists.
    DeviceEventReader reader(PCI_DEVICE);
    if (!reader.is_firmware_attached()) {
        std::fprintf(stderr, "firmware does not publish device events\n");
        return 1;
    }

    Cluster cluster;
    const CoreCoord tile(x, y, CoreType::TENSIX, CoordSystem::TRANSLATED);
    auto write32 = [&](uint32_t addr, uint32_t value) { cluster.write_to_device(&value, 4, CHIP, tile, addr); };
    auto read32 = [&](uint32_t addr) {
        uint32_t value = 0;
        cluster.read_from_device(&value, CHIP, tile, addr, 4);
        return value;
    };

    // BRISC starts at L1 address 0 when released.
    cluster.assert_risc_reset(CHIP, tile, RiscType::ALL_TENSIX);
    cluster.write_to_device(program.data(), program.size(), CHIP, tile, 0);
    for (uint32_t i = 0; i < n; i++) {
        write32(RECORDS + RECORD_STRIDE * i, 0);
    }
    write32(PARAM_N, n);
    write32(PARAM_MARKER, MARKER);
    write32(PARAM_SYNC, 0);
    write32(PARAM_PMM, 1);
    write32(DONE, 0);
    cluster.l1_membar(CHIP, {tile});
    cluster.deassert_risc_reset(CHIP, tile, RiscType::BRISC, false);

    std::vector<DeviceEvent> events;
    uint32_t lost = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (events.size() < n && std::chrono::steady_clock::now() < deadline) {
        DeviceEventBatch batch = reader.read(n - events.size(), std::chrono::milliseconds(500));
        if (batch.device_reset) {
            std::fprintf(stderr, "device reset during the run\n");
            return 1;
        }
        lost += batch.lost;
        events.insert(events.end(), batch.events.begin(), batch.events.end());
    }
    const uint32_t done = read32(DONE);
    cluster.assert_risc_reset(CHIP, tile, RiscType::ALL_TENSIX);

    const uint16_t source = static_cast<uint16_t>(x | y << 6);
    int bad = 0;
    for (uint32_t i = 0; i < events.size(); i++) {
        const DeviceEvent& e = events[i];
        print_event(e);
        // brisc.cpp sends data = {marker, index, its own x | y << 6}.
        const bool ok = e.type == TYPE_TEST && e.source == source && e.cause == BRISC_CAUSE && e.data[0] == MARKER &&
                        e.data[1] == i && e.data[2] == source;
        if (!ok) {
            std::printf("  ^ record %u does not match\n", i);
            bad++;
        }
    }
    // ARC clears a record's magic once it has forwarded it.
    uint32_t unacked = 0;
    for (uint32_t i = 0; i < n; i++) {
        unacked += read32(RECORDS + RECORD_STRIDE * i) == RECORD_MAGIC;
    }

    std::printf(
        "tile (%u,%u): BRISC done 0x%x%s, received %zu/%u, mismatched %d, lost %u, magic still set %u\n",
        x,
        y,
        done,
        (done & DONE_STUCK) ? " (stuck waiting for an ack)" : "",
        events.size(),
        n,
        bad,
        lost,
        unacked);
    const bool pass = done == n && events.size() == n && bad == 0 && lost == 0 && unacked == 0;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "watch" && argc >= 3) {
        return watch(std::atoi(argv[2]), argc > 3 ? std::strtoul(argv[3], nullptr, 0) : SIZE_MAX);
    }
    if (mode == "send" && argc >= 3) {
        const uint32_t n = argc > 3 ? std::strtoul(argv[3], nullptr, 0) : 4;
        uint32_t x = 1, y = 2;
        if (argc > 4 && std::sscanf(argv[4], "%u,%u", &x, &y) != 2) {
            std::fprintf(stderr, "bad tile %s\n", argv[4]);
            return 2;
        }
        if (!tile_on_every_sku(x, y)) {
            std::fprintf(stderr, "refusing tile (%u,%u): not present on every Blackhole SKU\n", x, y);
            return 2;
        }
        if (n == 0 || n > MAX_TOKENS) {
            std::fprintf(stderr, "N must be 1..%u\n", MAX_TOKENS);
            return 2;
        }
        return send(argv[2], n, x, y);
    }
    std::fprintf(stderr, "usage: %s send <brisc.bin> [N] [X,Y] | watch <seconds> [max_events]\n", argv[0]);
    return 2;
}
