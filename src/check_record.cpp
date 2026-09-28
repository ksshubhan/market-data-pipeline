// Prints sizeof(Record). A quick manual check from early in the project;
// the size is enforced at compile time by the static_assert in
// record.hpp, so this program adds no protection of its own. Not a ctest
// entry. See ARCHITECTURE.md.

#include <iostream>
#include "record.hpp"

int main() {
    std::cout << "sizeof(Record) = "
              << sizeof(Record)
              << " bytes\n";

    return 0;
}