"""Auxiliary module for the frozen NWS python_two_files_v2 fixture.

v2 contract: y = 0.5 * primary - 0.5 * reference, scaled by the gain parameter.
Only this auxiliary module differs from python_two_files; node.json is byte
identical so the declaration fingerprint does not change.
"""

import numpy as np

COEFFICIENTS = (0.5, -0.5)


def mix(primary, reference, gain):
    a, b = COEFFICIENTS
    return (np.float32(a) * primary + np.float32(b) * reference) * np.float32(gain)
