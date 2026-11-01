// Defaults for the sanitizers (when the tests are built with them), which their runtimes ask the program for. The
// environment's ASAN_OPTIONS and LSAN_OPTIONS still override them.

extern "C"
{
#if defined(__APPLE__) && !defined(__apple_build_version__)
// LeakSanitizer is on by default on Linux, but not on macOS, where LLVM's clang supports it, but Apple's doesn't: its
// ASan fails to start if asked for it.
// NOLINTNEXTLINE(*-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the sanitizer runtime looks for this name.
[[gnu::visibility("default"), gnu::used]] const char* __asan_default_options() { return "detect_leaks=1"; }
#endif

// Leaks in system libraries, rather than this project.
// NOLINTNEXTLINE(*-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the sanitizer runtime looks for this name.
[[gnu::visibility("default"), gnu::used]] const char* __lsan_default_suppressions()
{
    // macOS: getaddrinfo() sets up process-wide state in the Network framework, which it never frees: to synthesize
    // NAT64 addresses, and (resolving a name, rather than a number) through nw_path_libinfo_path_check, once.
    return "leak:_gai_nat64_second_pass\n"
           "leak:nw_path_libinfo_path_check\n";
}

// Races ThreadSanitizer reports in code it can't see, rather than this project's.
// NOLINTNEXTLINE(*-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the sanitizer runtime looks for this name.
[[gnu::visibility("default"), gnu::used]] const char* __tsan_default_suppressions()
{
    // Botan (built by vcpkg without -fsanitize=thread): its cipher suites are a function-local static vector
    // (Ciphersuite::all_known_ciphersuites()), made once under the static's guard and never changed. A thread that
    // finds it made already does so in Botan's uninstrumented code, so TSan never sees it synchronize with the thread
    // that made it, and reports that thread's reads (e.g. our policy's, of a suite's code) as racing the construction:
    // when several clients' first handshakes run at once.
    return "race:Botan::TLS::Ciphersuite\n";
}
}
