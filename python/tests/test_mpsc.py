import pytest

import turboq


def test_send_receive_roundtrip(unique_name):
    queue = turboq.MPSCQueue(unique_name(), slot_size_hint=256, length_hint=64, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    assert producer.send(b"hello") is True
    assert consumer.receive() == b"hello"


def test_multiple_producers_single_consumer(unique_name):
    queue = turboq.MPSCQueue(unique_name(), slot_size_hint=256, length_hint=64, anonymous=True)
    consumer = queue.create_consumer()
    producer_a = queue.create_producer()
    producer_b = queue.create_producer()

    assert producer_a.send(b"from A") is True
    assert producer_b.send(b"from B") is True

    received = {consumer.receive(), consumer.receive()}
    assert received == {b"from A", b"from B"}
    assert consumer.receive() is None


def test_slot_size_and_length_are_visible_on_both_ends(unique_name):
    queue = turboq.MPSCQueue(unique_name(), slot_size_hint=128, length_hint=32, anonymous=True)
    producer = queue.create_producer()
    consumer = queue.create_consumer()

    # length_hint is rounded up to the next power of two -- 32 already is one.
    assert producer.slot_size >= 128
    assert producer.length == 32
    assert consumer.slot_size == producer.slot_size
    assert consumer.length == producer.length


def test_send_larger_than_slot_size_raises(unique_name):
    queue = turboq.MPSCQueue(unique_name(), slot_size_hint=32, length_hint=16, anonymous=True)
    producer = queue.create_producer()

    with pytest.raises(RuntimeError):
        producer.send(b"x" * (producer.slot_size + 1))


def test_second_consumer_on_same_queue_object_is_not_blocked(unique_name):
    # MPSCMessageQueueImpl::createConsumer() locks with flock() and has no extra in-process
    # "already created" guard (unlike SPSC's createProducer()/createConsumer(), which additionally
    # track a producerCreated_/consumerCreated_ flag precisely to catch this same-fd case). flock()
    # locks are scoped to the open file description, and both calls here share the same
    # File/fd (this `queue` object), so the second lock attempt just re-asserts a lock this
    # process already holds and succeeds. Real cross-process protection is unaffected: a second
    # process opening the same named queue gets its own file descriptor / open file description.
    queue = turboq.MPSCQueue(unique_name(), slot_size_hint=256, length_hint=64, anonymous=True)
    queue.create_consumer()
    second_consumer = queue.create_consumer()

    assert bool(second_consumer) is True
