"""Opt-in fault injection: fail a real Python assertion as an independent test."""


def test_deliberate_assertion_failure() -> None:
    assert False, "intentional G0 failure-propagation probe"


if __name__ == "__main__":
    test_deliberate_assertion_failure()
