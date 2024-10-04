// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <string>
#include <string_view>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <turboq/BoundedSPSCRawQueue.h>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

template <class T, std::size_t Extent>
struct type_caster<std::span<T, Extent>> : list_caster<std::span<T, Extent>, T> {};

NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)

using namespace nanobind::literals;

NB_MODULE(pyturboq, m) {
#if 0
  nanobind::class_<turboq::MappedRegion>(m, "MappedRegion");
#endif

  nanobind::class_<turboq::BoundedSPSCRawQueue::Producer>(m, "BoundedSPSCRawQueueProducer")
      .def("enqueue", [](turboq::BoundedSPSCRawQueue::Producer& self, std::string_view s) -> bool {
        auto result = self.prepare(s.size());
        if (result.empty()) {
          return false;
        }
        std::memcpy(result.data(), s.data(), s.size());
        self.commit();
        return true;
      });

  nanobind::class_<turboq::BoundedSPSCRawQueue::Consumer>(m, "BoundedSPSCRawQueueConsumer")
      .def("dequeue", [](turboq::BoundedSPSCRawQueue::Consumer& self) -> std::string {
        auto result = self.fetch();
        if (!result.empty()) {
          std::string data(std::bit_cast<char const*>(result.data()), result.size());
          self.consume();
          return data;
        }
        return std::string();
      });

  m.def("create_consumer", [](std::string_view name) -> turboq::BoundedSPSCRawQueue::Consumer {
    return turboq::BoundedSPSCRawQueue(name).createConsumer();
  });
}
