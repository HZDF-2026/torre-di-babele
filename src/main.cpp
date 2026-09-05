// main.cpp — entry point.
#include <string>
#include <vector>

#include "cli.h"

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    return gr::runCli(args);
}
