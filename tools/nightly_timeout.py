"""Execution watchdog policy shared by tools used in both CI and nightlies.

Only ROCKETNET_NIGHTLY_NO_TIMEOUT=1 disables an outer execution timeout.
Scenario durations, protocol deadlines, cleanup waits and result oracles
remain the caller's responsibility.
"""
import os


def no_execution_timeout():
    return os.environ.get("ROCKETNET_NIGHTLY_NO_TIMEOUT") == "1"


def execution_timeout(seconds):
    """Return the usual subprocess timeout, or None for the nightly opt-in."""
    return None if no_execution_timeout() else seconds
