import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('control', Path(__file__).resolve().parents[1] / 'tools/vista_control.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class UiaClientTest(unittest.TestCase):
    def test_value_is_encoded_and_worker_is_desktop_bounded(self):
        value = '" & echo %PATH% | héllo'
        with patch.object(control, 'Client') as client:
            instance = client.return_value.__enter__.return_value
            instance.checked.return_value = {'status': 'DONE', 'code': 0}
            self.assertEqual(control.main(['uia', 'set-value', '--hwnd', '1234',
                                          '--expect-name', 'Field', '--value', value]), 0)
            operation, command, timeout, _ = instance.checked.call_args.args
            self.assertEqual(operation, 'USER')
            self.assertEqual(timeout, 20000)
            self.assertIn(value.encode().hex().encode(), command)
            self.assertNotIn(b'%PATH%', command)

    def test_actions_require_root_and_name(self):
        for args in [['uia', 'invoke'], ['uia', 'focus', '--hwnd', '1234'],
                     ['uia', 'tree', '--path', '0&whoami'],
                     ['uia', 'tree', '--hwnd', '1&whoami']]:
            with self.assertRaises(SystemExit) as error:
                control.main(args)
            self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
