"""Author test for the frozen NWS fixture.

It lives under tests/ and therefore never enters the runtime content manifest.
It is not executed by the AudioAlgo acceptance suite; the independent float64
oracle lives outside the workspace tree.
"""

import numpy as np

from print_a import filter as coefficient_filter


def test_mix_matches_the_v1_formula():
    primary = np.array([[1.0, -2.0]], dtype=np.float32)
    reference = np.array([[4.0, 8.0]], dtype=np.float32)
    actual = coefficient_filter.mix(primary, reference, 1.0)
    expected = np.array([[0.5 - 1.0, -1.0 - 2.0]], dtype=np.float32)
    assert np.max(np.abs(actual - expected)) <= 1e-6
