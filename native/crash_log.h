#pragma once
#include <string>

namespace logx {

// Installs POSIX signal handlers (SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP)
// using an alternate signal stack to catch native crashes and out-of-stack issues.
void InstallCrashHandlers();

// Saves the entire in-memory execution log buffer along with stack backtrace and crash
// diagnostics to /sdcard/mc3dr/crash_log.txt and /storage/emulated/0/mc3dr/crash_log.txt.
// Called ONLY when the application encounters a fatal error, unhandled signal, or exception.
void SaveCrashReport(const char* category, const char* details = nullptr, const void* ucontext = nullptr);

}  // namespace logx
