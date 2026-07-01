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
        "gcode_comments": True,
        "layer_change_gcode": "G92 E0",
    }


def worker_request(source_root, work_dir, job_id, output_name):
    return {
        "version": 1,
        "job_id": job_id,
        "kind": "slice",
        "working_dir": str(work_dir),
        "resources_dir": str(source_root / "resources"),
        "data_dir": str(work_dir / "data"),
        "input": {
            "type": "stl",
            "path": str(source_root / "tests" / "data" / "test_3mf" / "Prusa.stl"),
        },
        "config": {
            "type": "resolved_orca_json",
            "path": str(work_dir / "config.json"),
        },
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
    second_request = worker_request(source_root, work_dir, "worker-socket-stl-2", "socket-output-2.gcode")
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

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(120)
            client.connect(str(socket_path))
            client.sendall(json.dumps({"type": "hello", "protocol": 1}).encode("utf-8") + b"\n")
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
