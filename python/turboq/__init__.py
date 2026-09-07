"""Python bindings for `ksergey/turboq <https://github.com/ksergey/turboq>`_ --
low-latency shared-memory message queues (SPSC, MPSC, Multicast).

Quick start (single process, for a demo / tests -- use ``anonymous=True``)::

    import turboq

    queue = turboq.SPSCQueue("demo", capacity_hint=1 << 20, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    producer.send(b"hello")
    print(consumer.receive())  # b"hello"

Across separate processes, drop ``anonymous`` and give both sides the same
``name`` (and, if you don't want the OS default of ``/dev/shm``/``/tmp``, the
same ``path``) -- the producer process creates the queue, the consumer opens it::

    # producer.py
    queue = turboq.SPSCQueue("orders", capacity_hint=1 << 20, path="/dev/shm/myapp")
    producer = queue.create_producer()

    # consumer.py
    queue = turboq.SPSCQueue.open("orders", path="/dev/shm/myapp")
    consumer = queue.create_consumer()

See the class docstrings for the differences between SPSCQueue, MPSCQueue and
MulticastQueue.
"""

from ._turboq import (
    MPSCConsumer,
    MPSCProducer,
    MPSCQueue,
    MulticastConsumer,
    MulticastProducer,
    MulticastQueue,
    SPSCConsumer,
    SPSCProducer,
    SPSCQueue,
)

__all__ = [
    "SPSCQueue",
    "SPSCProducer",
    "SPSCConsumer",
    "MPSCQueue",
    "MPSCProducer",
    "MPSCConsumer",
    "MulticastQueue",
    "MulticastProducer",
    "MulticastConsumer",
]
