// Defaults for the sanitizers (when the tests are built with them), which their runtimes ask the program for. The
// environment's ASAN_OPTIONS and LSAN_OPTIONS still override them.

extern "C"
{

#if defined(__APPLE__) && !defined(__apple_build_version__)
// LeakSanitizer is on by default on Linux, but not on macOS, where LLVM's clang supports it, but Apple's doesn't: its
// ASan fails to start if asked for it.
[[gnu::visibility("default"), gnu::used]] const char* __asan_default_options() { return "detect_leaks=1"; }
#endif

// Leaks in system libraries, rather than this project.
[[gnu::visibility("default"), gnu::used]] const char* __lsan_default_suppressions()
{
    // macOS: getaddrinfo() sets up process-wide state in the Network framework, to synthesize NAT64 addresses, which it
    // never frees.
    return "leak:_gai_nat64_second_pass\n";
}

}
