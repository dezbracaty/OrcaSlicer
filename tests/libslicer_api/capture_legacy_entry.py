#!/usr/bin/env python3
"""Build a diagnostic copy of Bulber; never patch the supplied source tree.

The trace contains real source coordinates before and after H/S/CUT and machine
quantization. A standard G-code configuration is input, not reconstructed paths.
This does not imply that the supplied Bulber version reproduces the standard.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path


TRACE_HEADER = r'''#pragma once
#include "ExtrusionEntity.hpp"
#include "nlohmann/json.hpp"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
namespace Slic3r { namespace legacy_entry_trace {
using J = nlohmann::json;
inline std::mutex mutex;
inline uint64_t sequence = 0;
inline thread_local J context = J::object();
inline bool enabled() { return std::getenv("LEGACY_ENTRY_TRACE") != nullptr; }
inline void emit(const char* stage, J record = J::object()) {
    const char* file = std::getenv("LEGACY_ENTRY_TRACE");
    if (!file) return;
    std::lock_guard<std::mutex> lock(mutex);
    static std::ofstream output(file);
    record["stage"]=stage; record["sequence"]=++sequence;
    std::ostringstream thread; thread<<std::this_thread::get_id();
    record["thread"]=thread.str(); record["gcode_context"]=context;
    output<<record.dump()<<'\n'; output.flush();
    if (!output) throw std::runtime_error("Cannot write legacy entry diagnostic trace");
}
inline J points(const Polyline& line) {
    J result=J::array();
    for(const auto& p:line.points) result.push_back({unscale<double>(p.x()),unscale<double>(p.y())});
    return result;
}
inline void path(const char* stage,const ExtrusionPath& p,J extra=J::object()) {
    if(!enabled() || !is_fiber(p.role())) return;
    extra["role"]=int(p.role()); extra["points_mm"]=points(p.polyline);
    extra["cut_point_index"]=p.cut_point_index; extra["has_cut_point"]=p.has_cut_point;
    emit(stage,extra);
}
struct PathScope {
    const ExtrusionPath& value; std::string stage; J extra;
    PathScope(const ExtrusionPath& p,const char* s,J info=J::object()):value(p),stage(s),extra(info) {
        path((stage+"_before").c_str(),value,extra);
    }
    ~PathScope() { path((stage+"_after").c_str(),value,extra); }
};
} }
'''

DRIVER = r'''#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/LegacyEntryTrace.hpp"
#include <tbb/task_arena.h>
#include <fstream>
#include <iostream>
using namespace Slic3r;
int main(int argc,char** argv) {
    if(argc!=6) {std::cerr<<"model settings.json output.gcode resources effective.json\n";return 2;}
    try {
        // Its worker-naming barrier reads arena concurrency. Use a serial
        // arena, rather than a global limit that would deadlock that barrier.
        tbb::task_arena serial(1);
        return serial.execute([&]() {
        set_resources_dir(argv[4]);
        std::ifstream input(argv[2]); nlohmann::json spec;input>>spec;
        DynamicPrintConfig config=DynamicPrintConfig::full_print_config();
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
        nlohmann::json unsupported=nlohmann::json::array();
        nlohmann::json enum_bindings=nlohmann::json::array();
        for(auto item=spec["settings"].begin();item!=spec["settings"].end();++item) {
            // Bulber's default coEnums objects omit keys_map. Bind the existing
            // definition for this headless loader; do not substitute values.
            const auto* def=config.def()->get(item.key());
            if(def && def->type==coEnums) {
                auto* option=config.option(item.key(),true);
                if(!def->enum_keys_map) throw std::runtime_error("Missing enum definition: "+item.key());
                if(option->nullable()) static_cast<ConfigOptionEnumsGenericNullable*>(option)->keys_map=def->enum_keys_map;
                else static_cast<ConfigOptionEnumsGeneric*>(option)->keys_map=def->enum_keys_map;
                enum_bindings.push_back(item.key());
            }
            try {config.set_deserialize(item.key(),item.value().get<std::string>(),substitutions);}
            catch(const UnknownOptionException&) {unsupported.push_back(item.key());}
        }
        config.normalize_fdm();
        nlohmann::json effective=nlohmann::json::object();
        for(const auto& key:config.keys()) effective["settings"][key]=config.opt_serialize(key);
        effective["unknown_standard_keys"]=unsupported;
        effective["headless_enum_definition_bindings"]=enum_bindings;
        effective["legacy_ignored_keys"]=substitutions.unrecogized_keys;
        effective["serial_diagnostic_scheduler"]=true;
        std::ofstream config_out(argv[5]);config_out<<effective.dump(2)<<'\n';config_out.close();
        Model model=Model::read_from_file(argv[1]);model.add_default_instances();
        for(auto* object:model.objects) object->ensure_on_bed();
        model.center_instances_around_point(Vec2d(spec["center"][0],spec["center"][1]));
        Print print;
        // The GUI/CLI normally supplies this machine classification. Bulber's
        // Print constructor leaves it unset; the reference machine is Klipper.
        if(config.opt_serialize("gcode_flavor")!="klipper")
            throw std::runtime_error("This diagnostic driver expects the reference Klipper machine");
        print.is_BBL_printer()=false;
        print.set_status_callback([](const PrintBase::SlicingStatus& s){std::cerr<<s.percent<<"% "<<s.text<<'\n';});
        print.apply(model,config);
        StringObjectException warning;
        const auto error=print.validate(&warning);
        if(!warning.string.empty()) std::cerr<<"Validation warning: "<<warning.string<<'\n';
        if(!error.string.empty() && !error.canslice) throw std::runtime_error(error.string);
        legacy_entry_trace::emit("run_start",{{"model",argv[1]},{"unknown_standard_keys",unsupported},
            {"placement_center",spec["center"]},{"serial_diagnostic_scheduler",true}});
        print.process();
        GCodeProcessorResult result;
        print.export_gcode(argv[3],&result);
        legacy_entry_trace::emit("run_complete",{{"gcode",argv[3]}});
        return 0;
        });
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(path, old, new):
    value = path.read_text(encoding='utf-8-sig')
    count = value.count(old)
    if count != 1:
        raise RuntimeError('{}: expected one patch anchor, found {}'.format(path, count))
    path.write_text(value.replace(old, new))


def prepare(source, output, standard, model):
    if source == output or source in output.parents:
        raise RuntimeError('Diagnostic output must be outside the supplied source tree')
    target = output/'legacy-source'
    if (output/'diagnostic-manifest.json').exists():
        raise RuntimeError('Output already prepared; use --resume or a new output directory')
    output.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        shutil.copytree(source, target, ignore=shutil.ignore_patterns('build_arm64', '.git', '.DS_Store', '__pycache__'))
    # Verify the entire tracked/untracked source copy before adding diagnostics.
    originals = {str(p.relative_to(source)): digest(p) for p in source.rglob('*')
                 if p.is_file() and p.suffix in ('.cpp', '.hpp', '.h', '.inc', '.cmake')
                 and not any(part in ('build_arm64', '.git') for part in p.relative_to(source).parts)}
    for name, expected in originals.items():
        if digest(target/name) != expected:
            raise RuntimeError('Diagnostic copy differs before patch: '+name)
    manifest = dict(source=str(source), source_head=subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip(),
        source_version=(source/'version.inc').read_text(), source_sha256=originals,
        standard=str(standard), standard_sha256=digest(standard), model=str(model), model_sha256=digest(model),
        limitations=['local Bulber version may differ from the standard producer',
                     'serial diagnostic scheduler; original seam counter is not reset or replaced'])
    (output/'source.patch').write_bytes(subprocess.check_output(['git', 'diff', '--binary', '--', '.'], cwd=source))
    lib = target/'src/libslic3r'
    (lib/'LegacyEntryTrace.hpp').write_text(TRACE_HEADER)
    for name in ('GCode.cpp', 'GCodeWriter.cpp', 'LayerRegion.cpp'):
        p = lib/name
        p.write_text('#include "LegacyEntryTrace.hpp"\n'+p.read_text(encoding='utf-8-sig'))
    p = lib/'Fill/FillConcentricCorners.cpp'
    p.write_text('#include "../LegacyEntryTrace.hpp"\n'+p.read_text(encoding='utf-8-sig'))
    replace_once(p, 'int    rr = randomnum();', '''int    rr = randomnum();
    legacy_entry_trace::emit("contour_seam_counter",{{"rr",rr},{"layer",layer_id},{"z",z}});''')
    replace_once(p, '        last_pos = polylines_out.back().last_point();', '''        legacy_entry_trace::emit("contour_seam_selected",{{"rr",rr},{"layer",layer_id},{"z",z},
            {"points_mm",legacy_entry_trace::points(polylines_out.back())}});
        last_pos = polylines_out.back().last_point();''')
    replace_once(p, '    // split paths using a nearest neighbor search', '''    for(const auto& ring:loops) legacy_entry_trace::emit("contour_generated",{{"layer",layer_id},{"z",z},
        {"points_mm",legacy_entry_trace::points(ring.split_at_index(0))}});
    // split paths using a nearest neighbor search''')
    p = lib/'LayerRegion.cpp'
    replace_once(p, 'void LayerRegion::simplify_path(ExtrusionPath* path)\n{', '''void LayerRegion::simplify_path(ExtrusionPath* path)
{
    legacy_entry_trace::PathScope trace(*path,"layer_simplify",{{"layer",this->layer()->id()},
        {"z",this->layer()->print_z}});''')
    p = lib/'GCode.cpp'
    replace_once(p, '    ExtrusionPath path(_path);', '''    ExtrusionPath path(_path);
    if(is_fiber(path.role())) {
        legacy_entry_trace::context={{"layer",m_layer ? m_layer->id() : size_t(-1)},
            {"z",m_layer ? m_layer->print_z : 0.0},{"origin",{m_origin.x(),m_origin.y()}}};
        legacy_entry_trace::path("gcode_source",path);
    }''')
    replace_once(p, '            path.reverse();\n            auto ii = path.split_start(scale_(m_config.fiber_z_hop));', '''            path.reverse();
            legacy_entry_trace::path("contour_reversed",path);
            auto ii = path.split_start(scale_(m_config.fiber_z_hop));
            legacy_entry_trace::path("contour_after_H",path);''')
    # All start/split/cut sites are logged without changing their control flow.
    text = p.read_text()
    text = text.replace('path.simplify(scale_(0.1));',
        'legacy_entry_trace::path("infill_before_simplify_0_1",path); path.simplify(scale_(0.1)); legacy_entry_trace::path("infill_after_simplify_0_1",path);')
    text = re.sub(r'(?<!\w)path.split_start\(scale_\(m_config.fiber_z_hop\)\);',
        'path.split_start(scale_(m_config.fiber_z_hop)); legacy_entry_trace::path("after_H",path);', text)
    text = text.replace('path.split_straightline(scale_(m_config.fiber_start_length));',
        'path.split_straightline(scale_(m_config.fiber_start_length)); legacy_entry_trace::path("after_S",path);')
    # The substring also occurs inside fiberinfillpath: replace exact word sites.
    text = re.sub(r'(?<!\w)path.split_end\(scale_\(m_config.fibercut_length\)\);',
        'path.split_end(scale_(m_config.fibercut_length)); legacy_entry_trace::path("after_CUT_split",path);', text)
    text = text.replace('fiberinfillpath.split_start(scale_(m_config.fiber_z_hop));',
        'fiberinfillpath.split_start(scale_(m_config.fiber_z_hop)); legacy_entry_trace::path("infill_after_H",fiberinfillpath);')
    # Undo accidental substring replacement and explicitly log the infill path.
    text = text.replace('fiberinfillpath.split_straightline(scale_(m_config.fiber_start_length)); legacy_entry_trace::path("after_S",path);',
        'fiberinfillpath.split_straightline(scale_(m_config.fiber_start_length)); legacy_entry_trace::path("infill_after_S",fiberinfillpath);')
    text = text.replace('fiberinfillpath.split_end(scale_(m_config.fibercut_length));',
        'fiberinfillpath.split_end(scale_(m_config.fibercut_length)); legacy_entry_trace::path("infill_after_CUT_split",fiberinfillpath);')
    p.write_text(text)
    replace_once(p, '                                if(pindex == 1)\n                                {', '''                                if(pindex == 1)
                                {
                                    legacy_entry_trace::path("executed_source",path);
                                    legacy_entry_trace::emit("first_edge_machine",{
                                        {"a_mm",{unscale<double>(line.a.x()),unscale<double>(line.a.y())}},
                                        {"b_mm",{unscale<double>(line.b.x()),unscale<double>(line.b.y())}},
                                        {"unquantized_start",{point_to_gcode(line.a).x(),point_to_gcode(line.a).y()}},
                                        {"unquantized_target",{point_to_gcode(line.b).x(),point_to_gcode(line.b).y()}},
                                        {"quantized_start",{nozzlePos.x(),nozzlePos.y()}},
                                        {"quantized_target",{p.x(),p.y()}},
                                        {"writer_start",{m_writer.get_position().x(),m_writer.get_position().y(),m_writer.get_position().z()}},
                                        {"H",m_config.fiber_z_hop.value},{"S",m_config.fiber_start_length.value},
                                        {"D",m_config.fibercut_length.value}});''')
    p = lib/'GCodeWriter.cpp'
    replace_once(p, 'std::string GCodeWriter::z_unhop(const Vec2d &point)\n{', '''std::string GCodeWriter::z_unhop(const Vec2d &point)
{
    legacy_entry_trace::emit("z_unhop",{{"from",{m_pos.x(),m_pos.y(),m_pos.z()}},
        {"to",{point.x(),point.y(),m_pos.z()-m_lifted}},{"lifted",m_lifted},
        {"F",config.fiber_z_down_speed.value*60}});''')
    replace_once(p, 'std::string GCodeWriter::restart_fiber(double dU,double F)\n{', '''std::string GCodeWriter::restart_fiber(double dU,double F)
{
    legacy_entry_trace::emit("prefeed",{{"E",dU},{"F",F*60}});''')
    replace_once(p, 'std::string GCodeWriter::adhesion(int fiber_z_hop_pause_adhesion)\n{', '''std::string GCodeWriter::adhesion(int fiber_z_hop_pause_adhesion)
{
    legacy_entry_trace::emit("adhesion",{{"ms",fiber_z_hop_pause_adhesion}});''')
    (target/'legacy_entry_probe.cpp').write_text(DRIVER)
    with (target/'CMakeLists.txt').open('a') as f:
        f.write('\n# Diagnostic-only target; not part of the supplied Bulber tree.\n'
                'add_executable(legacy_entry_probe legacy_entry_probe.cpp)\n'
                'target_link_libraries(legacy_entry_probe PRIVATE libslic3r OpenSSL::Crypto)\n'
                'target_link_directories(legacy_entry_probe PRIVATE "${CMAKE_PREFIX_PATH}/lib")\n')
    text = standard.read_text()
    config = text.split('; CONFIG_BLOCK_START', 1)[1].split('; CONFIG_BLOCK_END', 1)[0]
    settings = dict(re.findall(r'^;[ \t]*(\w+)[ \t]*=[ \t]*(.*)$', config, re.MULTILINE))
    center = re.search(r'EXCLUDE_OBJECT_DEFINE .*? CENTER=([-+\d.]+),([-+\d.]+)', text)
    if not center:
        raise RuntimeError('Standard does not record placement center; provide a traceable placement before slicing')
    (output/'standard-settings.json').write_text(json.dumps(dict(settings=settings,
        center=[float(center[1]), float(center[2])]), indent=2)+'\n')
    manifest['diagnostic_sha256'] = {str(p.relative_to(target)): digest(p) for p in
        [lib/'LegacyEntryTrace.hpp', lib/'GCode.cpp', lib/'GCodeWriter.cpp', lib/'LayerRegion.cpp',
         lib/'Fill/FillConcentricCorners.cpp', target/'legacy_entry_probe.cpp', target/'CMakeLists.txt']}
    (output/'diagnostic-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return target


def run_logged(command, log, env=None):
    print('Running '+str(command[0])+'; log='+str(log), flush=True)
    with log.open('w') as f:
        subprocess.run([str(x) for x in command], stdout=f, stderr=subprocess.STDOUT, env=env, check=True)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'output', 'standard', 'model', 'deps-prefix'):
        cli.add_argument('--'+name, type=Path, required=True)
    cli.add_argument('--jobs', type=int, default=4)
    cli.add_argument('--prepare-only', action='store_true')
    cli.add_argument('--resume', action='store_true')
    args = cli.parse_args()
    output = args.output.resolve()
    if args.resume:
        manifest = json.loads((output/'diagnostic-manifest.json').read_text())
        if manifest['source'] != str(args.source.resolve()):
            raise RuntimeError('Resume source differs from the recorded source')
        for name in ('model', 'standard'):
            if digest(getattr(args, name)) != manifest[name+'_sha256']:
                raise RuntimeError('Resume '+name+' differs from the frozen input')
        target = output/'legacy-source'
        for name, expected in manifest['diagnostic_sha256'].items():
            if digest(target/name) != expected:
                raise RuntimeError('Diagnostic source changed since preparation: '+name)
    else:
        target = prepare(args.source.resolve(), output, args.standard.resolve(), args.model.resolve())
    if args.prepare_only:
        print(target)
        return 0
    build = output/'legacy-build'
    run_logged(['cmake', '-S', target, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        '-DSLIC3R_GUI=OFF', '-DSLIC3R_PCH=ON', '-DSLIC3R_STATIC=ON', '-DSLIC3R_BUILD_TESTS=OFF',
        '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_PREFIX_PATH='+str(args.deps_prefix.resolve())], output/'configure.log')
    run_logged(['cmake', '--build', build, '--target', 'legacy_entry_probe', '-j', args.jobs], output/'build.log')
    env = dict(os.environ, LEGACY_ENTRY_TRACE=str(output/'legacy-entry-trace.jsonl'))
    run_logged([build/'legacy_entry_probe', args.model.resolve(), output/'standard-settings.json',
        output/'legacy-4xiao.gcode', target/'resources', output/'legacy-effective-config.json'], output/'slice.log', env)
    manifest = json.loads((output/'diagnostic-manifest.json').read_text())
    manifest['capture_script_sha256'] = digest(Path(__file__))
    manifest['output_sha256'] = {name: digest(output/name) for name in
        ('legacy-4xiao.gcode', 'legacy-entry-trace.jsonl', 'legacy-effective-config.json', 'standard-settings.json')}
    manifest['probe_sha256'] = digest(build/'legacy_entry_probe')
    manifest['standard_reproduction_established'] = None  # requires independent comparison
    (output/'diagnostic-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
