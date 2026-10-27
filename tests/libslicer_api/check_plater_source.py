"""Verify the compiled calibration methods against the pinned Orca source."""
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import re

root = Path(__file__).resolve().parents[2]
source_dir = root / "src/libslicer_api/orca"
origin = json.loads((source_dir / "origin.json").read_text(encoding="utf-8"))
copied = (source_dir / "PlaterCalibration.original.inc").read_text(encoding="utf-8")
def normalized_body(text):
    # Ignore line-ending and trailing-space differences from Git checkout.
    return "\n".join(line.rstrip() for line in text.splitlines()).strip() + "\n"


body = normalized_body(copied[copied.index(origin["start"]):])
assert hashlib.sha256(body.encode()).hexdigest() == origin["sha256_normalized_body"], "Original Plater reference changed"
for path, expected_hash in origin["algorithms"].items():
    algorithm = (root / path).read_text(encoding="utf-8")
    assert hashlib.sha256(algorithm.encode()).hexdigest() == expected_hash, f"Original algorithm changed: {path}"

# Source archives need no Git history. A checkout additionally verifies the
# pinned revision itself, rather than trusting only the recorded checksum.
original = subprocess.run(
    ["git", "-C", str(root), "show", f'{origin["revision"]}:{origin["path"]}'],
    capture_output=True, check=False,
) if shutil.which("git") else None
if original is not None and original.returncode == 0:
    text = original.stdout.decode("utf-8").replace("\r\n", "\n")
    expected = normalized_body(text[text.index(origin["start"]):text.index(origin["end_exclusive"])])
    assert body == expected, "Compiled methods differ from the original Plater.cpp"

def adapt_gui_access(text):
    """Exhaustive GUI removal / context substitutions; no numerical rewrites.

    Keep these rules explicit: a production edit outside this allowlist must
    fail the source check rather than be accepted by an updated output hash.
    """
    start = text.index("void Plater::_calib_pa_select_added_objects()")
    end = text.index("// Adjust settings for flowrate calibration", start)
    text = text[:start] + text[end:]
    start = text.index("    wxString calib_name;")
    end = text.index("    if (is_linear) {\n        if (pass == 1)", start)
    text = text[:start] + "    new_project();\n\n" + text[end:]
    text = text.replace('''        if (obj_idx == 0)
            sidebar().obj_list()->update_name_for_items();
        else
            sidebar().obj_list()->add_object_to_list(obj_idx);
''', "")
    text = text.replace('''    const auto canvas    = wxGetApp().plater()->canvas3D();
    auto&      selection = canvas->get_selection();''', "    auto& selection = m_selection;")
    lines = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith(("wxGetApp().get_tab(", "wxGetApp().mainframe->", "changed_objects(",
                                "wxBusyCursor ", "TransformationType ", "transformation_type.",
                                "_calib_pa_select_added_objects();", "std::vector<size_t> object_idxs",
                                "object_idxs.emplace_back(", "const std::vector<size_t> object_idx(")):
            continue
        if re.fullmatch(r'const auto \w+ = wxString::Format\(L".*"\);', stripped):
            continue
        if stripped.startswith("new_project(false, false,"):
            line = "    new_project();\n"
        lines.append(line)
    text = "".join(lines)
    text = text.replace("void adjust_settings_for_flowrate_calib(", "void Plater::adjust_settings_for_flowrate_calib(")
    text = text.replace("wxGetApp().preset_bundle->", "m_presets.")
    text = text.replace("wxGetApp().preset_bundle", "&m_presets")
    text = text.replace("p->background_process.fff_print()->", "m_print.")
    text = text.replace("p->model", "m_model")
    text = text.replace('sidebar().obj_list()->load_generic_subobject("Cube", ModelVolumeType::INVALID)', "load_generic_subobject()")
    text = text.replace("_L(", "std::string(")
    text = text.replace("get_notification_manager()->push_notification(msg.ToStdString())", "m_notifications.push_back(msg)")
    text = text.replace("}, transformation_type)", "})")
    text = text.replace('    canvas->do_scale("");', "    for (auto* object : objects)\n        object->ensure_on_bed();")
    text = re.sub(r'    wxCHECK_RET\((.*), (".*")\);',
                  r'    if (!(\1))\n        throw std::out_of_range(\2);', text)
    assert "wxGetApp" not in text and "wxString" not in text and "wxCHECK" not in text
    return text


adapted = (source_dir / "PlaterCalibration.inc").read_text(encoding="utf-8")
adapted = normalized_body(adapted[adapted.index(origin["start"]):])
assert adapted == adapt_gui_access(body), "Calibration implementation contains changes outside the GUI adaptation allowlist"
print("Original algorithms and GUI-only adaptation verified:", origin["revision"])
