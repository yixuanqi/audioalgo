"""Auxiliary module for the frozen NWS python_two_files fixture.

v1 contract: y = 0.5 * primary - 0.25 * reference, scaled by the gain parameter.
"""

import numpy as np

COEFFICIENTS = (0.5, -0.25)


def mix(primary, reference, gain):
    a, b = COEFFICIENTS
    return (np.float32(a) * primary + np.float32(b) * reference) * np.float32(gain)
