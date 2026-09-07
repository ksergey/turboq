import pytest

import turboq


def test_send_receive_roundtrip(unique_name):
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    assert producer.send(b"hello") is True
    assert consumer.receive() == b"hello"


def test_receive_on_empty_returns_none(unique_name):
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    consumer = queue.create_consumer()

    assert consumer.receive() is None


def test_preserves_message_order_and_boundaries(unique_name):
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    # Note: a genuinely empty (zero-length) payload is deliberately not covered here -- see
    # "Zero-length messages" in python/README.md, it's indistinguishable from "queue empty" by
    # design all the way down in turboq's fetch().
    messages = [b"first", b"second", b"third message, a bit longer this time"]
    for message in messages:
        assert producer.send(message) is True

    received = [consumer.receive() for _ in messages]
    assert received == messages
    assert consumer.receive() is None


def test_zero_length_payload_is_indistinguishable_from_empty_queue(unique_name):
    # Documents a real limitation (see python/README.md "Zero-length messages"): turboq's fetch()
    # uses an empty span as its sole "nothing to read" sentinel, all the way from the C++ layer up
    # through this binding's receive(). A zero-byte message sent successfully still comes back as
    # None, exactly like an empty queue -- there is no way to tell the two apart.
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    assert producer.send(b"") is True
    assert consumer.receive() is None


def test_send_returns_false_when_full(unique_name):
    # Small enough that a handful of sends definitely exhaust it, with nothing consuming.
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=256, anonymous=True)
    producer = queue.create_producer()

    payload = b"x" * 64
    results = [producer.send(payload) for _ in range(64)]

    assert False in results, "expected the queue to fill up and reject at least one send()"


def test_capacity_matches_hint(unique_name):
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    assert producer.capacity == 1 << 16
    assert consumer.capacity == 1 << 16


def test_reset_drops_unconsumed_backlog(unique_name):
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    producer.send(b"stale message")
    consumer.reset()

    assert consumer.receive() is None


def test_second_producer_raises(unique_name):
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    queue.create_producer()

    with pytest.raises(RuntimeError):
        queue.create_producer()


def test_second_consumer_raises(unique_name):
    queue = turboq.SPSCQueue(unique_name(), capacity_hint=1 << 16, anonymous=True)
    queue.create_consumer()

    with pytest.raises(RuntimeError):
        queue.create_consumer()


def test_open_nonexistent_queue_raises(unique_name):
    with pytest.raises(RuntimeError):
        turboq.SPSCQueue.open(unique_name(), anonymous=True)
