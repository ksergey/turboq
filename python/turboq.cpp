// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <nanobind/nanobind.h>

#include <turboq/BoundedMPSCRawQueue.h>
#include <turboq/BoundedSPMCRawQueue.h>
#include <turboq/BoundedSPSCRawQueue.h>

using namespace nanobind::literals;

NB_MODULE(pyturboq, m) {
  m.def(
      "sample",
      [](int a, int b = 1) {
        return a + b;
      },
      "a"_a, "b"_a = 1, "sample text");
}
