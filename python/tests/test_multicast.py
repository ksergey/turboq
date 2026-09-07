import pytest

import turboq


def test_send_receive_roundtrip(unique_name):
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    assert producer.send(b"tick") is True
    assert consumer.receive() == b"tick"


def test_broadcast_to_multiple_consumers(unique_name):
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer_a = queue.create_consumer()
    consumer_b = queue.create_consumer()

    assert producer.send(b"broadcast") is True
    assert consumer_a.receive() == b"broadcast"
    assert consumer_b.receive() == b"broadcast"


def test_consumer_only_sees_messages_sent_after_it_attaches(unique_name):
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()

    producer.send(b"before")
    late_consumer = queue.create_consumer()
    producer.send(b"after")

    assert late_consumer.receive() == b"after"
    assert late_consumer.receive() is None


def test_overrun_count_starts_at_zero(unique_name):
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    queue.create_producer()
    consumer = queue.create_consumer()

    assert consumer.overrun_count == 0


def test_overrun_is_detected_when_consumer_falls_far_behind(unique_name):
    # Small ring: easy to lap many times over with a consumer that isn't reading.
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=4096, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    payload = b"x" * 32

    # The very first fetch() a consumer ever does only baselines its expected sequence number
    # (there's nothing to compare against yet), so it can never itself report an overrun -- see
    # MulticastMessageQueue.h's fetch(). Establish that baseline first, on a message that's still
    # valid when we read it.
    assert producer.send(payload) is True
    assert consumer.receive() == payload

    # Now flood well past the ring's capacity without the consumer reading anything, so the
    # producer wraps around and overwrites the slot the consumer is still sitting on.
    for _ in range(512):
        producer.send(payload)

    assert consumer.receive() is None  # the overwritten slot is reported as empty, not garbage
    assert consumer.overrun_count > 0


def test_reset_rebaselines_the_consumer(unique_name):
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    producer.send(b"stale")
    consumer.reset()
    producer.send(b"fresh")

    assert consumer.receive() == b"fresh"


def test_second_producer_on_same_queue_object_is_not_blocked(unique_name):
    # Unlike SPSC/MPSC, MulticastMessageQueueImpl::createProducer() locks with flock() and has no
    # extra in-process "already created" guard. flock() locks are scoped to the open file
    # description, and both calls here go through the same File/fd (this same `queue` object), so
    # the second lock attempt just re-asserts a lock this process already holds and succeeds --
    # it does NOT protect against calling create_producer() twice on one MulticastQueue instance.
    # This documents that observed behavior rather than prescribing it as a good idea: real
    # cross-process protection still works, since a second process opening the same named queue
    # gets its own file descriptor / open file description.
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    queue.create_producer()
    second_producer = queue.create_producer()

    assert bool(second_producer) is True


def test_many_independent_consumers_are_allowed(unique_name):
    queue = turboq.MulticastQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumers = [queue.create_consumer() for _ in range(5)]

    producer.send(b"fan-out")

    assert all(consumer.receive() == b"fan-out" for consumer in consumers)
