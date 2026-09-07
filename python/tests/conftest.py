import itertools

import pytest


@pytest.fixture
def unique_name():
    """Yield a fresh, process-unique queue name for each test.

    Not strictly required for anonymous=True queues (AnonymousMemorySource
    ignores the name entirely -- see turboq/MemorySource.h), but it keeps
    tests independent and makes it easy to switch a test to a real
    path-based queue later without picking a name by hand.
    """
    counter = itertools.count()

    def _make(prefix: str = "test") -> str:
        return f"{prefix}-{id(_make)}-{next(counter)}"

    return _make
