#include "core/thread_stats.hpp"

#include <algorithm>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>
#else
#include <pthread.h>
#include <time.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#endif
#endif

namespace vsa {
namespace {

#if defined(_WIN32)

double filetime_ms(const FILETIME& kernel, const FILETIME& user) noexcept {
    const auto to_100ns = [](const FILETIME& t) {
        return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(to_100ns(kernel) + to_100ns(user)) / 10'000.0;
}

double handle_cpu_ms(HANDLE handle) noexcept {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (handle == nullptr || GetThreadTimes(handle, &created, &exited, &kernel, &user) == 0) {
        return 0.0;
    }
    return filetime_ms(kernel, user);
}

/// The base name of the module a thread's start address lies in, "" if unknown.
std::string start_module(HANDLE thread) {
    using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const QueryFn query = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr ? reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQueryInformationThread")) : nullptr;
    }();
    if (query == nullptr) {
        return {};
    }
    constexpr int kThreadQuerySetWin32StartAddress = 9;
    void* start = nullptr;
    if (query(thread, kThreadQuerySetWin32StartAddress, &start, sizeof start, nullptr) != 0 || start == nullptr) {
        return {};
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(start), &module) == 0) {
        return {};
    }
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0) {
        return {};
    }
    std::wstring wide(path, length);
    const std::size_t slash = wide.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        wide.erase(0, slash + 1);
    }
    std::string name;
    name.reserve(wide.size());
    for (const wchar_t c : wide) {
        name.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return name;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

#elif defined(__linux__)

/// utime + stime of a thread of this process from /proc, in ms; 0 if unreadable.
double task_cpu_ms(uint32_t id) noexcept {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%u/stat", id);
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        return 0.0;
    }
    char line[1024];
    const bool read = std::fgets(line, sizeof line, f) != nullptr;
    std::fclose(f);
    if (!read) {
        return 0.0;
    }
    // Fields after the parenthesised command name: state is 3, utime 14, stime 15.
    const char* p = std::strrchr(line, ')');
    if (p == nullptr) {
        return 0.0;
    }
    unsigned long utime = 0;
    unsigned long stime = 0;
    if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) != 2) {
        return 0.0;
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    return ticks > 0 ? static_cast<double>(utime + stime) * 1000.0 / static_cast<double>(ticks) : 0.0;
}

#endif

}  // namespace

ThreadRegistry& ThreadRegistry::instance() {
    static ThreadRegistry registry;
    return registry;
}

ThreadRegistry::~ThreadRegistry() {
    for (Entry& entry : entries_) {
        close(entry);
    }
}

uint32_t ThreadRegistry::current_id() noexcept {
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<uint32_t>(syscall(SYS_gettid));
#else
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<uint32_t>(id);
#endif
}

void ThreadRegistry::close(Entry& entry) noexcept {
#if defined(_WIN32)
    if (entry.handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(entry.handle));
        entry.handle = nullptr;
    }
#else
    entry.clock = -1;
#endif
}

double ThreadRegistry::cpu_ms(const Entry& entry) noexcept {
#if defined(_WIN32)
    return handle_cpu_ms(static_cast<HANDLE>(entry.handle));
#elif defined(__APPLE__)
    // `clock` holds a Mach thread port here, not a clock id; see register_current.
    if (entry.clock > 0) {
        thread_basic_info_data_t info{};
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(static_cast<thread_inspect_t>(entry.clock), THREAD_BASIC_INFO,
                        reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS) {
            const auto ms = [](const time_value_t& t) {
                return static_cast<double>(t.seconds) * 1000.0 + static_cast<double>(t.microseconds) / 1000.0;
            };
            return ms(info.user_time) + ms(info.system_time);
        }
    }
    return 0.0;
#else
    if (entry.clock >= 0) {
        timespec ts{};
        if (clock_gettime(static_cast<clockid_t>(entry.clock), &ts) == 0) {
            return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
        }
    }
#if defined(__linux__)
    return task_cpu_ms(entry.id);
#else
    return 0.0;
#endif
#endif
}

void ThreadRegistry::register_current(const std::string& name) {
    const uint32_t id = current_id();
    Entry entry;
    entry.name = name;
    entry.id = id;
#if defined(_WIN32)
    HANDLE handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0) {
        handle = nullptr;
    }
    entry.handle = handle;
    std::wstring wide(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), wide.c_str());
#elif defined(__APPLE__)
    // macOS has no pthread_getcpuclockid. The thread's Mach port answers the same question through
    // thread_info, so `clock` carries the port here. pthread_mach_thread_np hands it over without
    // taking a reference: there is nothing to deallocate, and it stops answering once the thread
    // ends, which is exactly when the entry stops being read.
    entry.clock = static_cast<long>(pthread_mach_thread_np(pthread_self()));
    pthread_setname_np(name.c_str());
#else
    clockid_t clock{};
    entry.clock = pthread_getcpuclockid(pthread_self(), &clock) == 0 ? static_cast<long>(clock) : -1;
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
    std::lock_guard lock(mutex_);
    for (Entry& existing : entries_) {
        if (existing.id == id) {
            close(existing);
            existing = std::move(entry);
            return;
        }
    }
    entries_.push_back(std::move(entry));
}

void ThreadRegistry::unregister_current() {
    const uint32_t id = current_id();
    std::lock_guard lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->id == id) {
            close(*it);
            entries_.erase(it);
            return;
        }
    }
}

void ThreadRegistry::announce(uint32_t id, const std::string& name) {
    if (id == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    for (Entry& existing : entries_) {
        if (existing.id == id) {
            existing.name = name;
            return;
        }
    }
    Entry entry;
    entry.name = name;
    entry.id = id;
#if defined(_WIN32)
    entry.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
#endif
    entries_.push_back(std::move(entry));
}

std::vector<ThreadSample> ThreadRegistry::sample() const {
    std::vector<ThreadSample> samples;
    std::unordered_set<uint32_t> seen;
    {
        std::lock_guard lock(mutex_);
        samples.reserve(entries_.size());
        for (const Entry& entry : entries_) {
            ThreadSample s;
            s.name = entry.name;
            s.kind = ThreadKind::Engine;
            s.id = entry.id;
            s.cpu_ms = cpu_ms(entry);
            samples.push_back(std::move(s));
            seen.insert(entry.id);
        }
    }
#if defined(_WIN32)
    const DWORD process = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return samples;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Thread32First(snapshot, &entry); more != 0; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || seen.contains(entry.th32ThreadID)) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        ThreadSample s;
        s.id = entry.th32ThreadID;
        s.cpu_ms = handle_cpu_ms(thread);
        const std::string module = start_module(thread);
        CloseHandle(thread);
        // By module, exactly (a substring would take the test executable for the engine):
        // Steam Audio's own; Embree's and TBB's workers, which build our scene; the engine's
        // threads that did not register; everything else by its module.
        const std::string key = lower(module);
        if (key == "phonon.dll") {
            s.kind = ThreadKind::SteamAudio;
            s.name = "steam audio worker";
        } else if (key.starts_with("embree") || key.starts_with("tbb")) {
            s.kind = ThreadKind::Engine;
            s.name = "scene build worker (" + module + ")";
        } else if (key == "vsaudio.dll") {
            s.kind = ThreadKind::Engine;
            s.name = "engine (unnamed)";
        } else {
            s.kind = ThreadKind::Other;
            s.name = module.empty() ? "unknown" : module;
        }
        samples.push_back(std::move(s));
    }
    CloseHandle(snapshot);
#endif
    return samples;
}

}  // namespace vsa
