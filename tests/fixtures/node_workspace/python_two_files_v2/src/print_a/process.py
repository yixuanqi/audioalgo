"""Entry point for the frozen NWS python_two_files fixture.

The helper module next door owns the coefficients, so editing this file alone
is not enough to change the audible result.
"""

import numpy as np

from . import filter as coefficient_filter


def process(inputs, params, state):
    primary = inputs["primary"]
    reference = inputs["reference"]
    return {"main": coefficient_filter.mix(primary, reference, params["gain"])}
