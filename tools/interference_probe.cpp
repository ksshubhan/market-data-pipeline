// Standalone one-shot probe: reports the standard library's interference
// constants together with the toolchain and library version that produced
// them. Not a CMake target. Compile by hand, commit the output once.
//
// The value must always be quoted with the library version (PROJECT_PLAN
// section 5), which is why the version stamp is printed alongside it.
//
// Read the constants through <new>, never through a macro dump: under
// --param=destructive-interference-size=N, "g++ -dM -E -x c++ /dev/null"
// still reports the target default while a real compilation sees N.

#include <cstddef>
#include <iostream>
#include <new>

int main()
{
    std::cout << "probe: interference constants\n";

#if defined(__clang__)
    std::cout << "compiler: clang " << __clang_version__ << '\n';
#elif defined(__GNUC__)
    std::cout << "compiler: gcc "
              << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__
              << '\n';
#else
    std::cout << "compiler: unrecognised\n";
#endif

#if defined(_LIBCPP_VERSION)
    std::cout << "library: libc++ _LIBCPP_VERSION " << _LIBCPP_VERSION << '\n';
#elif defined(__GLIBCXX__)
    std::cout << "library: libstdc++ __GLIBCXX__ " << __GLIBCXX__
              << " _GLIBCXX_RELEASE " << _GLIBCXX_RELEASE << '\n';
#else
    std::cout << "library: unrecognised\n";
#endif

#if defined(__aarch64__)
    std::cout << "target: aarch64\n";
#elif defined(__x86_64__)
    std::cout << "target: x86_64\n";
#else
    std::cout << "target: other\n";
#endif

    const std::size_t destructive = std::hardware_destructive_interference_size;
    const std::size_t constructive = std::hardware_constructive_interference_size;

    std::cout << "hardware_destructive_interference_size: " << destructive << '\n';
    std::cout << "hardware_constructive_interference_size: " << constructive << '\n';

#if defined(__GCC_DESTRUCTIVE_SIZE)
    std::cout << "__GCC_DESTRUCTIVE_SIZE: " << __GCC_DESTRUCTIVE_SIZE << '\n';
    std::cout << "macro agrees with <new>: "
              << (__GCC_DESTRUCTIVE_SIZE == destructive ? "yes" : "NO") << '\n';
#else
    std::cout << "__GCC_DESTRUCTIVE_SIZE: undefined\n";
#endif

#if defined(__GCC_CONSTRUCTIVE_SIZE)
    std::cout << "__GCC_CONSTRUCTIVE_SIZE: " << __GCC_CONSTRUCTIVE_SIZE << '\n';
#else
    std::cout << "__GCC_CONSTRUCTIVE_SIZE: undefined\n";
#endif

    return 0;
}