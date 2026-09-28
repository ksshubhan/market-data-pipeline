// Prints the C++-side facts about this machine and toolchain that a shell
// script cannot read directly: the page size from two APIs, the libc++
// version, and the standard library's two interference-size constants.
//
// Called by env/dump_environment.sh, which pastes this output verbatim
// under "=== c++ environment ===" in every committed env/ dump, so keep
// the line format unchanged. macOS only: sysctlbyname is unguarded. See
// ARCHITECTURE.md.

#include <cstddef>
#include <iostream>
#include <new>

#include <sys/sysctl.h>
#include <unistd.h>

int main()
{
    const long sysconf_page_size = sysconf(_SC_PAGESIZE);

    std::size_t sysctl_page_size = 0;
    std::size_t sysctl_page_size_length = sizeof(sysctl_page_size);

    if (sysctlbyname(
            "hw.pagesize",
            &sysctl_page_size,
            &sysctl_page_size_length,
            nullptr,
            0
        ) != 0) {
        std::cerr << "failed to read hw.pagesize\n";
        return 1;
    }

    std::cout
        << "sysconf(_SC_PAGESIZE): "
        << sysconf_page_size
        << '\n';

    std::cout
        << "sysctlbyname(hw.pagesize): "
        << sysctl_page_size
        << '\n';

// Only LLVM's libc++ defines _LIBCPP_VERSION. It records which library the
// build actually used, since Homebrew clang uses its own libc++ rather
// than Apple's.
#ifdef _LIBCPP_VERSION
    std::cout
        << "_LIBCPP_VERSION: "
        << _LIBCPP_VERSION
        << '\n';
#else
    std::cout << "_LIBCPP_VERSION: unavailable\n";
#endif

    // These are the library's compile-time constants, not measurements of
    // this machine. They are recorded so they can be compared against the
    // measured coherence granule.
    std::cout
        << "hardware_destructive_interference_size: "
        << std::hardware_destructive_interference_size
        << '\n';

    std::cout
        << "hardware_constructive_interference_size: "
        << std::hardware_constructive_interference_size
        << '\n';

    return 0;
}