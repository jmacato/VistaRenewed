#!/usr/bin/env python3
"""Exercise IE using a durable or temporary low-integrity Supermium policy."""
import argparse
import signal
from vista_supermium_policy import exercise_policy, interrupt, policy_lock, require, run

ZONES = r'reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings\Zones" /s'


def exercise(mode):
    zones = require(ZONES, user=True)
    durable = False
    try:
        with exercise_policy() as durable:
            require(f'C:\\TritonSupermiumBridge\\run_iexplore_mshtml_activation_exercise.bat {mode} extended',
                    user=True, timeout=60)
    finally:
        if zones != require(ZONES, user=True):
            raise RuntimeError('zone settings changed during exercise')
    print('LOW_POLICY_PRESERVED_AND_ZONES_UNCHANGED' if durable else
          'LOW_POLICY_REMOVED_AND_ZONES_UNCHANGED')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['http', 'web', 'input'], default='http')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupt)
    with policy_lock():
        exercise(args.mode)


if __name__ == '__main__':
    main()
