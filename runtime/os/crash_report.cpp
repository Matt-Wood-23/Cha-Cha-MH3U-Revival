// Fatal exceptions other than access violations (guest_memory.cpp reports those): stack overflow, integer divide by
// zero or overflow, illegal instruction, an uncaught C++ exception, abort(). Without this the process just vanishes
// (Git Bash shows exit code 127). Prints the code, where, the host thread's name and a short backtrace as
// module+offset, which Cha-Cha.map (next to the exe) turns into function names.
#ifdef _WIN32
#include <windows.h>
#endif

#include "hle.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace mh3u::os {
namespace {

#ifdef _WIN32
void where(const void* addr, const char* prefix) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    std::fprintf(stderr, "%s%s+0x%llX\n", prefix, base,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
}

void thread_and_backtrace() {
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) {
        std::fprintf(stderr, "  host thread %lu: %ls\n", GetCurrentThreadId(), desc);
        LocalFree(desc);
    }
    void* frames[40];
    const USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) where(frames[i], "  bt ");
    std::fflush(stderr);
}

bool fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION: case 0xC0000374: /* heap corruption */
            return true;
        default: return false;
    }
}

LONG CALLBACK first_chance(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (!fatal(r->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr, "host exception %08lX\n", r->ExceptionCode);
    where(r->ExceptionAddress, "  at ");
    std::fflush(stderr);
    thread_and_backtrace();
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (!fatal(r->ExceptionCode)) {  // the rest (fatal ones were reported first-chance)
        std::fprintf(stderr, "unhandled host exception %08lX\n", r->ExceptionCode);
        where(r->ExceptionAddress, "  at ");
        thread_and_backtrace();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_terminate() {
    std::fprintf(stderr, "std::terminate");
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            std::fprintf(stderr, ": uncaught %s", x.what());
        } catch (const GuestExit& x) {
            std::fprintf(stderr, ": GuestExit(%d) on a thread that doesn't catch it", x.code);
        } catch (...) {
            std::fprintf(stderr, ": uncaught exception of another type");
        }
    }
    std::fprintf(stderr, "\n");
    thread_and_backtrace();
    std::_Exit(4);
}

// a C runtime function given an argument it refuses (a bad printf format, ...): it would end the process with a
// fast fail (0xC0000409, code 5), which no exception handler sees
void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    std::fprintf(stderr, "C runtime: invalid parameter\n");
    thread_and_backtrace();
    std::_Exit(4);
}

void on_abort(int) {
    std::fprintf(stderr, "abort()\n");
    thread_and_backtrace();
    std::_Exit(4);
}
#endif

}  // namespace

void install_crash_report() {
#ifdef _WIN32
    AddVectoredExceptionHandler(0, first_chance);
    SetUnhandledExceptionFilter(unhandled);
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_abort);
    _set_invalid_parameter_handler(on_invalid_parameter);
#endif
}

}  // namespace mh3u::os
