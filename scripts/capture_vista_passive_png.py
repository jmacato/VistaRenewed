#!/usr/bin/env python3
"""Capture one passive Vista PNG after the required guest evidence exists."""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import socket
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


PROBE_PASS = "TRITON9-PROBE PASS"
PROBE_FAIL = "TRITON9-PROBE FAIL"
GLASS_SCENE = "TRITON9-AERO API-PATH proof-pattern"
POST_REBOOT = "POST_REBOOT_COMMIT id="
REPROBE_REBOOT = "REPROBE_REBOOT id="
REPROBE_LAUNCHED = "REPROBE_LAUNCHED id="
PROBE_RUN_ARMED = "PROBE_RUN_ARMED id="
PROBE_RUN = "TRITON9-RUN nonce="
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def marker_offsets(text: str, marker: str) -> list[int]:
    offsets: list[int] = []
    start = 0
    while True:
        offset = text.find(marker, start)
        if offset < 0:
            return offsets
        offsets.append(offset)
        start = offset + len(marker)


def validate_evidence(
    text: str, deployment_id: str, minimum_probe_passes: int = 1
) -> None:
    if minimum_probe_passes not in (1, 2):
        raise ValueError("minimum probe passes must be 1 or 2")

    all_commits = marker_offsets(text, POST_REBOOT)
    commits = marker_offsets(text, POST_REBOOT + deployment_id)
    if len(commits) < minimum_probe_passes:
        raise ValueError(
            "the current deployment does not have enough post-reboot byte proofs"
        )

    selected_commits = commits[-minimum_probe_passes:]
    if all_commits[-minimum_probe_passes:] != selected_commits:
        raise ValueError("a newer post-reboot proof belongs to another deployment")
    selected_passes: list[int] = []
    selected_scenes: list[int] = []
    for index, commit in enumerate(selected_commits):
        end = (
            selected_commits[index + 1]
            if index + 1 < len(selected_commits)
            else len(text)
        )
        passed = text.rfind(PROBE_PASS, commit, end)
        failed = text.rfind(PROBE_FAIL, commit, end)
        scene = text.rfind(GLASS_SCENE, commit, end)
        if passed < commit:
            raise ValueError(
                f"public D3D9 probe run {index + 1} has not passed"
            )
        if failed > passed:
            raise ValueError(
                f"public D3D9 probe run {index + 1} ended in failure"
            )
        if scene < passed:
            raise ValueError(
                f"controlled glass scene {index + 1} is not ready"
            )
        armed_expression = re.compile(
            re.escape(PROBE_RUN_ARMED + deployment_id + " nonce=")
            + r"([0-9a-f]{64})"
        )
        bound_run = False
        armed_matches = list(armed_expression.finditer(text, commit, passed))
        for armed in reversed(armed_matches):
            nonce = armed.group(1)
            if text.find(PROBE_RUN + nonce, armed.end(), passed) >= 0:
                bound_run = True
                break
        if not bound_run:
            raise ValueError(
                f"public D3D9 probe run {index + 1} has no matching launch nonce"
            )
        selected_passes.append(passed)
        selected_scenes.append(scene)

    if minimum_probe_passes == 2:
        reboot = text.find(
            REPROBE_REBOOT + deployment_id,
            selected_commits[0],
            selected_commits[1],
        )
        relaunched = text.find(
            REPROBE_LAUNCHED + deployment_id,
            selected_commits[1],
        )
        if reboot < 0:
            raise ValueError("the guest service did not request the proof reboot")
        if relaunched < 0:
            raise ValueError("the guest service did not launch the second proof")
        if not (
            selected_commits[0]
            < selected_passes[0]
            < selected_scenes[0]
            < reboot
            < selected_commits[1]
            < relaunched
            < selected_passes[1]
            < selected_scenes[1]
        ):
            raise ValueError("the two proof runs are not in guest-reboot order")


def receive_message(stream: Any) -> dict[str, Any]:
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP closed the connection")
        message = json.loads(line)
        if "event" not in message:
            return message


def execute(stream: Any, command: str, arguments: dict[str, Any] | None = None) -> None:
    request: dict[str, Any] = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request, separators=(",", ":")).encode("ascii") + b"\n")
    stream.flush()
    response = receive_message(stream)
    if "error" in response:
        raise RuntimeError(f"QMP {command} failed: {response['error']}")
    if "return" not in response:
        raise RuntimeError(f"QMP {command} returned an invalid response")


def capture(qmp_socket: Path, output: Path) -> None:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(15)
        connection.connect(str(qmp_socket))
        with connection.makefile("rwb", buffering=0) as stream:
            greeting = receive_message(stream)
            if "QMP" not in greeting:
                raise RuntimeError("the socket did not return a QMP greeting")
            execute(stream, "qmp_capabilities")
            execute(
                stream,
                "screendump",
                {"filename": str(output), "format": "png"},
            )


def validate_run_directory(
    run_directory: Path,
    status_log: Path,
    qmp_socket: Path,
    deployment_id: str,
    check_process: bool = True,
) -> str:
    if run_directory.is_symlink() or not run_directory.is_dir():
        raise ValueError("deployment run directory is missing or is a symbolic link")
    run_stat = run_directory.stat()
    if run_stat.st_uid != os.getuid() or stat.S_IMODE(run_stat.st_mode) != 0o700:
        raise ValueError("deployment run directory must be owned by this user with mode 0700")
    expected_status = run_directory / "status.log"
    expected_socket = run_directory / "qmp.sock"
    if status_log.absolute() != expected_status.absolute():
        raise ValueError("status log is not bound to the deployment run directory")
    if qmp_socket.absolute() != expected_socket.absolute():
        raise ValueError("QMP socket is not bound to the deployment run directory")
    if status_log.is_symlink() or not status_log.is_file():
        raise ValueError("status log is missing or is a symbolic link")
    if qmp_socket.is_symlink() or not qmp_socket.is_socket():
        raise ValueError("QMP socket is missing or is a symbolic link")
    token_path = run_directory / "run-token"
    identity_path = run_directory / "deployment-id"
    pid_path = run_directory / "qemu-pid"
    executable_path = run_directory / "qemu-path"
    metadata_paths = (token_path, identity_path, pid_path, executable_path)
    if any(item.is_symlink() or not item.is_file() for item in metadata_paths):
        raise ValueError("deployment run metadata cannot use symbolic links")
    token = token_path.read_text(encoding="ascii").strip()
    run_deployment_id = identity_path.read_text(encoding="ascii").strip()
    if len(token) != 64 or any(character not in "0123456789abcdef" for character in token):
        raise ValueError("deployment run token is invalid")
    if run_deployment_id.lower() != deployment_id.lower():
        raise ValueError("deployment run identity does not match the requested package")
    if check_process:
        pid_text = pid_path.read_text(encoding="ascii").strip()
        if not pid_text.isascii() or not pid_text.isdecimal():
            raise ValueError("deployment QEMU process ID is invalid")
        pid = int(pid_text, 10)
        if pid <= 1:
            raise ValueError("deployment QEMU process ID is invalid")
        qemu_text = executable_path.read_text(encoding="utf-8").strip()
        qemu_executable = Path(qemu_text)
        if (
            not qemu_executable.is_absolute()
            or qemu_executable.is_symlink()
            or not qemu_executable.is_file()
            or not os.access(qemu_executable, os.X_OK)
            or str(qemu_executable.resolve(strict=True)) != qemu_text
        ):
            raise ValueError("deployment QEMU executable identity is invalid")
        try:
            os.kill(pid, 0)
        except OSError as error:
            raise ValueError("deployment QEMU process is not running") from error

        process = subprocess.run(
            ["/bin/ps", "-ww", "-p", str(pid), "-o", "uid=", "-o", "command="],
            check=False,
            capture_output=True,
            text=True,
        )
        process_line = process.stdout.strip()
        process_fields = process_line.split(None, 1)
        expected_name = "vista-aero-" + token[:16]
        if (
            process.returncode != 0
            or len(process_fields) != 2
            or process_fields[0] != str(os.getuid())
            or qemu_text not in process_fields[1]
            or expected_name not in process_fields[1]
            or f"unix:{qmp_socket.absolute()},server=on,wait=off" not in process_fields[1]
            or f"file:{status_log.absolute()}" not in process_fields[1]
        ):
            raise ValueError("deployment QEMU command does not match this run")

        if sys.platform.startswith("linux"):
            proc = Path("/proc") / str(pid)
            if not os.path.samefile(proc / "exe", qemu_executable):
                raise ValueError("deployment QEMU executable does not match")
            descriptors = list((proc / "fd").iterdir())
            targets = set()
            log_owned = False
            for descriptor in descriptors:
                try:
                    targets.add(os.readlink(descriptor))
                    if os.path.samefile(descriptor, status_log):
                        log_owned = True
                except FileNotFoundError:
                    continue
            socket_inodes = set()
            for line in (proc / "net/unix").read_text().splitlines()[1:]:
                fields = line.split(None, 7)
                if len(fields) == 8 and fields[7] == str(qmp_socket.absolute()):
                    socket_inodes.add("socket:[" + fields[6] + "]")
            if not log_owned or not (socket_inodes & targets):
                raise ValueError("deployment QEMU process does not own the run endpoints")
        else:
            lsof_checks = (
                ["/usr/sbin/lsof", "-a", "-p", str(pid), "-d", "txt", "-Fn"],
                ["/usr/sbin/lsof", "-a", "-p", str(pid), "--", str(qmp_socket.absolute())],
                ["/usr/sbin/lsof", "-a", "-p", str(pid), "--", str(status_log.absolute())],
            )
            lsof_results = [
                subprocess.run(command, check=False, capture_output=True, text=True)
                for command in lsof_checks
            ]
            executable_names = {
                line[1:]
                for line in lsof_results[0].stdout.splitlines()
                if line.startswith("n")
            }
            if (
                lsof_results[0].returncode != 0
                or qemu_text not in executable_names
                or any(result.returncode != 0 for result in lsof_results[1:])
            ):
                raise ValueError("deployment QEMU process does not own the run endpoints")
    return token


def self_test() -> int:
    deployment_id = "a" * 64
    commit = POST_REBOOT + deployment_id
    reboot = REPROBE_REBOOT + deployment_id
    relaunched = REPROBE_LAUNCHED + deployment_id
    first_nonce = "b" * 64
    second_nonce = "d" * 64
    first_armed = PROBE_RUN_ARMED + deployment_id + " nonce=" + first_nonce
    second_armed = PROBE_RUN_ARMED + deployment_id + " nonce=" + second_nonce
    first_run = PROBE_RUN + first_nonce
    second_run = PROBE_RUN + second_nonce
    valid_one = "\n".join(
        (commit, first_armed, first_run, PROBE_PASS, GLASS_SCENE)
    )
    valid_two = "\n".join(
        (
            commit,
            first_armed,
            first_run,
            PROBE_PASS,
            GLASS_SCENE,
            reboot,
            commit,
            relaunched,
            second_armed,
            second_run,
            PROBE_PASS,
            GLASS_SCENE,
        )
    )
    validate_evidence(valid_one, deployment_id, 1)
    validate_evidence(valid_two, deployment_id, 2)
    rejected = (
        (valid_one, 2),
        (valid_one.replace(PROBE_PASS, PROBE_PASS + "\n" + PROBE_FAIL), 1),
        (valid_one.replace(first_run, PROBE_RUN + ("e" * 64)), 1),
        (valid_two.replace(reboot, "missing-reboot"), 2),
        (valid_two.replace(relaunched, "missing-relaunch"), 2),
        (
            valid_two.replace(PROBE_PASS, PROBE_PASS + "\n" + PROBE_FAIL, 1),
            2,
        ),
        (
            "\n".join(
                (
                    valid_one,
                    POST_REBOOT + ("c" * 64),
                    PROBE_PASS,
                    GLASS_SCENE,
                )
            ),
            1,
        ),
        (
            "\n".join(
                (
                    commit,
                    reboot,
                    first_armed,
                    first_run,
                    PROBE_PASS,
                    GLASS_SCENE,
                    commit,
                    relaunched,
                    second_armed,
                    second_run,
                    PROBE_PASS,
                    GLASS_SCENE,
                )
            ),
            2,
        ),
        (
            "\n".join(
                (
                    commit,
                    first_armed,
                    first_run,
                    PROBE_PASS,
                    GLASS_SCENE,
                    reboot,
                    commit,
                    second_armed,
                    second_run,
                    PROBE_PASS,
                    GLASS_SCENE,
                    relaunched,
                )
            ),
            2,
        ),
    )
    for evidence, minimum in rejected:
        try:
            validate_evidence(evidence, deployment_id, minimum)
        except ValueError:
            continue
        print("passive evidence self-test accepted an invalid log", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as temporary:
        run_directory = Path(temporary) / "run"
        run_directory.mkdir(mode=0o700)
        status_log = run_directory / "status.log"
        status_log.write_text(valid_two, encoding="utf-8")
        (run_directory / "run-token").write_text("b" * 64, encoding="ascii")
        (run_directory / "deployment-id").write_text(
            deployment_id, encoding="ascii"
        )
        (run_directory / "qemu-pid").write_text(str(os.getpid()), encoding="ascii")
        (run_directory / "qemu-path").write_text(
            str(Path(sys.executable).resolve()), encoding="utf-8"
        )
        qmp_socket = run_directory / "qmp.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(qmp_socket))
            validate_run_directory(
                run_directory,
                status_log,
                qmp_socket,
                deployment_id,
                check_process=False,
            )
            try:
                validate_run_directory(
                    run_directory,
                    Path(temporary) / "wrong.log",
                    qmp_socket,
                    deployment_id,
                    check_process=False,
                )
            except ValueError:
                pass
            else:
                print("run binding accepted an external log", file=sys.stderr)
                return 1
            os.chmod(run_directory, 0o755)
            try:
                validate_run_directory(
                    run_directory,
                    status_log,
                    qmp_socket,
                    deployment_id,
                    check_process=False,
                )
            except ValueError:
                pass
            else:
                print("run binding accepted an unsafe directory", file=sys.stderr)
                return 1
    print(
        "self-test passed: ordered evidence, run binding, and eleven negative controls"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run-directory", type=Path)
    parser.add_argument("--status-log", type=Path)
    parser.add_argument("--deployment-id")
    parser.add_argument("--qmp-socket", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--minimum-probe-passes",
        type=int,
        choices=(1, 2),
        default=1,
        help="require one proof run, or two runs separated by a guest reboot",
    )
    arguments = parser.parse_args()

    if arguments.self_test:
        return self_test()
    if arguments.run_directory is None:
        parser.error("--run-directory is required")
    if arguments.status_log is None:
        parser.error("--status-log is required")
    if arguments.deployment_id is None:
        parser.error("--deployment-id is required")
    if arguments.qmp_socket is None:
        parser.error("--qmp-socket is required")
    if arguments.output is None:
        parser.error("--output is required")

    if len(arguments.deployment_id) != 64 or any(
        character not in "0123456789abcdefABCDEF"
        for character in arguments.deployment_id
    ):
        parser.error("deployment ID must contain exactly 64 hexadecimal characters")
    if arguments.output.suffix.lower() != ".png":
        parser.error("output must use the .png suffix")
    if arguments.output.exists() or arguments.output.is_symlink():
        parser.error("output already exists; refusing to overwrite it")
    if not arguments.output.parent.is_dir():
        parser.error("output parent directory does not exist")
    temporary_output: Path | None = None
    published = False
    try:
        run_token = validate_run_directory(
            arguments.run_directory,
            arguments.status_log,
            arguments.qmp_socket,
            arguments.deployment_id,
        )
        evidence = arguments.status_log.read_text(encoding="utf-8", errors="replace")
        validate_evidence(
            evidence,
            arguments.deployment_id.lower(),
            arguments.minimum_probe_passes,
        )
        temporary_output = arguments.run_directory / (
            f".capture-{run_token[:16]}-{secrets.token_hex(8)}.png"
        )
        if temporary_output.exists() or temporary_output.is_symlink():
            raise RuntimeError("private capture path already exists")
        capture(arguments.qmp_socket, temporary_output.absolute())
        descriptor = os.open(
            temporary_output,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
        )
        try:
            signature = os.read(descriptor, len(PNG_SIGNATURE))
        finally:
            os.close(descriptor)
        if signature != PNG_SIGNATURE:
            raise RuntimeError("QMP did not create a PNG")
        os.link(
            temporary_output,
            arguments.output,
            src_dir_fd=None,
            dst_dir_fd=None,
            follow_symlinks=False,
        )
        published = True
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        if published:
            try:
                arguments.output.unlink()
            except FileNotFoundError:
                pass
            except OSError as cleanup_error:
                print(
                    f"passive PNG cleanup failed: {cleanup_error}", file=sys.stderr
                )
        if temporary_output is not None:
            try:
                temporary_output.unlink()
            except FileNotFoundError:
                pass
            except OSError as cleanup_error:
                print(
                    f"private PNG cleanup failed: {cleanup_error}",
                    file=sys.stderr,
                )
        print(f"passive PNG capture failed: {error}", file=sys.stderr)
        return 1
    if temporary_output is not None:
        try:
            temporary_output.unlink()
        except OSError as error:
            try:
                arguments.output.unlink()
            except OSError:
                pass
            print(f"private PNG cleanup failed: {error}", file=sys.stderr)
            return 1
    print(arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
