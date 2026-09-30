"""Host-only policy ownership, failure-path, and exercise lifecycle tests."""
import contextlib
import io
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import vista_supermium_policy as policy
import vista_low_launch_exercise as launch


class Registry:
    def __init__(self, values=None):
        self.values = None if values is None else dict(values)
        self.commands = []
        self.fail_value = None
        self.executable = True
        self.subkey = False
        self.user_values = None

    @property
    def writes(self):
        return [c for c in self.commands if c.startswith(('reg add ', 'reg delete '))]

    def run(self, command, user=False):
        self.commands.append(command)
        if user:
            if command != f'reg query "{policy.USER_ROOT}" /s /reg:32':
                raise AssertionError(command)
            if self.user_values is None:
                return 1, 'ERROR: The system was unable to find the specified registry key or value.'
            output = policy.USER_FULL_ROOT + '\\{remembered}\n'
            for name, (kind, value) in self.user_values.items():
                output += f'    {name}    {kind}    {value}\n'
            return 0, output
        if command.startswith('if exist '):
            return (0, 'SUPERMIUM_EXECUTABLE_PRESENT') if self.executable else (1, '')
        # No machine-default, zones, COM registration, or 64-bit mutation.
        if not command.endswith('/reg:32') or f'"{policy.KEY}"' not in command:
            raise AssertionError(command)
        if command.startswith('reg query '):
            if self.values is None:
                return 1, 'ERROR: The system was unable to find the specified registry key or value.'
            output = '\r\nC:\\>reg query ignored-echo\r\n' + policy.FULL_KEY + '\r\n'
            for name, (kind, value) in self.values.items():
                output += f'    {name}    {kind}    {value}\r\n'
            if self.subkey:
                output += policy.FULL_KEY + '\\Unrelated\r\n'
            return 0, output
        if command.startswith('reg delete '):
            self.values = None
            return 0, 'The operation completed successfully.'
        match = re.search(r'/v (\S+) /t (REG_\w+) /d "([^"]*)" /f', command)
        if not match:
            raise AssertionError(command)
        name, kind, value = match.groups()
        if name == self.fail_value:
            return 5, 'ERROR: Access is denied.'
        if self.values is None:
            self.values = {}
        self.values[name] = kind, value
        return 0, 'The operation completed successfully.'


class PolicyTest(unittest.TestCase):
    def setUp(self):
        self.registry = Registry()
        self.policy = policy.Policy(self.registry.run)
        self.output = io.StringIO()
        self.capture = contextlib.redirect_stdout(self.output)
        self.capture.__enter__()
        self.addCleanup(self.capture.__exit__, None, None, None)

    def test_install_readback_and_idempotency(self):
        self.policy.install()
        self.assertEqual(self.registry.values, policy.expected(policy.DURABLE))
        self.assertIn('/v Policy /t REG_DWORD /d "0x1"', self.registry.writes[-1])
        first = list(self.registry.writes)
        self.policy.install()
        self.assertEqual(self.registry.writes, first)
        self.assertEqual(self.policy.status(), 0)

    def test_user_medium_policy_blocks_install_status_and_exercise(self):
        self.registry.user_values = {**policy.BASE, 'Policy': ('REG_DWORD', '0x3')}
        for operation in (self.policy.install, self.policy.status):
            with self.assertRaisesRegex(RuntimeError, 'conflicting per-user'):
                operation()
        with self.assertRaisesRegex(RuntimeError, 'conflicting per-user'), policy.exercise_policy(self.policy):
            self.fail('must not launch with a conflict')
        self.assertFalse(self.registry.writes)

    def test_matching_low_user_policy_or_other_executable_is_preserved(self):
        for values in (dict(policy.BASE), {**policy.BASE, 'AppName': ('REG_SZ', 'other.exe')},
                       {**policy.BASE, 'AppPath': ('REG_SZ', r'C:\Other')}):
            self.registry.user_values = values
            self.policy.install()
            self.assertEqual(self.registry.user_values, values)

    def test_workflow_lock_excludes_other_operation_and_releases(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(policy.vista_control, 'DEFAULT_SOCKET', Path(directory) / 'control.sock'):
            with policy.policy_lock():
                with self.assertRaisesRegex(RuntimeError, 'another Supermium policy operation'), policy.policy_lock():
                    self.fail('a second workflow must not start')
            with policy.policy_lock():
                pass

    def test_uninstall_idempotency(self):
        self.policy.install()
        self.policy.remove(policy.DURABLE)
        first = list(self.registry.writes)
        self.policy.remove(policy.DURABLE)
        self.assertEqual(first, self.registry.writes)
        self.assertEqual(self.policy.status(), 1)

    def test_refuses_foreign_and_drifted_entries(self):
        for values in (dict(policy.BASE), {}, policy.expected(policy.TEMPORARY),
                       {**policy.expected(policy.DURABLE), 'Policy': ('REG_DWORD', '0x3')},
                       {**policy.expected(policy.DURABLE), 'Extra': ('REG_SZ', 'foreign')},
                       {**policy.expected(policy.DURABLE), 'AppPath': ('REG_SZ', r'C:\Other')}):
            with self.subTest(values=values):
                self.registry.values = dict(values)
                self.registry.commands.clear()
                for operation in (self.policy.install, self.policy.status,
                                  lambda: self.policy.remove(policy.DURABLE)):
                    with self.assertRaises(RuntimeError):
                        operation()
                self.assertEqual(self.registry.values, values)
                self.assertFalse(self.registry.writes)

    def test_missing_executable_changes_nothing(self):
        self.registry.executable = False
        with self.assertRaises(RuntimeError):
            self.policy.install()
        self.assertIsNone(self.registry.values)
        self.assertFalse(self.registry.writes)

    def test_partial_write_failure_rolls_back(self):
        for name in (policy.OWNER, 'AppName', 'AppPath', 'Policy'):
            with self.subTest(value=name):
                self.registry.fail_value = name
                with self.assertRaises(RuntimeError):
                    self.policy.install()
                self.assertIsNone(self.registry.values)

    def test_interrupted_install_rolls_back_owned_partial_key(self):
        def run(command, **kwargs):
            result = self.registry.run(command, **kwargs)
            if '/v AppPath ' in command:
                raise KeyboardInterrupt
            return result
        with self.assertRaises(KeyboardInterrupt):
            policy.Policy(run).install()
        self.assertIsNone(self.registry.values)

    def test_drift_during_install_is_preserved(self):
        def run(command, **kwargs):
            result = self.registry.run(command, **kwargs)
            if '/v AppPath ' in command:
                self.registry.values['Extra'] = 'REG_SZ', 'foreign'
                raise RuntimeError('injected failure')
            return result
        with self.assertRaisesRegex(RuntimeError, 'preserved'):
            policy.Policy(run).install()
        self.assertEqual(self.registry.values['Extra'], ('REG_SZ', 'foreign'))
        self.assertFalse(any(c.startswith('reg delete') for c in self.registry.writes))

    def test_query_fails_closed(self):
        for code, text in [(1, 'ERROR: Access is denied.'), (0, ''),
                           (0, policy.FULL_KEY + '\n    unreadable')]:
            with self.subTest(code=code, text=text), self.assertRaises(RuntimeError):
                policy.parse_query(code, text)

    def test_subkey_prevents_mutation(self):
        self.registry.values = policy.expected(policy.DURABLE)
        self.registry.subkey = True
        with self.assertRaises(RuntimeError):
            self.policy.remove(policy.DURABLE)
        self.assertFalse(self.registry.writes)

    def test_duplicate_values_rejected(self):
        text = policy.FULL_KEY + '\n    Policy    REG_DWORD    0x1\n    Policy    REG_DWORD    0x3'
        with self.assertRaises(RuntimeError):
            policy.parse_query(0, text)

    def test_temporary_policy_cleaned_on_success_failure_and_interrupt(self):
        for error in (None, RuntimeError, KeyboardInterrupt):
            with self.subTest(error=error):
                try:
                    with policy.exercise_policy(self.policy) as durable:
                        self.assertFalse(durable)
                        self.assertEqual(self.registry.values, policy.expected(policy.TEMPORARY))
                        if error:
                            raise error('injected')
                except (RuntimeError, KeyboardInterrupt):
                    if not error:
                        raise
                self.assertIsNone(self.registry.values)

    def test_durable_policy_preserved_on_success_failure_and_interrupt(self):
        self.policy.install()
        first = list(self.registry.writes)
        for error in (None, RuntimeError, KeyboardInterrupt):
            with self.subTest(error=error):
                try:
                    with policy.exercise_policy(self.policy) as durable:
                        self.assertTrue(durable)
                        if error:
                            raise error('injected')
                except (RuntimeError, KeyboardInterrupt):
                    if not error:
                        raise
                self.assertEqual(self.registry.values, policy.expected(policy.DURABLE))
                self.assertEqual(first, self.registry.writes)

    def test_exercise_rejects_unowned_entry(self):
        self.registry.values = dict(policy.BASE)
        with self.assertRaises(RuntimeError), policy.exercise_policy(self.policy):
            self.fail('must not yield')
        self.assertFalse(self.registry.writes)

    def test_exercise_detects_drift_without_clobbering_it(self):
        self.policy.install()
        with self.assertRaises(RuntimeError), policy.exercise_policy(self.policy):
            self.registry.values['Extra'] = 'REG_SZ', 'keep'
        self.assertEqual(self.registry.values['Extra'], ('REG_SZ', 'keep'))

    def test_actual_harness_borrows_durable_policy_and_checks_zones(self):
        self.policy.install()
        with mock.patch.object(launch, 'exercise_policy', lambda: policy.exercise_policy(self.policy)), \
                mock.patch.object(launch, 'require', return_value='zones') as commands:
            launch.exercise('web')
        self.assertEqual(commands.call_count, 3)
        self.assertEqual(self.registry.values, policy.expected(policy.DURABLE))
        self.assertIn('LOW_POLICY_PRESERVED_AND_ZONES_UNCHANGED', self.output.getvalue())

    def test_actual_harness_failure_preserves_policy_without_success_marker(self):
        self.policy.install()
        with mock.patch.object(launch, 'exercise_policy', lambda: policy.exercise_policy(self.policy)), \
                mock.patch.object(launch, 'require', side_effect=['zones', RuntimeError('IE failed'), 'zones']), \
                self.assertRaisesRegex(RuntimeError, 'IE failed'):
            launch.exercise('web')
        self.assertEqual(self.registry.values, policy.expected(policy.DURABLE))
        self.assertNotIn('AND_ZONES_UNCHANGED', self.output.getvalue())

    def test_actual_harness_detects_zone_changes(self):
        with mock.patch.object(launch, 'exercise_policy', lambda: policy.exercise_policy(self.policy)), \
                mock.patch.object(launch, 'require', side_effect=['before', 'result', 'after']), \
                self.assertRaisesRegex(RuntimeError, 'zone settings changed'):
            launch.exercise('web')
        self.assertIsNone(self.registry.values)
        self.assertNotIn('AND_ZONES_UNCHANGED', self.output.getvalue())


if __name__ == '__main__':
    unittest.main()
