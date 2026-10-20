// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include <print>

#include <cxxopts.hpp>

int main(int argc, char* argv[]) {
    try {

    } catch (std::exception const& e) {
        std::print(stderr, "ERROR: {}\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
