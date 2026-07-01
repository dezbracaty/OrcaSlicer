#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


def write_json(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def worker_config():
    return {
        "extruder_type": ["Direct Drive"],
        "filament_retract_lift_enforce": ["nil"],
        "gcode_comments": True,
        "layer_change_gcode": "G92 E0",
    }


def worker_config_with_oversized_flush_matrix():
    config = worker_config()
    config.update({
        "worker_test_unknown_key": True,
        "filament_colour": ["#26A69A", "#26A69A", "#26A69A", "#26A69A"],
        "filament_diameter": [1.75, 1.75, 1.75, 1.75],
        "filament_settings_id": ["Test PLA", "Test PLA", "Test PLA", "Test PLA"],
        "filament_type": ["PLA", "PLA", "PLA", "PLA"],
        "flush_multiplier": [0.3],
        "flush_volumes_vector": [140, 140, 140, 140, 140, 140, 140, 140],
        "flush_volumes_matrix": [
            0 if row == col else 280
            for row in range(8)
            for col in range(8)
        ],
    })
    return config


def test_3mf_path(source_root):
    matches = sorted((source_root / "tests" / "data" / "test_3mf").rglob("*.3mf"))
    if not matches:
        raise AssertionError("No 3MF fixture found")
    return matches[0]


def orca_project_3mf_path(source_root):
    path = source_root / "resources" / "calib" / "pressure_advance" / "pa_pattern.3mf"
    if not path.exists():
        raise AssertionError(f"Orca project 3MF fixture not found: {path}")
    return path


def worker_request(source_root, work_dir, job_id, output_name, input_type="stl", input_path=None, config_type="resolved_orca_json"):
    if input_path is None:
        input_path = source_root / "tests" / "data" / "test_3mf" / "Prusa.stl"
    config = {
        "type": config_type,
    }
    if config_type == "resolved_orca_json":
        config["path"] = str(work_dir / "config.json")

    return {
        "version": 1,
        "job_id": job_id,
        "kind": "slice",
        "working_dir": str(work_dir),
        "resources_dir": str(source_root / "resources"),
        "data_dir": str(work_dir / "data"),
        "input": {
            "type": input_type,
            "path": str(input_path),
        },
        "config": config,
        "output": {
            "gcode": str(work_dir / output_name),
            "artifacts_dir": str(work_dir / "artifacts"),
        },
        "options": {
            "overwrite": True,
        },
    }


def assert_gcode(path):
    if not path.exists():
        raise AssertionError(f"G-code file was not created: {path}")
    content = path.read_text(encoding="utf-8", errors="replace")
    if "G1" not in content:
        raise AssertionError("G-code output does not contain movement commands")
    if "; filament used" not in content:
        raise AssertionError("G-code output does not contain filament summary")


def assert_intermediates_cleaned(work_dir):
    data_dir = work_dir / "data"
    artifacts_dir = work_dir / "artifacts"
    if data_dir.exists():
        raise AssertionError(f"worker data_dir was not cleaned: {data_dir}")
    if artifacts_dir.exists():
        raise AssertionError(f"empty worker artifacts_dir was not cleaned: {artifacts_dir}")


def assert_overwrite_false_rejected(worker, source_root, work_dir):
    request = worker_request(source_root, work_dir, "worker-cli-no-overwrite", "cli-output.gcode")
    request["options"]["overwrite"] = False
    write_json(work_dir / "request-no-overwrite.json", request)

    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-no-overwrite.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    if proc.returncode == 0:
        raise AssertionError("worker overwrote an existing G-code file with overwrite=false")

    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    if not any(event.get("code") == "output_exists" for event in events):
        raise AssertionError(f"worker did not report output_exists for overwrite=false:\n{proc.stdout}\n{proc.stderr}")


def assert_bad_request_version_rejected(worker, source_root, work_dir):
    request = worker_request(source_root, work_dir, "worker-cli-bad-version", "bad-version.gcode")
    request["version"] = 999
    write_json(work_dir / "request-bad-version.json", request)

    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-bad-version.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    if proc.returncode == 0:
        raise AssertionError("worker accepted an unsupported request version")

    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    if not any(event.get("code") == "unsupported_protocol_version" for event in events):
        raise AssertionError(f"worker did not report unsupported request version:\n{proc.stdout}\n{proc.stderr}")


def assert_project_embedded_requires_orca_project(worker, source_root, work_dir):
    request = worker_request(
        source_root,
        work_dir,
        "worker-cli-bad-project-config",
        "bad-project-config.gcode",
        input_type="3mf",
        input_path=test_3mf_path(source_root),
        config_type="project_embedded",
    )
    request["config"] = {"type": "project_embedded"}
    write_json(work_dir / "request-bad-project-config.json", request)

    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-bad-project-config.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    if proc.returncode == 0:
        raise AssertionError("worker accepted project_embedded config for a generic 3MF request")

    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    if not any(event.get("code") == "invalid_request" for event in events):
        raise AssertionError(f"worker did not reject invalid project_embedded request:\n{proc.stdout}\n{proc.stderr}")


def run_cli(worker, source_root, root_work_dir):
    work_dir = root_work_dir / "cli"
    shutil.rmtree(work_dir, ignore_errors=True)
    work_dir.mkdir(parents=True)

    write_json(work_dir / "config.json", worker_config())
    request = worker_request(source_root, work_dir, "worker-cli-stl", "cli-output.gcode")
    write_json(work_dir / "request.json", request)

    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker CLI failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")

    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    results = [event for event in events if event.get("type") == "result"]
    if not results or not results[-1].get("success"):
        raise AssertionError(f"worker CLI did not emit a successful result: {proc.stdout}")
    assert_gcode(work_dir / "cli-output.gcode")
    assert_intermediates_cleaned(work_dir)
    assert_overwrite_false_rejected(worker, source_root, work_dir)
    assert_bad_request_version_rejected(worker, source_root, work_dir)
    assert_project_embedded_requires_orca_project(worker, source_root, work_dir)

    request_3mf = worker_request(
        source_root,
        work_dir,
        "worker-cli-3mf",
        "cli-3mf-output.gcode",
        input_type="3mf",
        input_path=test_3mf_path(source_root),
    )
    write_json(work_dir / "request-3mf.json", request_3mf)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-3mf.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker CLI 3MF failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    assert_gcode(work_dir / "cli-3mf-output.gcode")
    assert_intermediates_cleaned(work_dir)

    request_orca_project = worker_request(
        source_root,
        work_dir,
        "worker-cli-orca-project-3mf",
        "cli-orca-project-output.gcode",
        input_type="orca_3mf_project",
        input_path=orca_project_3mf_path(source_root),
        config_type="project_embedded",
    )
    write_json(work_dir / "request-orca-project.json", request_orca_project)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-orca-project.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker CLI Orca project 3MF failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    assert_gcode(work_dir / "cli-orca-project-output.gcode")
    assert_intermediates_cleaned(work_dir)

    write_json(work_dir / "config-oversized-flush.json", worker_config_with_oversized_flush_matrix())
    request_oversized_flush = worker_request(
        source_root,
        work_dir,
        "worker-cli-oversized-flush-matrix",
        "cli-oversized-flush-output.gcode",
    )
    request_oversized_flush["config"]["path"] = str(work_dir / "config-oversized-flush.json")
    write_json(work_dir / "request-oversized-flush.json", request_oversized_flush)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-oversized-flush.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker CLI oversized flush matrix failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    if not any(event.get("code") == "unknown_config_key" for event in events):
        raise AssertionError(f"worker did not report unknown key warning for oversized flush matrix test:\n{proc.stdout}")
    assert_gcode(work_dir / "cli-oversized-flush-output.gcode")
    assert_intermediates_cleaned(work_dir)


def recv_json_line(sock, timeout_at):
    buffer = bytearray()
    while time.monotonic() < timeout_at:
        chunk = sock.recv(1)
        if not chunk:
            raise AssertionError("worker socket closed before result")
        if chunk == b"\n":
            if buffer:
                raw = buffer.decode("utf-8")
                try:
                    return json.loads(raw)
                except json.JSONDecodeError as exc:
                    raise AssertionError(f"worker socket sent invalid JSON line: {raw!r}") from exc
            continue
        buffer.extend(chunk)
    raise AssertionError("timed out waiting for worker socket event")


def run_socket(worker, source_root, root_work_dir):
    work_dir = root_work_dir / "socket"
    shutil.rmtree(work_dir, ignore_errors=True)
    work_dir.mkdir(parents=True)

    socket_path = Path(f"/tmp/orca-worker-{os.getpid()}.sock")
    try:
        socket_path.unlink()
    except FileNotFoundError:
        pass
    write_json(work_dir / "config.json", worker_config())
    request = worker_request(source_root, work_dir, "worker-socket-stl", "socket-output.gcode")
    write_json(work_dir / "request.json", request)
    second_request = worker_request(
        source_root,
        work_dir,
        "worker-socket-3mf",
        "socket-output-2.gcode",
        input_type="3mf",
        input_path=test_3mf_path(source_root),
    )
    write_json(work_dir / "request-2.json", second_request)

    proc = subprocess.Popen(
        [str(worker), "serve", "--socket", str(socket_path)],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        timeout_at = time.monotonic() + 30
        while not socket_path.exists():
            if proc.poll() is not None:
                stdout, stderr = proc.communicate(timeout=5)
                raise AssertionError(f"worker server exited early with {proc.returncode}\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}")
            if time.monotonic() >= timeout_at:
                raise AssertionError("timed out waiting for worker socket")
            time.sleep(0.05)

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as disconnected:
            disconnected.settimeout(10)
            disconnected.connect(str(socket_path))
            disconnected.sendall(json.dumps({"type": "hello", "protocol": 1}).encode("utf-8") + b"\n")
        time.sleep(0.1)
        if proc.poll() is not None:
            stdout, stderr = proc.communicate(timeout=5)
            raise AssertionError(f"worker server exited after client disconnect with {proc.returncode}\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}")

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as bad_client:
            bad_client.settimeout(30)
            bad_client.connect(str(socket_path))
            bad_client.sendall(json.dumps({"type": "start_job", "request_path": str(work_dir / "request.json")}).encode("utf-8") + b"\n")
            event = recv_json_line(bad_client, time.monotonic() + 30)
            if event.get("code") != "bad_protocol":
                raise AssertionError(f"worker accepted start_job before hello: {event}")

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(120)
            client.connect(str(socket_path))
            client.sendall(json.dumps({"type": "hello", "protocol": 999}).encode("utf-8") + b"\n")
            event = recv_json_line(client, time.monotonic() + 30)
            if event.get("code") != "unsupported_protocol_version":
                raise AssertionError(f"worker accepted unsupported protocol version: {event}")
            client.sendall(json.dumps({"type": "hello", "protocol": 1}).encode("utf-8") + b"\n")
            event = recv_json_line(client, time.monotonic() + 30)
            if event.get("type") != "hello" or event.get("protocol") != 1:
                raise AssertionError(f"worker did not return a versioned hello: {event}")
            client.sendall(json.dumps({"type": "start_job", "request_path": str(work_dir / "request.json")}).encode("utf-8") + b"\n")

            result = None
            timeout_at = time.monotonic() + 120
            while result is None:
                event = recv_json_line(client, timeout_at)
                if event.get("type") == "result":
                    result = event
            if not result.get("success"):
                raise AssertionError(f"worker socket returned failure result: {result}")
            client.sendall(json.dumps({"type": "start_job", "request_path": str(work_dir / "request-2.json")}).encode("utf-8") + b"\n")
            second_result = None
            timeout_at = time.monotonic() + 120
            while second_result is None:
                event = recv_json_line(client, timeout_at)
                if event.get("type") == "result":
                    second_result = event
            if not second_result.get("success"):
                raise AssertionError(f"worker socket returned failure result for second job: {second_result}")
            client.sendall(json.dumps({"type": "stop", "force": True}).encode("utf-8") + b"\n")

        assert_gcode(work_dir / "socket-output.gcode")
        assert_gcode(work_dir / "socket-output-2.gcode")
        assert_intermediates_cleaned(work_dir)
    finally:
        try:
            socket_path.unlink()
        except FileNotFoundError:
            pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    if not args.worker.exists():
        raise AssertionError(f"worker executable not found: {args.worker}")
    if os.name == "nt":
        raise AssertionError("socket worker tests currently require Unix domain sockets")

    args.work_dir.mkdir(parents=True, exist_ok=True)
    run_cli(args.worker, args.source_root, args.work_dir)
    run_socket(args.worker, args.source_root, args.work_dir)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
