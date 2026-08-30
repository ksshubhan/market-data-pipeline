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

#ifdef _LIBCPP_VERSION
    std::cout
        << "_LIBCPP_VERSION: "
        << _LIBCPP_VERSION
        << '\n';
#else
    std::cout << "_LIBCPP_VERSION: unavailable\n";
#endif

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