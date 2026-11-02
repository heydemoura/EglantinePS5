// SPDX-License-Identifier: GPL-3.0-or-later
// Compile with -DREFERENCE_MAPPING_HEADER='"/absolute/reference.h"'.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#define Nx2GpuMappingTable ReferenceMappingTable
#include REFERENCE_MAPPING_HEADER
#undef Nx2GpuMappingTable
#include "core/hle/service/nvdrv/devices/nx2_gpu_mapping_table.h"

using Table = Service::Nvidia::Devices::Nx2GpuMappingTable;
using Reference = Service::Nvidia::Devices::ReferenceMappingTable;

void Require(bool value) {
    if (!value) {
        std::cerr << "mapping differential mismatch\n";
        std::exit(1);
    }
}

template <typename A, typename B>
void SameRanges(const A& a, const B& b) {
    Require(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        Require(a[i].gpu == b[i].gpu && a[i].cpu == b[i].cpu && a[i].size == b[i].size);
    }
}

template <typename A, typename B>
void SameUpdate(const A& a, const B& b) {
    Require(a.success == b.success && a.lease == b.lease);
    Require(a.changes.size() == b.changes.size());
    for (size_t i = 0; i < a.changes.size(); ++i) {
        const auto& x = a.changes[i];
        const auto& y = b.changes[i];
        Require(x.map == y.map && x.native == y.native && x.range.gpu == y.range.gpu &&
                x.range.cpu == y.range.cpu && x.range.size == y.range.size);
    }
}

template <typename T>
double Bench() {
    T table;
    constexpr unsigned ranges = 1024;
    constexpr unsigned operations = 4000;
    for (unsigned i = 0; i < ranges; ++i) {
        table.Map(0x10000ULL + i * 0x2000ULL, 0x10000000ULL + i * 0x2000ULL, 0x1000);
    }
    const auto start = std::chrono::steady_clock::now();
    size_t changes{};
    for (unsigned i = 0; i < operations; ++i) {
        const u64 address = 0x10000ULL + (i % ranges) * 0x2000ULL;
        changes += table.Map(address, 0x20000000ULL + (i % ranges) * 0x2000ULL, 0x1000)
                       .changes.size();
        changes += table.Map(address, 0x10000000ULL + (i % ranges) * 0x2000ULL, 0x1000)
                       .changes.size();
    }
    Require(changes == operations * 2);
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
               .count() / (operations * 2);
}

int main() {
    std::mt19937_64 random{0x53454c45564554ULL};
    Table candidate;
    Reference reference;
    std::vector<u64> leases;
    constexpr unsigned operations = 200000;
    for (unsigned i = 0; i < operations; ++i) {
        const u64 address = (random() % 256) * 0x1000;
        const u64 size = (random() % 9) * 0x1000;
        const u64 cpu = random() % 2 ? address : 0x100000 + (random() % 256) * 0x1000;
        switch (random() % 5) {
        case 0:
        case 1:
            SameUpdate(candidate.Map(address, cpu, size), reference.Map(address, cpu, size));
            break;
        case 2:
            SameUpdate(candidate.Unmap(address, size), reference.Unmap(address, size));
            break;
        case 3:
            if (leases.size() < 32) {
                auto a = candidate.Acquire(address, size);
                auto b = reference.Acquire(address, size);
                SameUpdate(a, b);
                if (a.success) leases.push_back(a.lease);
            }
            break;
        case 4:
            if (!leases.empty()) {
                const size_t index = random() % leases.size();
                SameUpdate(candidate.Release(leases[index]), reference.Release(leases[index]));
                leases.erase(leases.begin() + index);
            } else {
                SameUpdate(candidate.Release(~u64{}), reference.Release(~u64{}));
            }
            break;
        }
        SameRanges(candidate.Effective(), reference.Effective());
        Require(candidate.UsesBacking(cpu, size) == reference.UsesBacking(cpu, size));
    }
    for (const u64 address : {~u64{}, ~u64{} - 0x1000, u64{0}}) {
        for (const u64 size : {u64{0}, u64{1}, u64{0x1000}, ~u64{}}) {
            SameUpdate(candidate.Map(address, address, size), reference.Map(address, address, size));
            SameUpdate(candidate.Unmap(address, size), reference.Unmap(address, size));
            SameUpdate(candidate.Acquire(address, size), reference.Acquire(address, size));
        }
    }
    const double old_us = Bench<Reference>();
    const double new_us = Bench<Table>();
    std::cout << "{\"random_operations\":" << operations
              << ",\"reference_us_per_update\":" << old_us
              << ",\"selevet_us_per_update\":" << new_us
              << ",\"ratio\":" << old_us / new_us << "}\n";
}
