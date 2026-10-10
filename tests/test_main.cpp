#include "support.hpp"
#include <iostream>
#include <string_view>
int main(int argc, char** argv) {
  const std::string_view filter = argc == 3 && std::string_view(argv[1]) == "--filter" ? argv[2] : "";
  int failures = 0;
  int executed = 0;
  for (const auto& test : test::Cases()) if (filter.empty() || std::string_view(test.name).starts_with(filter)) {
    ++executed;
    try { test.run(); } catch (const std::exception& error) { ++failures; std::cerr << test.name << ": " << error.what() << '\n'; }
  }
  std::cout << executed - failures << "/" << executed << " tests passed\n";
  return failures == 0 ? 0 : 1;
}
