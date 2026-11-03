// Temporary MKW diagnostic; remove before retaining the fix.
#include <signal.h>
#include <pthread_np.h>
#include <unistd.h>
namespace MkwNativeSample {
struct Block { std::atomic<unsigned long long> entry{}, size{}, location{}; };
struct Sample { std::atomic<unsigned long long> pc{}, highwater{}, rsp{}, rbp{}, marker{}; std::array<std::atomic<unsigned long long>, 32> stack{}; std::array<std::atomic<unsigned long long>, 12> frames{}; };
static_assert(sizeof(Sample) == 392);
inline std::array<Block, 1 << 20> blocks{};
inline std::atomic<unsigned long long> block_count{}, block_overflow{};
inline std::array<Sample, 16384> samples{};
inline std::atomic<unsigned long long> sample_count{}, state{};
inline uintptr_t stack_low{}, stack_high{};
struct Worker { std::atomic<pthread_t> thread{}; uintptr_t low{}, high{}; };
inline std::array<Worker, 7> workers{};
inline std::atomic<unsigned long long> sample_target{6};
struct FiberStack { std::atomic<uintptr_t> low{}, high{}; };
inline std::array<FiberStack, 2048> fiber_stacks{};
inline std::mutex fiber_stacks_mutex;
inline std::atomic<unsigned long long> fiber_stack_overflow{};
void RecordFiberStack(void* base, size_t bytes) {
    const auto low = reinterpret_cast<uintptr_t>(base);
    std::lock_guard lock{fiber_stacks_mutex};
    if (!bytes) {
        for (auto& stack : fiber_stacks) {
            if (stack.low.load() == low) {
                stack.low.store(0, std::memory_order_release);
                return;
            }
        }
        return;
    }
    for (auto& stack : fiber_stacks) {
        if (!stack.low.load()) {
            stack.high.store(low + bytes, std::memory_order_relaxed);
            stack.low.store(low, std::memory_order_release);
            return;
        }
    }
    fiber_stack_overflow.fetch_add(1, std::memory_order_relaxed);
}

static_assert(std::atomic<unsigned long long>::is_always_lock_free);
void Signal(int, siginfo_t*, void* context) {
    const auto i = sample_count.load(std::memory_order_relaxed);
    if (i >= samples.size()) return;
    // Read only; validate sampled addresses against this run's executable maps.
    samples[i].highwater.store(block_count.load(std::memory_order_relaxed));
    const auto* words = static_cast<const uintptr_t*>(context);
    const auto pc = words[224 / sizeof(uintptr_t)];
    samples[i].pc.store(pc);
    const auto rsp = words[248 / sizeof(uintptr_t)];
    samples[i].rsp.store(rsp); samples[i].rbp.store(words[136 / sizeof(uintptr_t)]);
    char marker;
    const auto here = reinterpret_cast<uintptr_t>(&marker);
    samples[i].marker.store(here);
    // Same interrupted stack, bounded to one page; reject alternate/unknown stacks.
    if (rsp > here && rsp - here < 65536 && (rsp & 7) == 0 &&
        (rsp & 4095) <= 4096 - 32 * sizeof(uintptr_t)) {
        const auto* stack = reinterpret_cast<const uintptr_t*>(rsp);
        for (unsigned j = 0; j < 32; ++j) samples[i].stack[j].store(stack[j]);
    }
    for (auto& frame : samples[i].frames) frame.store(0, std::memory_order_relaxed);
    // Guest kernel contexts run on Common::Fiber allocations, outside pthread's stack.
    // Use their registered live bounds rather than treating raw stack words as return addresses.
    auto low = stack_low, high = stack_high;
    if (rsp < low || rsp >= high) {
        low = high = 0;
        for (const auto& stack : fiber_stacks) {
            const auto begin = stack.low.load(std::memory_order_acquire);
            const auto end = stack.high.load(std::memory_order_relaxed);
            if (begin && stack.low.load(std::memory_order_acquire) == begin &&
                rsp >= begin && rsp < end) { low = begin; high = end; break; }
        }
    }
    auto frame = words[136 / sizeof(uintptr_t)];
    for (unsigned depth = 0; depth < samples[i].frames.size(); ++depth) {
        if ((frame & 7) || frame < rsp || frame < low || high < 16 ||
            frame > high - 16) break;
        const auto* record = reinterpret_cast<const uintptr_t*>(frame);
        const auto next = record[0];
        samples[i].frames[depth].store(record[1], std::memory_order_relaxed);
        if (next <= frame || next - frame > 128 * 1024) break;
        frame = next;
    }
    sample_count.store(i + 1, std::memory_order_release);
}
void Register(const char* name) {
    unsigned index = 7;
    if (!std::strcmp(name, "GPU")) index = 6;
    else if (!std::strncmp(name, "CPUCore_", 8) && name[8] >= '0' && name[8] <= '5' && !name[9])
        index = unsigned(name[8] - '0');
    if (index >= workers.size()) return;
    auto& worker = workers[index];
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) == 0) {
        if (pthread_attr_get_np(pthread_self(), &attributes) == 0) {
            void* base{};
            size_t bytes{};
            if (pthread_attr_getstack(&attributes, &base, &bytes) == 0) {
                worker.low = reinterpret_cast<uintptr_t>(base);
                worker.high = worker.low + bytes;
            }
        }
        pthread_attr_destroy(&attributes);
    }
    worker.thread.store(pthread_self(), std::memory_order_release);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
    if (pthread_sigmask(SIG_UNBLOCK, &mask, nullptr)) return;
    unsigned long long expected = 0;
    if (!state.compare_exchange_strong(expected, 1)) return;
    struct sigaction old{}, action{};
    if (sigaction(SIGUSR2, nullptr, &old) || old.sa_handler != SIG_DFL) { state.store(90); return; }
    action.sa_sigaction = Signal; action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, nullptr)) { state.store(91); return; }
    std::thread([] {
        struct Arm { unsigned long long sequence{}; unsigned target{6}, count{12000}; };
        const auto read_arm = [] {
            Arm arm;
            if (auto* file = std::fopen("/data/eglantineps5/logs/mkw-profile-arm.txt", "r")) {
                (void)std::fscanf(file, "%llu %u %u", &arm.sequence, &arm.target, &arm.count);
                std::fclose(file);
            }
            return arm;
        };
        auto previous = read_arm().sequence;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const auto arm = read_arm();
            if (!arm.sequence || arm.sequence == previous) continue;
            previous = arm.sequence;
            if (arm.target >= workers.size()) continue;
            const auto target = workers[arm.target].thread.load(std::memory_order_acquire);
            if (!target) continue;
            stack_low = workers[arm.target].low;
            stack_high = workers[arm.target].high;
            sample_target.store(arm.target, std::memory_order_release);
            sample_count.store(0, std::memory_order_release);
            state.store(2, std::memory_order_release);
            const unsigned limit = std::clamp(arm.count, 1000U, 12000U);
            for (unsigned i = 0; i < limit; ++i) {
                if (pthread_kill(target, SIGUSR2)) { state.store(93); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const auto count = sample_count.load(std::memory_order_acquire);
            const unsigned long long header[]{0x4d4b5753414d5031ULL,
                static_cast<unsigned long long>(getpid()), arm.target, count, sizeof(Sample)};
            char path[160];
            std::snprintf(path, sizeof(path), "/data/eglantineps5/logs/mkw-sample-%u-%u.bin",
                          unsigned(getpid()), arm.target);
            if (auto* file = std::fopen(path, "wb")) {
                std::fwrite(header, sizeof(header), 1, file);
                std::fwrite(samples.data(), sizeof(Sample), count, file);
                std::fclose(file);
            }
            state.store(3, std::memory_order_release);
        }
    }).detach();
}
}
extern "C" void mkw_native_block(unsigned long long location, const void* entry, unsigned long long size) {
    const auto i = MkwNativeSample::block_count.fetch_add(1);
    if (i >= MkwNativeSample::blocks.size()) { MkwNativeSample::block_overflow.fetch_add(1); return; }
    auto& b = MkwNativeSample::blocks[i]; b.size.store(size); b.location.store(location);
    b.entry.store(reinterpret_cast<uintptr_t>(entry), std::memory_order_release);
}

extern "C" void mkw_native_fiber_stack(void* base, size_t bytes) {
    MkwNativeSample::RecordFiberStack(base, bytes);
}
