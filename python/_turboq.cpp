// Python bindings for ksergey/turboq (https://github.com/ksergey/turboq)
//
// turboq is a header-only C++23 library of shared-memory message queues
// (SPSC / MPSC / Multicast). This file wraps its prepare()/commit() and
// fetch()/consume() zero-copy API with a bytes-based Python API using
// pybind11.
//
// SPDX-License-Identifier: MIT

#include <cstring>
#include <optional>
#include <string>
#include <system_error>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "turboq/MPSCMessageQueue.h"
#include "turboq/MemorySource.h"
#include "turboq/MulticastMessageQueue.h"
#include "turboq/SPSCMessageQueue.h"

namespace py = pybind11;

namespace {

/// Build a MemorySource on the fly.
/// - path is empty and anonymous == false -> DefaultMemorySource() (auto: /dev/shm or /tmp)
/// - path is non-empty                    -> DefaultMemorySource(path)
/// - anonymous == true                    -> AnonymousMemorySource() (single-process only,
///                                            producer and consumer must be created from the
///                                            SAME queue object, in the SAME process -- there is
///                                            no name-based lookup across processes for this one)
std::unique_ptr<turboq::MemorySource> makeMemorySource(std::string const& path, bool anonymous) {
    if (anonymous) {
        return std::make_unique<turboq::AnonymousMemorySource>();
    }
    if (!path.empty()) {
        return std::make_unique<turboq::DefaultMemorySource>(std::filesystem::path{path});
    }
    return std::make_unique<turboq::DefaultMemorySource>();
}

/// Convert a raw std::span<std::byte const> into a Python bytes object (one copy -- this is the
/// bindings' price for a safe, GC-friendly bytes-based API; the C++ side stays zero-copy).
py::bytes toBytes(std::span<std::byte const> buffer) {
    return py::bytes(reinterpret_cast<char const*>(buffer.data()), buffer.size());
}

/// system_error -> a Python exception carrying category/message/errno-like value, so
/// `except turboq.TurboqError as e: e.args` is actually useful.
void translateSystemError(std::system_error const& e) {
    PyErr_SetString(PyExc_RuntimeError, e.what());
}

// ---------------------------------------------------------------------------------------------
// Generic producer/consumer wrappers, templated over the turboq queue's nested Producer/Consumer
// types. All three queue kinds (SPSC, MPSC, Multicast) share the same prepare/commit/fetch/
// consume shape, so one template pair covers all three Python classes.
// ---------------------------------------------------------------------------------------------

template <typename ProducerImpl>
class ProducerWrapper {
public:
    explicit ProducerWrapper(ProducerImpl&& impl) : impl_{std::move(impl)} {}

    [[nodiscard]] bool valid() const {
        return static_cast<bool>(impl_);
    }

    /// Try to enqueue `payload`. Returns False if there isn't currently enough free space
    /// (queue full / message too large for an MPSC slot raises instead, matching prepare()).
    [[nodiscard]] bool send(py::buffer const& payload) {
        py::buffer_info info = payload.request();
        auto const size = static_cast<std::size_t>(info.size) * static_cast<std::size_t>(info.itemsize);
        auto buffer = impl_.prepare(size);
        if (buffer.empty() && size != 0) {
            return false;
        }
        if (size != 0) {
            std::memcpy(buffer.data(), info.ptr, size);
        }
        impl_.commit();
        return true;
    }

    ProducerImpl impl_;
};

template <typename ConsumerImpl>
class ConsumerWrapper {
public:
    explicit ConsumerWrapper(ConsumerImpl&& impl) : impl_{std::move(impl)} {}

    [[nodiscard]] bool valid() const {
        return static_cast<bool>(impl_);
    }

    /// Return the next message as bytes, or None if the queue is currently empty.
    [[nodiscard]] std::optional<py::bytes> receive() {
        auto buffer = impl_.fetch();
        if (buffer.empty()) {
            return std::nullopt;
        }
        auto result = toBytes(buffer);
        impl_.consume();
        return result;
    }

    void reset() {
        impl_.reset();
    }

    ConsumerImpl impl_;
};

// ---------------------------------------------------------------------------------------------
// Queue handle wrappers. Each corresponds to turboq::{SPSC,MPSC,Multicast}MessageQueue and owns
// the underlying shared file handle until createProducer()/createConsumer() is called.
// ---------------------------------------------------------------------------------------------

// SPSC and Multicast producers/consumers expose capacity(); MPSC's don't (it's slot-based, see
// slotSize()/length() below), so `capacity` is added per-class rather than in the base template.

class SPSCProducer : public ProducerWrapper<turboq::SPSCMessageQueue::Producer> {
public:
    using ProducerWrapper::ProducerWrapper;

    [[nodiscard]] std::size_t capacity() const {
        return impl_.capacity();
    }
};

class SPSCConsumer : public ConsumerWrapper<turboq::SPSCMessageQueue::Consumer> {
public:
    using ConsumerWrapper::ConsumerWrapper;

    [[nodiscard]] std::size_t capacity() const {
        return impl_.capacity();
    }
};

class MulticastProducer : public ProducerWrapper<turboq::MulticastMessageQueue::Producer> {
public:
    using ProducerWrapper::ProducerWrapper;

    [[nodiscard]] std::size_t capacity() const {
        return impl_.capacity();
    }
};

class MulticastConsumer : public ConsumerWrapper<turboq::MulticastMessageQueue::Consumer> {
public:
    using ConsumerWrapper::ConsumerWrapper;

    [[nodiscard]] std::size_t capacity() const {
        return impl_.capacity();
    }

    [[nodiscard]] std::size_t overrunCount() const {
        return impl_.overrunCount();
    }
};

class MPSCProducer : public ProducerWrapper<turboq::MPSCMessageQueue::Producer> {
public:
    using ProducerWrapper::ProducerWrapper;

    [[nodiscard]] std::size_t slotSize() const {
        return impl_.slotSize();
    }

    [[nodiscard]] std::size_t length() const {
        return impl_.length();
    }
};

class MPSCConsumer : public ConsumerWrapper<turboq::MPSCMessageQueue::Consumer> {
public:
    using ConsumerWrapper::ConsumerWrapper;

    [[nodiscard]] std::size_t slotSize() const {
        return impl_.slotSize();
    }

    [[nodiscard]] std::size_t length() const {
        return impl_.length();
    }
};

class SPSCQueue {
public:
    SPSCQueue(std::string const& name, std::size_t capacityHint, std::string const& path, bool anonymous) {
        auto source = makeMemorySource(path, anonymous);
        auto result = turboq::SPSCMessageQueue::makeQueue(
            name, turboq::SPSCMessageQueue::CreationOptions{.capacityHint = capacityHint}, *source);
        if (!result) {
            throw std::system_error{result.error(), "failed to create/open SPSC queue"};
        }
        impl_ = std::move(result).value();
    }

    static SPSCQueue open(std::string const& name, std::string const& path, bool anonymous) {
        return SPSCQueue{OpenOnlyTag{}, name, path, anonymous};
    }

    [[nodiscard]] SPSCProducer createProducer() {
        return SPSCProducer{impl_.createProducer()};
    }

    [[nodiscard]] SPSCConsumer createConsumer() {
        return SPSCConsumer{impl_.createConsumer()};
    }

private:
    struct OpenOnlyTag {};

    SPSCQueue(OpenOnlyTag, std::string const& name, std::string const& path, bool anonymous) {
        auto source = makeMemorySource(path, anonymous);
        auto result = turboq::SPSCMessageQueue::makeQueue(name, *source);
        if (!result) {
            throw std::system_error{result.error(), "failed to open SPSC queue"};
        }
        impl_ = std::move(result).value();
    }

    turboq::SPSCMessageQueue impl_;
};

class MPSCQueue {
public:
    MPSCQueue(std::string const& name, std::size_t slotSizeHint, std::size_t lengthHint, std::string const& path,
        bool anonymous) {
        auto source = makeMemorySource(path, anonymous);
        auto result = turboq::MPSCMessageQueue::makeQueue(name,
            turboq::MPSCMessageQueue::CreationOptions{.slotSizeHint = slotSizeHint, .lengthHint = lengthHint},
            *source);
        if (!result) {
            throw std::system_error{result.error(), "failed to create/open MPSC queue"};
        }
        impl_ = std::move(result).value();
    }

    static MPSCQueue open(std::string const& name, std::string const& path, bool anonymous) {
        return MPSCQueue{OpenOnlyTag{}, name, path, anonymous};
    }

    /// MPSC has no producer-count limit: call this once per producing thread/process.
    [[nodiscard]] MPSCProducer createProducer() {
        return MPSCProducer{impl_.createProducer()};
    }

    /// MPSC allows exactly one consumer.
    [[nodiscard]] MPSCConsumer createConsumer() {
        return MPSCConsumer{impl_.createConsumer()};
    }

private:
    struct OpenOnlyTag {};

    MPSCQueue(OpenOnlyTag, std::string const& name, std::string const& path, bool anonymous) {
        auto source = makeMemorySource(path, anonymous);
        auto result = turboq::MPSCMessageQueue::makeQueue(name, *source);
        if (!result) {
            throw std::system_error{result.error(), "failed to open MPSC queue"};
        }
        impl_ = std::move(result).value();
    }

    turboq::MPSCMessageQueue impl_;
};

class MulticastQueue {
public:
    MulticastQueue(std::string const& name, std::size_t capacityHint, std::string const& path, bool anonymous) {
        auto source = makeMemorySource(path, anonymous);
        auto result = turboq::MulticastMessageQueue::makeQueue(
            name, turboq::MulticastMessageQueue::CreationOptions{.capacityHint = capacityHint}, *source);
        if (!result) {
            throw std::system_error{result.error(), "failed to create/open multicast queue"};
        }
        impl_ = std::move(result).value();
    }

    static MulticastQueue open(std::string const& name, std::string const& path, bool anonymous) {
        return MulticastQueue{OpenOnlyTag{}, name, path, anonymous};
    }

    /// Multicast allows exactly one producer.
    [[nodiscard]] MulticastProducer createProducer() {
        return MulticastProducer{impl_.createProducer()};
    }

    /// Multicast allows any number of independent consumers (each sees only messages published
    /// after it attaches).
    [[nodiscard]] MulticastConsumer createConsumer() {
        return MulticastConsumer{impl_.createConsumer()};
    }

private:
    struct OpenOnlyTag {};

    MulticastQueue(OpenOnlyTag, std::string const& name, std::string const& path, bool anonymous) {
        auto source = makeMemorySource(path, anonymous);
        auto result = turboq::MulticastMessageQueue::makeQueue(name, *source);
        if (!result) {
            throw std::system_error{result.error(), "failed to open multicast queue"};
        }
        impl_ = std::move(result).value();
    }

    turboq::MulticastMessageQueue impl_;
};

} // namespace

PYBIND11_MODULE(_turboq, m) {
    m.doc() = "Python bindings for ksergey/turboq low-latency message queues (SPSC / MPSC / Multicast)";

    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) {
                std::rethrow_exception(p);
            }
        } catch (std::system_error const& e) {
            translateSystemError(e);
        }
    });

    // ---- SPSC ----
    py::class_<SPSCProducer>(m, "SPSCProducer")
        .def("__bool__", &SPSCProducer::valid)
        .def_property_readonly("capacity", &SPSCProducer::capacity)
        .def("send", &SPSCProducer::send, py::arg("payload"),
            "Enqueue bytes-like payload. Returns False if the queue is currently full.");

    py::class_<SPSCConsumer>(m, "SPSCConsumer")
        .def("__bool__", &SPSCConsumer::valid)
        .def_property_readonly("capacity", &SPSCConsumer::capacity)
        .def("receive", &SPSCConsumer::receive, "Dequeue next message as bytes, or None if empty.")
        .def("reset", &SPSCConsumer::reset, "Drop any unconsumed backlog.");

    py::class_<SPSCQueue>(m, "SPSCQueue",
        "Single-producer/single-consumer queue. At most one producer and one consumer may exist "
        "at a time (enforced with an OS file lock).")
        .def(py::init<std::string const&, std::size_t, std::string const&, bool>(), py::arg("name"),
            py::arg("capacity_hint"), py::arg("path") = std::string{}, py::arg("anonymous") = false,
            "Create the queue if it doesn't exist yet, or open it if it does (capacity must match).")
        .def_static("open", &SPSCQueue::open, py::arg("name"), py::arg("path") = std::string{},
            py::arg("anonymous") = false, "Open an existing queue only; raises if it doesn't exist.")
        .def("create_producer", &SPSCQueue::createProducer)
        .def("create_consumer", &SPSCQueue::createConsumer);

    // ---- MPSC ----
    py::class_<MPSCProducer>(m, "MPSCProducer")
        .def("__bool__", &MPSCProducer::valid)
        .def_property_readonly("slot_size", &MPSCProducer::slotSize)
        .def_property_readonly("length", &MPSCProducer::length)
        .def("send", &MPSCProducer::send, py::arg("payload"),
            "Enqueue bytes-like payload. Returns False if the queue is currently full. Raises if "
            "payload is larger than slot_size.");

    py::class_<MPSCConsumer>(m, "MPSCConsumer")
        .def("__bool__", &MPSCConsumer::valid)
        .def_property_readonly("slot_size", &MPSCConsumer::slotSize)
        .def_property_readonly("length", &MPSCConsumer::length)
        .def("receive", &MPSCConsumer::receive, "Dequeue next message as bytes, or None if empty.")
        .def("reset", &MPSCConsumer::reset, "Drop any unconsumed backlog.");

    py::class_<MPSCQueue>(m, "MPSCQueue",
        "Multi-producer/single-consumer queue backed by a fixed number of fixed-size slots. Any "
        "number of producers may attach; only one consumer may exist at a time.")
        .def(py::init<std::string const&, std::size_t, std::size_t, std::string const&, bool>(), py::arg("name"),
            py::arg("slot_size_hint"), py::arg("length_hint"), py::arg("path") = std::string{},
            py::arg("anonymous") = false,
            "Create the queue if it doesn't exist yet, or open it if it does. length_hint is "
            "rounded up to the next power of two.")
        .def_static("open", &MPSCQueue::open, py::arg("name"), py::arg("path") = std::string{},
            py::arg("anonymous") = false, "Open an existing queue only; raises if it doesn't exist.")
        .def("create_producer", &MPSCQueue::createProducer)
        .def("create_consumer", &MPSCQueue::createConsumer);

    // ---- Multicast ----
    py::class_<MulticastProducer>(m, "MulticastProducer")
        .def("__bool__", &MulticastProducer::valid)
        .def_property_readonly("capacity", &MulticastProducer::capacity)
        .def("send", &MulticastProducer::send, py::arg("payload"),
            "Publish bytes-like payload. Returns False only if the payload can never fit in the "
            "ring; this queue has no backpressure -- a slow consumer is simply lapped, see "
            "MulticastConsumer.overrun_count.");

    py::class_<MulticastConsumer>(m, "MulticastConsumer")
        .def("__bool__", &MulticastConsumer::valid)
        .def_property_readonly("capacity", &MulticastConsumer::capacity)
        .def_property_readonly("overrun_count", &MulticastConsumer::overrunCount,
            "Cumulative number of times this consumer was lapped by the producer since it was "
            "created or last reset().")
        .def("receive", &MulticastConsumer::receive,
            "Dequeue next message as bytes, or None if empty (including right after an overrun).")
        .def("reset", &MulticastConsumer::reset);

    py::class_<MulticastQueue>(m, "MulticastQueue",
        "Broadcast queue: one producer, any number of independent consumers. Each consumer only "
        "sees messages published after it attaches; a consumer that falls behind is lapped rather "
        "than blocking the producer.")
        .def(py::init<std::string const&, std::size_t, std::string const&, bool>(), py::arg("name"),
            py::arg("capacity_hint"), py::arg("path") = std::string{}, py::arg("anonymous") = false,
            "Create the queue if it doesn't exist yet, or open it if it does (capacity must match).")
        .def_static("open", &MulticastQueue::open, py::arg("name"), py::arg("path") = std::string{},
            py::arg("anonymous") = false, "Open an existing queue only; raises if it doesn't exist.")
        .def("create_producer", &MulticastQueue::createProducer)
        .def("create_consumer", &MulticastQueue::createConsumer);
}
