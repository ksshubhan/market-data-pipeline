#include <iostream>
#include <version>
#include <new>



int main() {
    std::cout << "clang major: " << __clang_major__ << '\n';

    std::cout << "clang minor: " << __clang_minor__ << '\n';

    std::cout << "clang patchlevel: " << __clang_patchlevel__ << '\n';

    std::cout << "cplusplus: " << __cplusplus << '\n';

    std::cout << "_LIBCPP_VERSION: " << _LIBCPP_VERSION << '\n';

    #ifdef __cpp_lib_hardware_interference_size
        std::cout << "destructive: " << std::hardware_destructive_interference_size << '\n';
        std::cout << "constructive: " << std::hardware_constructive_interference_size << '\n';
    #else
        std::cout << "interference sizes: not provided\n";
    #endif
    
    return 0;
}

