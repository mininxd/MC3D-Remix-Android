#include "core/log.h"
#include "crash_log.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if __has_include("build_info.h")
#include "build_info.h"
#endif
#ifndef MC3D_COMMIT
#define MC3D_COMMIT "unknown"
#endif

namespace logx {

namespace {

struct LogEntry {
    double timestamp;
    Level level;
    uint32_t tid;
    std::string text;
};

constexpr size_t kRingCapacity = 4096;
static std::mutex g_mutex;
static std::vector<LogEntry> g_ring(kRingCapacity);
static size_t g_head = 0;
static size_t g_count = 0;
static auto g_start = std::chrono::steady_clock::now();
static std::string g_path = "memory://ring_buffer";
static std::atomic<bool> g_handling_crash{false};

const char* LevelName(Level lv) {
    switch (lv) {
        case Level::Debug:  return "debug";
        case Level::Info:   return "info";
        case Level::Warn:   return "warn";
        case Level::Error:  return "error";
        case Level::Report: return "report";
    }
    return "unknown";
}

int AndroidPriority(Level lv) {
    switch (lv) {
        case Level::Debug:  return ANDROID_LOG_DEBUG;
        case Level::Info:   return ANDROID_LOG_INFO;
        case Level::Warn:   return ANDROID_LOG_WARN;
        case Level::Error:  return ANDROID_LOG_ERROR;
        case Level::Report: return ANDROID_LOG_INFO;
    }
    return ANDROID_LOG_INFO;
}

// Unwind backtrace helper
struct BacktraceState {
    void** current;
    void** end;
};

_Unwind_Reason_Code UnwindCallback(struct _Unwind_Context* context, void* arg) {
    auto* state = static_cast<BacktraceState*>(arg);
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc) {
        if (state->current == state->end) return _URC_END_OF_STACK;
        *state->current++ = reinterpret_cast<void*>(pc);
    }
    return _URC_NO_REASON;
}

std::string CaptureBacktrace(int skip = 1, int max_frames = 64) {
    void* buffer[64];
    if (max_frames > 64) max_frames = 64;
    BacktraceState state{buffer, buffer + max_frames};
    _Unwind_Backtrace(UnwindCallback, &state);
    size_t count = state.current - buffer;

    std::ostringstream ss;
    for (size_t i = skip; i < count; i++) {
        void* addr = buffer[i];
        Dl_info info;
        if (dladdr(addr, &info) && info.dli_fname) {
            const char* sname = info.dli_sname ? info.dli_sname : "???";
            ptrdiff_t offset = reinterpret_cast<char*>(addr) - reinterpret_cast<char*>(info.dli_saddr);
            ptrdiff_t foffset = reinterpret_cast<char*>(addr) - reinterpret_cast<char*>(info.dli_fbase);
            char line[512];
            std::snprintf(line, sizeof(line), "  #%02zu pc %016tx  %s (%s+%td)\n",
                          i - skip, foffset, info.dli_fname, sname, offset);
            ss << line;
        } else {
            char line[256];
            std::snprintf(line, sizeof(line), "  #%02zu pc %p  <unknown>\n", i - skip, addr);
            ss << line;
        }
    }
    return ss.str();
}

static void CrashSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    if (g_handling_crash.exchange(true)) {
        _exit(128 + sig);
    }

    const char* sig_name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: sig_name = "SIGSEGV (Segmentation fault / Invalid memory access)"; break;
        case SIGABRT: sig_name = "SIGABRT (Aborted / Assertion failure / Guest trap)"; break;
        case SIGBUS:  sig_name = "SIGBUS (Bus error / Misaligned memory access)"; break;
        case SIGFPE:  sig_name = "SIGFPE (Floating-point exception)"; break;
        case SIGILL:  sig_name = "SIGILL (Illegal instruction)"; break;
        case SIGTRAP: sig_name = "SIGTRAP (Trace/Breakpoint trap)"; break;
    }

    char details[512];
    std::snprintf(details, sizeof(details),
                  "Signal: %d [%s]\nCode: %d\nFaulting Address: %p\nFaulting Thread ID: %d",
                  sig, sig_name, info ? info->si_code : 0, info ? info->si_addr : nullptr,
                  static_cast<int>(gettid()));

    SaveCrashReport("Fatal Native Signal", details, ucontext);

    // Re-raise with default action
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

}  // namespace

void Init(const std::string&, const std::string&, bool) {
    // In-memory ring buffer is always active from startup.
    // We install the native crash handlers on initialization.
    InstallCrashHandlers();
}

const std::string& Path() {
    return g_path;
}

void Write(Level lv, const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    auto tid = static_cast<uint32_t>(gettid());

    // 1. Always forward to Android logcat for real-time monitoring
    __android_log_print(AndroidPriority(lv), "MC3DR", "[%9.3f] [%s] [t%04x] %s",
                        t, LevelName(lv), tid, msg);

    // 2. Store in memory ring buffer (Zero disk writes during normal play)
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ring[g_head] = LogEntry{t, lv, tid, msg};
    g_head = (g_head + 1) % kRingCapacity;
    if (g_count < kRingCapacity) g_count++;
}

void Flush() {
    // Ring buffer lives in RAM; nothing to flush during normal operation
}

void InstallCrashHandlers() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // Alternate signal stack for stack exhaustion crashes
    static uint8_t alt_stack[65536];
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = CrashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
}

void SaveCrashReport(const char* category, const char* details, const void*) {
    // Collect UTC timestamp
    std::time_t now = std::time(nullptr);
    char time_str[64] = {};
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&now));

    std::ostringstream ss;
    ss << "================================================================================\n";
    ss << "       MIDNIGHT CLUB 3: DUB EDITION REMIX - APPLICATION CRASH REPORT\n";
    ss << "================================================================================\n";
    ss << "Timestamp:    " << time_str << "\n";
    ss << "Package:      io.github.mininxd.midnightclub3remixrecomp\n";
    ss << "Build Commit: " << MC3D_COMMIT << "\n";
    ss << "Architecture: aarch64-linux-android\n";
    ss << "Crash Type:   " << (category ? category : "Fatal Crash") << "\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Crash Details:\n";
    if (details && details[0]) {
        ss << details << "\n";
    } else {
        ss << "No additional error message provided.\n";
    }
    ss << "\n--------------------------------------------------------------------------------\n";
    ss << "Native Callstack Backtrace:\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << CaptureBacktrace(2, 48);

    ss << "\n--------------------------------------------------------------------------------\n";
    ss << "Pre-Crash Execution Log (In-Memory History leading to crash):\n";
    ss << "--------------------------------------------------------------------------------\n";

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        size_t start_idx = (g_count < kRingCapacity) ? 0 : g_head;
        for (size_t i = 0; i < g_count; i++) {
            const auto& e = g_ring[(start_idx + i) % kRingCapacity];
            char line[512];
            std::snprintf(line, sizeof(line), "[%9.3f] [%-6s] [t%04x] %s\n",
                          e.timestamp, LevelName(e.level), e.tid, e.text.c_str());
            ss << line;
        }
    }

    ss << "================================================================================\n";
    ss << "End of Crash Report\n";
    ss << "================================================================================\n";

    std::string report = ss.str();

    // 1. Output to Android logcat
    __android_log_write(ANDROID_LOG_FATAL, "MC3DR_Crash", report.c_str());

    // 2. Persist ONLY ON CRASH to external storage paths
    const char* const kOutputPaths[] = {
        "/storage/emulated/0/mc3dr/crash_log.txt",
        "/sdcard/mc3dr/crash_log.txt",
        "/sdcard/mc3dr/app_logs.txt",
        "/storage/emulated/0/mc3dr/app_logs.txt"
    };

    for (const char* path : kOutputPaths) {
        FILE* f = std::fopen(path, "w");
        if (f) {
            std::fwrite(report.data(), 1, report.size(), f);
            std::fflush(f);
            fsync(fileno(f));
            std::fclose(f);
        }
    }
}

}  // namespace logx

// JNI export so Java uncaught exceptions dump through this same crash pipeline
extern "C" JNIEXPORT void JNICALL
Java_io_github_mininxd_midnightclub3remixrecomp_MC3DActivity_nativeRecordCrash(
    JNIEnv* env, jclass, jstring category, jstring details) {
    const char* cat = category ? env->GetStringUTFChars(category, nullptr) : "Java Exception";
    const char* det = details ? env->GetStringUTFChars(details, nullptr) : "";
    logx::SaveCrashReport(cat, det);
    if (category && cat) env->ReleaseStringUTFChars(category, cat);
    if (details && det) env->ReleaseStringUTFChars(details, det);
}
