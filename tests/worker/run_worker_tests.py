#!/usr/bin/env python3
import argparse
import array
import ctypes
import json
import mmap
import os
import shutil
import socket
import struct
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
        "default_nozzle_volume_type": ["Standard"],
        "filament_colour": ["#26A69A"],
        "filament_diameter": [1.75],
        "filament_extruder_variant": ["Direct Drive Standard"],
        "filament_map": [1],
        "filament_retract_lift_enforce": ["nil"],
        "filament_self_index": [1],
        "filament_settings_id": ["Test PLA"],
        "filament_type": ["PLA"],
        "gcode_flavor": 0,
        "gcode_comments": True,
        "layer_change_gcode": "G92 E0",
        "nozzle_diameter": [0.4],
        "nozzle_volume_type": ["Standard"],
        "print_settings_id": "Test Process",
        "printer_model": "Test Printer",
        "printer_settings_id": "Test Printer 0.4 nozzle",
        "before_layer_change_gcode": "",
        "use_relative_e_distances": True,
    }


def worker_config_with_oversized_flush_matrix():
    config = worker_config()
    config.update({
        "worker_test_unknown_key": True,
        "filament_colour": ["#26A69A", "#26A69A", "#26A69A", "#26A69A"],
        "filament_diameter": [1.75, 1.75, 1.75, 1.75],
        "filament_extruder_variant": [
            "Direct Drive Standard",
            "Direct Drive Standard",
            "Direct Drive Standard",
            "Direct Drive Standard",
        ],
        "filament_map": [1, 1, 1, 1],
        "filament_retract_lift_enforce": ["nil", "nil", "nil", "nil"],
        "filament_self_index": [1, 2, 3, 4],
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


def worker_bbl_config_without_layer_reset():
    config = worker_config()
    config.update({
        "printer_model": "Bambu Lab X1 Carbon",
        "printer_settings_id": "Bambu Lab X1 Carbon 0.4 nozzle",
        "print_settings_id": "0.20mm Standard @BBL X1C",
        "before_layer_change_gcode": "",
        "layer_change_gcode": "",
        "use_relative_e_distances": True,
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
    if config_type in {"resolved_orca_json", "preset_selection"}:
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


def preset_selection_config(source_root):
    return {
        "vendor_bundle_dirs": [
            str(source_root / "resources" / "profiles" / "OrcaFilamentLibrary"),
            str(source_root / "resources" / "profiles" / "BBL"),
        ],
        "printer_preset_id": "Bambu Lab X1 Carbon 0.4 nozzle",
        "process_preset_id": "0.20mm Standard @BBL X1C",
        "process_overrides": {
            "bridge_line_width": "0.4",
        },
        "filament_slots": [
            {
                "slot_index": 0,
                "filament_preset_id": "Bambu PLA Basic @BBL X1C",
                "color": "#FFFFFF",
                "filament_type": "PLA",
            },
            {
                "slot_index": 1,
                "filament_preset_id": "Bambu PLA Basic @BBL X1C",
                "color": "#000000",
                "filament_type": "PLA",
            },
        ],
    }


def project_process_preset(name):
    return {
        "type": "process",
        "name": name,
        "version": "1.0.0",
        "from": "project",
        "inherits": "0.20mm Standard @BBL X1C",
        "print_settings_id": name,
        "layer_height": "0.23",
    }


def enable_shared_memory_preview(request):
    request["output"]["preview"] = {
        "enabled": True,
        "transport": "shared_memory",
        "format": "orca-toolpath-preview-binary-v2",
        "publish": "final",
    }


def enable_shared_memory_fd_preview(request):
    request["output"]["preview"] = {
        "enabled": True,
        "transport": "shared_memory_fd",
        "format": "orca-toolpath-preview-binary-v2",
        "publish": "final",
    }


def assert_gcode(path):
    if not path.exists():
        raise AssertionError(f"G-code file was not created: {path}")
    content = path.read_text(encoding="utf-8", errors="replace")
    if "G1" not in content:
        raise AssertionError("G-code output does not contain movement commands")
    if "; filament used" not in content:
        raise AssertionError("G-code output does not contain filament summary")


def read_posix_shared_memory(shm_name, size):
    if not shm_name:
        raise AssertionError("shared-memory preview event did not include shm_name")
    if not isinstance(size, int) or size <= 0:
        raise AssertionError(f"shared-memory preview event has invalid size: {size!r}")

    libc = ctypes.CDLL(None, use_errno=True)
    libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    libc.shm_open.restype = ctypes.c_int
    libc.shm_unlink.argtypes = [ctypes.c_char_p]
    libc.shm_unlink.restype = ctypes.c_int

    name = shm_name.encode("utf-8")
    fd = libc.shm_open(name, os.O_RDONLY, 0)
    if fd < 0:
        errno = ctypes.get_errno()
        raise OSError(errno, os.strerror(errno), shm_name)
    try:
        mapping = mmap.mmap(fd, size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ)
        try:
            if libc.shm_unlink(name) != 0:
                errno = ctypes.get_errno()
                raise OSError(errno, os.strerror(errno), shm_name)
            return mapping[:]
        finally:
            mapping.close()
    finally:
        os.close(fd)


def shared_memory_exists(shm_name):
    if not shm_name:
        raise AssertionError("shared-memory name is empty")

    libc = ctypes.CDLL(None, use_errno=True)
    libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    libc.shm_open.restype = ctypes.c_int

    fd = libc.shm_open(shm_name.encode("utf-8"), os.O_RDONLY, 0)
    if fd >= 0:
        os.close(fd)
        return True
    errno = ctypes.get_errno()
    if errno == getattr(os, "ENOENT", 2):
        return False
    raise OSError(errno, os.strerror(errno), shm_name)


def assert_shared_memory_absent(shm_name):
    if shared_memory_exists(shm_name):
        raise AssertionError(f"shared-memory segment was not cleaned up: {shm_name}")


def read_fd_bytes(fd, size):
    if fd < 0:
        raise AssertionError("shared-memory fd preview event did not include a valid fd")
    if not isinstance(size, int) or size <= 0:
        raise AssertionError(f"shared-memory fd preview event has invalid size: {size!r}")
    try:
        mapping = mmap.mmap(fd, size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ)
        try:
            return mapping[:]
        finally:
            mapping.close()
    finally:
        os.close(fd)


def assert_preview_artifact_bytes(data, expected_transport=None):
    if len(data) < 80:
        raise AssertionError("preview artifact is smaller than the wire header")

    header = struct.unpack_from("<8sHHHHQQQQQQQQ", data, 0)
    magic, header_size, version, endian, section_count, table_offset, file_size, metadata_offset, metadata_size, *_ = header
    if magic != b"ORCAPV1\0":
        raise AssertionError(f"invalid preview artifact magic: {magic!r}")
    if header_size != 80 or version != 2 or endian != 0x0102:
        raise AssertionError(f"unsupported preview artifact header: size={header_size} version={version} endian={endian}")
    if file_size != len(data):
        raise AssertionError(f"preview artifact file size mismatch: header={file_size} actual={len(data)}")
    if table_offset + section_count * 32 > len(data):
        raise AssertionError("preview artifact section table is outside file bounds")
    if metadata_offset + metadata_size > len(data):
        raise AssertionError("preview artifact metadata is outside file bounds")

    metadata = json.loads(data[metadata_offset:metadata_offset + metadata_size].decode("utf-8"))
    if metadata.get("schema") != "orca.toolpath_preview":
        raise AssertionError(f"preview metadata schema mismatch: {metadata}")
    if metadata.get("format") != "orca-toolpath-preview-binary-v2":
        raise AssertionError(f"preview metadata format mismatch: {metadata}")
    if expected_transport is not None:
        actual_transport = metadata.get("output", {}).get("preview_transport")
        if actual_transport != expected_transport:
            raise AssertionError(f"preview metadata transport mismatch: {metadata}")

    sections = {}
    for index in range(section_count):
        section, record_size, offset, size, count = struct.unpack_from("<IIQQQ", data, table_offset + index * 32)
        if size and offset + size > len(data):
            raise AssertionError(f"preview section {section} is outside file bounds")
        if record_size and count and size != record_size * count:
            raise AssertionError(f"preview section {section} size/count mismatch")
        sections[section] = {
            "record_size": record_size,
            "offset": offset,
            "size": size,
            "count": count,
        }

    required = {1, 2, 3, 4, 5, 6, 9}
    missing = required.difference(sections)
    if missing:
        raise AssertionError(f"preview artifact is missing sections: {sorted(missing)}")
    if sections[9]["count"] <= 0:
        raise AssertionError("preview artifact has no move records")
    if sections[6]["count"] <= 0:
        raise AssertionError("preview artifact has no color records")
    if sections[9]["record_size"] != 216:
        raise AssertionError(f"unexpected move record size: {sections[9]['record_size']}")
    if sections[6]["record_size"] != 48:
        raise AssertionError(f"unexpected color record size: {sections[6]['record_size']}")


def assert_preview_artifact(path, expected_transport=None):
    if not path.exists():
        raise AssertionError(f"preview artifact was not created: {path}")
    assert_preview_artifact_bytes(path.read_bytes(), expected_transport=expected_transport)


def assert_intermediates_cleaned(work_dir):
    data_dir = work_dir / "data"
    artifacts_dir = work_dir / "artifacts"
    if data_dir.exists():
        raise AssertionError(f"worker data_dir was not cleaned: {data_dir}")
    if artifacts_dir.exists():
        raise AssertionError(f"empty worker artifacts_dir was not cleaned: {artifacts_dir}")


def assert_data_dir_cleaned(work_dir):
    data_dir = work_dir / "data"
    if data_dir.exists():
        raise AssertionError(f"worker data_dir was not cleaned: {data_dir}")


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


def assert_preset_selection_failure_code(worker, source_root, work_dir, name, selection, expected_code):
    write_json(work_dir / f"preset-selection-{name}.json", selection)

    request = worker_request(
        source_root,
        work_dir,
        f"worker-cli-preset-selection-{name}",
        f"{name}-output.gcode",
        config_type="preset_selection",
    )
    request["config"]["path"] = str(work_dir / f"preset-selection-{name}.json")
    write_json(work_dir / f"request-preset-selection-{name}.json", request)

    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / f"request-preset-selection-{name}.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode == 0:
        raise AssertionError(f"worker accepted invalid preset_selection for {name}")

    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    results = [event for event in events if event.get("type") == "result"]
    if not results or results[-1].get("code") != expected_code:
        raise AssertionError(f"worker did not propagate {expected_code} for preset_selection {name}:\n{proc.stdout}\n{proc.stderr}")


def assert_preset_selection_reports_sdk_error_code(worker, source_root, work_dir):
    selection = preset_selection_config(source_root)
    selection["filament_slots"][0]["filament_preset_id"] = "Missing SDK Filament Preset"
    assert_preset_selection_failure_code(worker, source_root, work_dir, "missing-filament", selection, "preset_not_found")


def assert_preset_selection_reports_incompatible_error_code(worker, source_root, work_dir):
    incompatible_process = preset_selection_config(source_root)
    incompatible_process["process_preset_id"] = "0.48mm Draft @BBL A1M 0.8 nozzle"
    assert_preset_selection_failure_code(
        worker,
        source_root,
        work_dir,
        "incompatible-process",
        incompatible_process,
        "preset_incompatible",
    )

    incompatible_filament = preset_selection_config(source_root)
    incompatible_filament["filament_slots"][0]["filament_preset_id"] = "Bambu ASA-CF @BBL A1"
    assert_preset_selection_failure_code(
        worker,
        source_root,
        work_dir,
        "incompatible-filament",
        incompatible_filament,
        "preset_incompatible",
    )


def assert_preset_selection_uses_project_preset_file(worker, source_root, work_dir):
    process_name = "SDK Worker Project Process"
    project_preset_relative = Path("project-presets") / "sdk-worker-project-process.config"
    write_json(work_dir / project_preset_relative, project_process_preset(process_name))

    selection = preset_selection_config(source_root)
    selection["project_preset_files"] = [str(project_preset_relative)]
    selection["process_preset_id"] = process_name
    write_json(work_dir / "preset-selection-project-process.json", selection)

    request = worker_request(
        source_root,
        work_dir,
        "worker-cli-preset-selection-project-process",
        "cli-preset-selection-project-process-output.gcode",
        config_type="preset_selection",
    )
    request["config"]["path"] = str(work_dir / "preset-selection-project-process.json")
    write_json(work_dir / "request-preset-selection-project-process.json", request)

    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-preset-selection-project-process.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker CLI project preset_selection failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")

    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    results = [event for event in events if event.get("type") == "result"]
    if not results or not results[-1].get("success"):
        raise AssertionError(f"worker CLI project preset_selection did not emit a successful result: {proc.stdout}")
    assert_gcode(work_dir / "cli-preset-selection-project-process-output.gcode")
    assert_intermediates_cleaned(work_dir)


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

    write_json(work_dir / "preset-selection.json", preset_selection_config(source_root))
    request_preset_selection = worker_request(
        source_root,
        work_dir,
        "worker-cli-preset-selection",
        "cli-preset-selection-output.gcode",
        config_type="preset_selection",
    )
    request_preset_selection["config"]["path"] = str(work_dir / "preset-selection.json")
    write_json(work_dir / "request-preset-selection.json", request_preset_selection)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-preset-selection.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker CLI preset_selection failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    assert_gcode(work_dir / "cli-preset-selection-output.gcode")
    assert_intermediates_cleaned(work_dir)
    assert_preset_selection_uses_project_preset_file(worker, source_root, work_dir)
    assert_preset_selection_reports_sdk_error_code(worker, source_root, work_dir)
    assert_preset_selection_reports_incompatible_error_code(worker, source_root, work_dir)

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

    write_json(work_dir / "config-bbl-no-g92.json", worker_bbl_config_without_layer_reset())
    request_bbl_no_g92 = worker_request(
        source_root,
        work_dir,
        "worker-cli-bbl-no-g92",
        "cli-bbl-no-g92-output.gcode",
    )
    request_bbl_no_g92["config"]["path"] = str(work_dir / "config-bbl-no-g92.json")
    write_json(work_dir / "request-bbl-no-g92.json", request_bbl_no_g92)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-bbl-no-g92.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker CLI BBL without G92 E0 failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    assert_gcode(work_dir / "cli-bbl-no-g92-output.gcode")
    assert_intermediates_cleaned(work_dir)


def run_preview_outputs(worker, source_root, root_work_dir):
    work_dir = root_work_dir / "preview"
    shutil.rmtree(work_dir, ignore_errors=True)
    work_dir.mkdir(parents=True)
    write_json(work_dir / "config.json", worker_config())

    preview_only = worker_request(source_root, work_dir, "worker-preview-only", "unused.gcode")
    preview_only["output"]["gcode"] = {"enabled": False}
    preview_only["output"]["preview"] = {
        "enabled": True,
        "required": True,
        "path": "preview-only.orcapv",
        "format": "orca-toolpath-preview-binary-v2",
        "publish": "final",
    }
    write_json(work_dir / "request-preview-only.json", preview_only)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-preview-only.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker preview-only slice failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    preview_events = [event for event in events if event.get("type") == "artifact" and event.get("kind") == "preview"]
    if not preview_events:
        raise AssertionError(f"worker did not emit preview artifact event:\n{proc.stdout}")
    ready = preview_events[-1]
    if ready.get("phase") != "ready" or not ready.get("complete"):
        raise AssertionError(f"preview artifact event did not report ready/complete: {ready}")
    if ready.get("schema") != "orca.toolpath_preview":
        raise AssertionError(f"preview artifact event schema mismatch: {ready}")
    if ready.get("format") != "orca-toolpath-preview-binary-v2":
        raise AssertionError(f"preview artifact event format mismatch: {ready}")
    if (work_dir / "unused.gcode").exists():
        raise AssertionError("preview-only request unexpectedly created public G-code")
    if (work_dir / "data" / "internal" / "processed.gcode.tmp").exists():
        raise AssertionError("preview-only request unexpectedly created internal G-code")
    assert_preview_artifact(work_dir / "artifacts" / "preview-only.orcapv", expected_transport="file")
    assert_data_dir_cleaned(work_dir)

    shared_memory_preview = worker_request(source_root, work_dir, "worker-preview-shared-memory", "unused-shm.gcode")
    shared_memory_preview["output"]["gcode"] = {"enabled": False}
    shared_memory_preview["output"]["preview"] = {
        "enabled": True,
        "required": True,
        "path": "shared-memory.orcapv",
        "transport": "shared_memory",
        "format": "orca-toolpath-preview-binary-v2",
        "publish": "final",
    }
    write_json(work_dir / "request-preview-shared-memory.json", shared_memory_preview)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-preview-shared-memory.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker shared-memory preview slice failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    preview_events = [event for event in events if event.get("type") == "artifact" and event.get("kind") == "preview"]
    if not preview_events:
        raise AssertionError(f"worker did not emit shared-memory preview artifact event:\n{proc.stdout}")
    ready = preview_events[-1]
    if ready.get("phase") != "ready" or not ready.get("complete"):
        raise AssertionError(f"shared-memory preview artifact event did not report ready/complete: {ready}")
    if ready.get("transport") != "shared_memory":
        raise AssertionError(f"shared-memory preview artifact event transport mismatch: {ready}")
    if "path" in ready:
        raise AssertionError(f"shared-memory preview artifact event unexpectedly exposed a file path: {ready}")
    if ready.get("schema") != "orca.toolpath_preview":
        raise AssertionError(f"shared-memory preview artifact event schema mismatch: {ready}")
    if ready.get("format") != "orca-toolpath-preview-binary-v2":
        raise AssertionError(f"shared-memory preview artifact event format mismatch: {ready}")
    preview_bytes = read_posix_shared_memory(ready.get("shm_name", ""), ready.get("size"))
    assert_preview_artifact_bytes(preview_bytes, expected_transport="shared_memory")
    if (work_dir / "artifacts" / "shared-memory.orcapv").exists():
        raise AssertionError("shared-memory preview request unexpectedly created a preview file")
    assert_data_dir_cleaned(work_dir)

    gcode_and_preview = worker_request(source_root, work_dir, "worker-gcode-preview", "with-preview.gcode")
    gcode_and_preview["output"]["gcode"] = {
        "enabled": True,
        "required": True,
        "path": str(work_dir / "with-preview.gcode"),
    }
    gcode_and_preview["output"]["preview"] = {
        "enabled": True,
        "required": True,
        "path": "with-preview.orcapv",
    }
    write_json(work_dir / "request-gcode-preview.json", gcode_and_preview)
    proc = subprocess.run(
        [str(worker), "slice", "--job", str(work_dir / "request-gcode-preview.json"), "--progress", "jsonl"],
        cwd=str(work_dir),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AssertionError(f"worker G-code+preview slice failed with {proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
    if not any(event.get("type") == "artifact" and event.get("kind") == "gcode" for event in events):
        raise AssertionError(f"worker did not emit G-code artifact event:\n{proc.stdout}")
    if not any(event.get("type") == "artifact" and event.get("kind") == "preview" for event in events):
        raise AssertionError(f"worker did not emit preview artifact event for G-code+preview:\n{proc.stdout}")
    assert_gcode(work_dir / "with-preview.gcode")
    assert_preview_artifact(work_dir / "artifacts" / "with-preview.orcapv", expected_transport="file")
    assert_data_dir_cleaned(work_dir)


def run_malformed_requests(worker, source_root, root_work_dir):
    work_dir = root_work_dir / "malformed"
    shutil.rmtree(work_dir, ignore_errors=True)
    work_dir.mkdir(parents=True)
    write_json(work_dir / "config.json", worker_config())

    base = worker_request(source_root, work_dir, "worker-malformed-base", "unused.gcode")
    cases = [
        ("gcode-enabled-string", lambda request: request["output"].update({
            "gcode": {"enabled": "not-a-bool", "path": str(work_dir / "unused.gcode")}
        }), "output.gcode.enabled"),
        ("preview-enabled-string", lambda request: request["output"].update({
            "gcode": {"enabled": False},
            "preview": {"enabled": "not-a-bool", "path": "bad.orcapv"}
        }), "output.preview.enabled"),
        ("preview-enabled-missing", lambda request: request["output"].update({
            "gcode": {"enabled": False},
            "preview": {"path": "bad.orcapv"}
        }), "output.preview.enabled"),
        ("preview-chunk-string", lambda request: request["output"].update({
            "gcode": {"enabled": False},
            "preview": {"enabled": True, "path": "bad.orcapv", "chunk_records": "not-an-int"}
        }), "output.preview.chunk_records"),
        ("preview-required-array", lambda request: request["output"].update({
            "gcode": {"enabled": False},
            "preview": {"enabled": True, "path": "bad.orcapv", "required": []}
        }), "output.preview.required"),
        ("overwrite-string", lambda request: request["options"].update({
            "overwrite": "yes"
        }), "options.overwrite"),
        ("input-path-number", lambda request: request["input"].update({
            "path": 123
        }), "input.path"),
        ("plate-index-string", lambda request: request["input"].update({
            "plate_index": "0"
        }), "input.plate_index"),
    ]

    for name, mutate, expected_message in cases:
        request = json.loads(json.dumps(base))
        request["job_id"] = f"worker-malformed-{name}"
        mutate(request)
        request_path = work_dir / f"request-{name}.json"
        write_json(request_path, request)
        proc = subprocess.run(
            [str(worker), "slice", "--job", str(request_path), "--progress", "jsonl"],
            cwd=str(work_dir),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=60,
        )
        if proc.returncode == 0:
            raise AssertionError(f"malformed request {name} unexpectedly succeeded\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
        events = [json.loads(line) for line in proc.stdout.splitlines() if line.lstrip().startswith("{")]
        results = [event for event in events if event.get("type") == "result"]
        if not results:
            raise AssertionError(f"malformed request {name} did not emit result event\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
        result = results[-1]
        if result.get("success") is not False:
            raise AssertionError(f"malformed request {name} result was not success=false: {result}")
        message = result.get("message", "")
        if expected_message not in message:
            raise AssertionError(f"malformed request {name} message did not identify {expected_message}: {result}")


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


class WorkerSocketReader:
    def __init__(self):
        self.buffer = bytearray()
        self.pending_fds = []

    def close_pending_fds(self):
        while self.pending_fds:
            os.close(self.pending_fds.pop())

    def _pop_event(self):
        newline = self.buffer.find(b"\n")
        if newline < 0:
            return None
        raw_bytes = bytes(self.buffer[:newline])
        del self.buffer[:newline + 1]
        if not raw_bytes:
            return None
        raw = raw_bytes.decode("utf-8")
        try:
            event = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise AssertionError(f"worker socket sent invalid JSON line: {raw!r}") from exc
        fd = self.pending_fds.pop(0) if self.pending_fds else None
        return event, fd

    def recv_event(self, sock, timeout_at):
        event = self._pop_event()
        if event is not None:
            return event

        while time.monotonic() < timeout_at:
            ancbuf_size = socket.CMSG_SPACE(array.array("i", [0]).itemsize * 4)
            try:
                chunk, ancdata, flags, _ = sock.recvmsg(4096, ancbuf_size)
            except socket.timeout as exc:
                raise AssertionError("timed out waiting for worker socket event") from exc
            if not chunk:
                raise AssertionError("worker socket closed before result")
            for level, cmsg_type, data in ancdata:
                if level != socket.SOL_SOCKET or cmsg_type != socket.SCM_RIGHTS:
                    continue
                fds = array.array("i")
                usable = len(data) - (len(data) % fds.itemsize)
                fds.frombytes(data[:usable])
                self.pending_fds.extend(fds.tolist())
            self.buffer.extend(chunk)
            event = self._pop_event()
            if event is not None:
                return event
        raise AssertionError("timed out waiting for worker socket event")


def recv_worker_event(reader, sock, timeout_at, label):
    try:
        return reader.recv_event(sock, timeout_at)
    except AssertionError as exc:
        raise AssertionError(f"{label}: {exc}") from exc


def send_start_job(sock, request_path):
    sock.sendall(json.dumps({"type": "start_job", "request_path": str(request_path)}).encode("utf-8") + b"\n")


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
    enable_shared_memory_preview(request)
    write_json(work_dir / "request.json", request)
    second_request = worker_request(
        source_root,
        work_dir,
        "worker-socket-3mf",
        "socket-output-2.gcode",
        input_type="3mf",
        input_path=test_3mf_path(source_root),
    )
    enable_shared_memory_preview(second_request)
    write_json(work_dir / "request-2.json", second_request)
    fd_request = worker_request(source_root, work_dir, "worker-socket-fd-preview", "socket-output-fd.gcode")
    enable_shared_memory_fd_preview(fd_request)
    write_json(work_dir / "request-fd.json", fd_request)

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
            reader = WorkerSocketReader()
            client.sendall(json.dumps({"type": "hello", "protocol": 999}).encode("utf-8") + b"\n")
            event, fd = recv_worker_event(reader, client, time.monotonic() + 30, "unsupported protocol response")
            if fd is not None:
                os.close(fd)
            if event.get("code") != "unsupported_protocol_version":
                raise AssertionError(f"worker accepted unsupported protocol version: {event}")
            client.sendall(json.dumps({"type": "hello", "protocol": 1}).encode("utf-8") + b"\n")
            event, fd = recv_worker_event(reader, client, time.monotonic() + 30, "hello response")
            if fd is not None:
                os.close(fd)
            if event.get("type") != "hello" or event.get("protocol") != 1:
                raise AssertionError(f"worker did not return a versioned hello: {event}")
            send_start_job(client, work_dir / "request.json")

            result = None
            first_preview = None
            timeout_at = time.monotonic() + 120
            while result is None:
                event, fd = recv_worker_event(reader, client, timeout_at, "first named-shm socket job")
                if fd is not None:
                    os.close(fd)
                if (event.get("type") == "artifact" and
                        event.get("kind") == "preview" and
                        event.get("transport") == "shared_memory"):
                    first_preview = event
                if event.get("type") == "result":
                    result = event
            if not result.get("success"):
                raise AssertionError(f"worker socket returned failure result: {result}")
            if first_preview is None:
                raise AssertionError("worker socket did not emit shared-memory preview for first job")
            send_start_job(client, work_dir / "request-2.json")
            second_result = None
            second_preview = None
            timeout_at = time.monotonic() + 120
            while second_result is None:
                event, fd = recv_worker_event(reader, client, timeout_at, "second named-shm socket job")
                if fd is not None:
                    os.close(fd)
                if event.get("type") == "error" and event.get("code") == "job_active":
                    time.sleep(0.05)
                    send_start_job(client, work_dir / "request-2.json")
                    continue
                if (event.get("type") == "artifact" and
                        event.get("kind") == "preview" and
                        event.get("transport") == "shared_memory"):
                    second_preview = event
                if event.get("type") == "result":
                    second_result = event
            if not second_result.get("success"):
                raise AssertionError(f"worker socket returned failure result for second job: {second_result}")
            if second_preview is None:
                raise AssertionError("worker socket did not emit shared-memory preview for second job")
            assert_shared_memory_absent(first_preview.get("shm_name", ""))

            send_start_job(client, work_dir / "request-fd.json")
            fd_result = None
            fd_preview = None
            fd_preview_descriptor = None
            timeout_at = time.monotonic() + 120
            while fd_result is None:
                event, fd = recv_worker_event(reader, client, timeout_at, "fd preview socket job")
                if event.get("type") == "error" and event.get("code") == "job_active":
                    if fd is not None:
                        os.close(fd)
                    time.sleep(0.05)
                    send_start_job(client, work_dir / "request-fd.json")
                    continue
                if (event.get("type") == "artifact" and
                        event.get("kind") == "preview" and
                        event.get("transport") == "shared_memory_fd"):
                    fd_preview = event
                    fd_preview_descriptor = fd
                elif fd is not None:
                    os.close(fd)
                if event.get("type") == "result":
                    fd_result = event
            if not fd_result.get("success"):
                if fd_preview_descriptor is not None:
                    os.close(fd_preview_descriptor)
                raise AssertionError(f"worker socket returned failure result for fd preview job: {fd_result}")
            if fd_preview is None:
                raise AssertionError("worker socket did not emit shared-memory-fd preview")
            if fd_preview_descriptor is None:
                raise AssertionError(f"shared-memory-fd preview event did not include an fd: {fd_preview}")
            if fd_preview.get("path"):
                raise AssertionError(f"shared-memory-fd preview unexpectedly exposed a path: {fd_preview}")
            if fd_preview.get("shm_name"):
                raise AssertionError(f"shared-memory-fd preview unexpectedly exposed a shm name: {fd_preview}")
            fd_preview_bytes = read_fd_bytes(fd_preview_descriptor, fd_preview.get("size"))
            fd_preview_descriptor = None
            assert_preview_artifact_bytes(fd_preview_bytes, expected_transport="shared_memory_fd")

            client.sendall(json.dumps({"type": "stop", "force": True}).encode("utf-8") + b"\n")
            reader.close_pending_fds()

        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            stdout, stderr = proc.communicate(timeout=5)
            raise AssertionError(f"worker server did not stop after stop request\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}")
        if proc.returncode != 0:
            stdout, stderr = proc.communicate(timeout=5)
            raise AssertionError(f"worker server exited with {proc.returncode}\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}")
        assert_shared_memory_absent(second_preview.get("shm_name", ""))

        assert_gcode(work_dir / "socket-output.gcode")
        assert_gcode(work_dir / "socket-output-2.gcode")
        assert_gcode(work_dir / "socket-output-fd.gcode")
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
    run_preview_outputs(args.worker, args.source_root, args.work_dir)
    run_malformed_requests(args.worker, args.source_root, args.work_dir)
    run_socket(args.worker, args.source_root, args.work_dir)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
