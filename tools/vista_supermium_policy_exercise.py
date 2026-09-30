#!/usr/bin/env python3
"""VM integration test: round-trip the owned policy, leaving it installed."""
from vista_supermium_policy import (DURABLE, KEY, Policy, exercise_policy,
                                    expected, policy_lock, require, run)
from vista_low_launch_exercise import ZONES
from vista_mshtml_script_exercise import STOCK


def main():
    with policy_lock():
        policy = Policy()
        policy.validate_user_policy()
        original = policy.read()
        if original is not None and original != expected(DURABLE):
            raise RuntimeError('existing foreign policy; integration test refuses to modify it')
        zones = require(ZONES, user=True)
        activation = require(STOCK, user=True)
        other_view = run(f'reg query "{KEY}" /s /reg:64')
        try:
            policy.install()
            policy.install()
            with exercise_policy(policy) as durable:
                if not durable:
                    raise RuntimeError('installed policy was not reused')
            policy.remove(DURABLE)
            policy.remove(DURABLE)
            if policy.status() != 1:
                raise RuntimeError('uninstall did not produce absent status')
            with exercise_policy(policy) as durable:
                if durable:
                    raise RuntimeError('temporary policy path was not exercised')
            if policy.read() is not None:
                raise RuntimeError('temporary policy leaked')
        finally:
            # Durable installation is the requested final state, even if an
            # assertion fails. install refuses to overwrite any drifted entry.
            policy.install()
            if zones != require(ZONES, user=True):
                raise RuntimeError('security Zones changed')
            if activation != require(STOCK, user=True):
                raise RuntimeError('HTMLDocument activation changed')
            if other_view != run(f'reg query "{KEY}" /s /reg:64'):
                raise RuntimeError('64-bit registry view changed')
        if policy.status():
            raise RuntimeError('durable final state missing')
    print('DURABLE SUPERMIUM POLICY ROUNDTRIP PASSED')


if __name__ == '__main__':
    main()
