// backtestx: thin dispatcher. All command logic lives in bt_core (src/commands.cpp) so tests can call it in-process.
#include <iostream>
#include <string>
#include <vector>

#include "bt/commands.hpp"

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  return bt::cli_main(args, std::cout, std::cerr);
}
