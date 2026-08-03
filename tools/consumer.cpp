// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <cstdio>
#include <cstdlib>
#include <exception>

#include <turboq/BoundedMPSCRawQueue.h>
#include <turboq/BoundedMulticastRawQueue.h>
#include <turboq/BoundedSPSCRawQueue.h>

#include <cxxopts.hpp>

int main(int argc, char* argv[]) {
    try {

    } catch (std::exception const& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
