// Prints the C++ standard library's interference constants beside the
// compiler, library and target that produced them and, where GCC's
// __GCC_DESTRUCTIVE_SIZE macro is defined, whether <new> matches it.
//
// Built directly, not through CMake: tools/mcpu_sweep.py compiles it once
// per -mcpu target and parses its output, and the README gives the
// single-build command. Committed output is in
// evidence/interference_libstdcxx_gcc15_aarch64.txt and
// evidence/mcpu_sweep_gcc_aarch64_20260919.txt.

#include <cstddef>
#include <iostream>
#include <new>

int main()
{
    std::cout << "probe: interference constants\n";

    // The constants are chosen by the toolchain, not measured, so they are
    // printed with the versions that produced them. Under GCC 15 on aarch64
    // they also depend on -mcpu, which this program does not print;
    // mcpu_sweep.py records each build command.
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

    // Read from <new> in a real build, not from a macro dump: with
    // --param=destructive-interference-size=N,
    // "g++ -dM -E -x c++ /dev/null" still prints the target default,
    // while a real build sees N.
    const std::size_t destructive = std::hardware_destructive_interference_size;
    const std::size_t constructive = std::hardware_constructive_interference_size;

    // mcpu_sweep.py finds this line and "macro agrees with <new>" by their
    // labels, and committed evidence contains both, so the labels are fixed.
    std::cout << "hardware_destructive_interference_size: " << destructive << '\n';
    std::cout << "hardware_constructive_interference_size: " << constructive << '\n';

    // libstdc++ 13 and 15 and libc++ 18 define the <new> constants as these
    // macros, so with them this cannot say NO: it shows only that the macro
    // is defined.
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