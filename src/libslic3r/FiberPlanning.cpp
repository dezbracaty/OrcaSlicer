// Region and concentric recipes migrated from the fixed bulber source.
#include "FiberPlanning.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Fill/FillBase.hpp"
#include "ShortestPath.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
namespace Slic3r {
bool fiber_active(const PrintRegionConfig& c) { return c.generate_reinforced_infills.value || c.generate_reinforced_perimeters.value; }
bool fiber_active(const Print& print) { for(const auto* object:print.objects())for(size_t r=0;r<object->num_printing_regions();++r)if(fiber_active(object->printing_region(r).config()))return true;return false; }
static void fiber_throw_if_canceled(const Print& print)
{
    if (print.canceled()) throw CanceledException();
}
static ExPolygon fiber_largest_contour(const ExPolygons& polygons)
{
    ExPolygon largest;double area=0;for(const auto& polygon:polygons)if(polygon.contour.area()>area){area=polygon.contour.area();largest=polygon;}return largest;
}
static void fiber_smooth(ExPolygons& polygons,double minimum)
{
    for(auto& polygon:polygons) {
        const Points points=polygon.contour.points;if(points.size()<=2)continue;
        Points result{points.front()};
        for(size_t i=1;i+1<points.size();++i){const Line before(points[i],points[i-1]),after(points[i],points[i+1]);
            if(before.length()==0 || after.length()==0){result.push_back(points[i]);continue;}
            double angle = after.atan2_() - before.atan2_();
            if (angle > M_PI) angle -= 2 * M_PI;
            if (angle < -M_PI) angle += 2 * M_PI;
            const double theta = std::abs(angle);
            // The second assignment in the legacy recipe intentionally replaces the first.
            const double length=std::min(after.length()/(theta<M_PI*0.5?1.:3.),minimum);
            result.emplace_back(points[i]+(before.vector()*(length/before.length())));
            result.emplace_back(points[i]+(after.vector()*(length/after.length())));
        }
        result.push_back(points.back());polygon.contour.points=std::move(result);
    }
}
FiberConfig resolve_fiber_config(const LayerRegion& layer,bool perimeter)
{
    FiberConfig out;out.region=layer.region().config();const auto& print=*layer.layer()->object()->print();const auto& config=print.config();
    out.material=(perimeter?out.region.reinforced_perimeters_filament.value:out.region.reinforced_infill_filament.value)-1;
    out.resin_material=out.region.internal_solid_filament_id.value-1;
    const auto maps=print.get_filament_maps();
    if(out.material>=maps.size() || out.resin_material>=maps.size() || maps[out.material]<=0 || maps[out.resin_material]<=0)throw std::runtime_error("Fiber material reference or manual tool mapping is invalid");
    out.tool=maps[out.material]-1;out.resin_tool=maps[out.resin_material]-1;
    if(out.tool>=config.nozzle_diameter.values.size() || out.resin_tool>=config.nozzle_diameter.values.size())throw std::runtime_error("Fiber physical tool reference is invalid");
    out.nozzle_mm=config.nozzle_diameter.get_at(out.tool);out.resin_nozzle_mm=config.nozzle_diameter.get_at(out.resin_tool);out.height_mm=layer.layer()->height;
    out.width_mm=Flow::new_from_config_width(frInfill,perimeter?out.region.reinforced_perimeters_extrusion_width:out.region.reinforced_infill_extrusion_width,out.nozzle_mm,out.height_mm).width();
    out.perimeter_width_mm=Flow::new_from_config_width(frPerimeter,out.region.reinforced_perimeters_extrusion_width,out.nozzle_mm,out.height_mm).width();
    out.resin_width_mm=Flow::new_from_config_width(frSolidInfill,out.region.internal_solid_infill_line_width,out.resin_nozzle_mm,out.height_mm).width();
    out.flow_ratio=config.filament_flow_ratio.get_at(out.material);return out;
}
static void prepare_legacy_regions(PrintObject& object)
{
    // Work on each region separately.
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++ region_id)
    {
        const PrintRegion &region = object.printing_region(region_id);

        const bool generate_reinforced = fiber_active(region.config());
        if (!generate_reinforced) continue;
        // Support internal solid infill when sparse_infill_density is 100%
        const bool          use_solid_infill = fabs(region.config().sparse_infill_density.value - 100.) < EPSILON;
        const SurfaceType   surface_type     = use_solid_infill ? stInternalSolid : stInternal;
        const InfillPattern infill_pattern   = use_solid_infill ? region.config().internal_solid_infill_pattern :
                                                                  region.config().sparse_infill_pattern;

        std::vector<size_t> combine(object.layers().size(), 1);
        // loop through layers to which we have assigned layers to combine
        for (size_t layer_idx = 0; layer_idx < object.layers().size(); ++ layer_idx)
        {
            fiber_throw_if_canceled(*object.print());
            size_t num_layers = combine[layer_idx];
			if (num_layers < 1)
                continue;

            if(generate_reinforced && layer_idx%region.config().fiber_layer_height_ratio!=0)
                continue;
            // Get all the LayerRegion objects to be combined.
            std::vector<LayerRegion*> layerms;
            layerms.reserve(num_layers);
			for (size_t i = layer_idx + 1 - num_layers; i <= layer_idx; ++ i)
                layerms.emplace_back(object.layers()[i]->regions()[region_id]);
            // We need to perform a multi-layer intersection, so let's split it in pairs.
            // Initialize the intersection with the candidates of the lowest layer.
            ExPolygons intersection = to_expolygons(layerms.front()->fill_surfaces.filter_by_type(layer_idx<region.config().bottom_shell_layers.getInt()?stInternal:surface_type));
            // Start looping from the second layer and intersect the current intersection with it.
                for (size_t i = 1; i < layerms.size(); ++ i)
                    intersection = intersection_ex(layerms[i]->fill_surfaces.filter_by_type(surface_type), intersection);
                double area_threshold = layerms.front()->infill_area_threshold();
                if (! intersection.empty() && area_threshold > 0.)
                    intersection.erase(std::remove_if(intersection.begin(), intersection.end(),
                        [area_threshold](const ExPolygon &expoly) { return expoly.area() <= area_threshold; }),
                        intersection.end());

            if (intersection.empty())
                continue;

//            Slic3r::debugf "  combining %d %s regions from layers %d-%d\n",
//                scalar(@$intersection),
//                ($type == stInternal ? 'internal' : 'internal-solid'),
//                $layer_idx-($every-1), $layer_idx;
            // intersection now contains the regions that can be combined across the full amount of layers,
            // so let's remove those areas from all layers.
            Polygons intersection_with_clearance;
            intersection_with_clearance.reserve(intersection.size());
            float clearance_offset =
                0.5f * layerms.back()->flow(frPerimeter).scaled_width() +
             // Because fill areas for rectilinear and honeycomb are grown
             // later to overlap perimeters, we need to counteract that too.
                ((infill_pattern == ipRectilinear   ||
                  infill_pattern == ipMonotonic     ||
                  infill_pattern == ipGrid          ||
                  infill_pattern == ipLateralLattice     ||
                  infill_pattern == ipLine          ||
                  infill_pattern == ipHoneycomb     ||
                  infill_pattern == ipLateralHoneycomb) ? 1.5f : 0.5f) *
                    layerms.back()->flow(frSolidInfill).scaled_width();
            for (ExPolygon &expoly : intersection)
                polygons_append(intersection_with_clearance, offset(expoly, clearance_offset));

            bool offseted = false;
            for (LayerRegion *layerm : layerms)
            {
                Polygons internal = to_polygons(std::move(layerm->fill_surfaces.filter_by_type(surface_type)));
                layerm->fill_surfaces.remove_type(surface_type);
                layerm->fill_surfaces.append(diff_ex(internal, intersection_with_clearance), surface_type);

                if (layerm == layerms.back())
                {
                    // Apply surfaces back with adjusted depth to the uppermost layer.
                    Surface templ(surface_type, ExPolygon());
                    templ.thickness = 0.;
                    for (LayerRegion *layerm2 : layerms)
                        templ.thickness += layerm2->layer()->height;
                    templ.thickness_layers = (unsigned short)layerms.size();

                    float offsetinfill = region.config().fiber_offset_infill_ratio;
                    float offsetperimeters = region.config().fiber_offset_perimeters_ratio;
                    int realcount = 0;
                    auto minfiberlength = scale_(region.config().fibercut_length
                                                + region.config().fiber_start_length
                                                + region.config().fiber_z_hop
                                                + region.config().fiber_tension_length);

                    const FiberConfig resolved = resolve_fiber_config(*layerm, true);
                    Flow fiber_flow(resolved.perimeter_width_mm, resolved.height_mm, resolved.nozzle_mm);
                    coord_t      fiber_spacing = fiber_flow.scaled_spacing();
                    float min_perimeter_infill_spacing = float(fiber_spacing) * 1.05f;

                    if(region.config().generate_reinforced_perimeters||
                        region.config().reinforced_infill_pattern == InfillPattern::ipConcentric&&region.config().generate_reinforced_infills)
                    {
                        float ratio = 0.5;
                        float offsetfp = 0.5;
                        templ.surface_type = stFiberPerimeter;
                        auto fpp = offset_ex(intersection,
                                        - scale_(resolved.perimeter_width_mm) * offsetperimeters,
                                        jtMiter, 2.0);
                        auto cap           = diff_ex(intersection,offset_ex(intersection,
                                                               -1.5*scale_(resolved.perimeter_width_mm) * offsetperimeters,jtMiter, 2.0));
                        layerm->fill_surfaces.append(cap, stInternalSolid);
                        if(! fpp.empty())
                        {
                            realcount++;
                            auto  tempfpp = diff_ex(
                                            offset_ex(intersection,-scale_(0*region.config().sparse_infill_line_width)),
                                            offset_ex(fpp,scale_((offsetfp)*resolved.perimeter_width_mm)));
                            if(! tempfpp.empty())
                            {
                                fpp = offset_ex(fpp,scale_(-ratio*resolved.perimeter_width_mm));
                                fiber_smooth(fpp,scale_(ratio*resolved.perimeter_width_mm));
                                fpp = offset_ex(fpp,scale_(ratio*resolved.perimeter_width_mm ));

                            }

                            if(! fpp.empty()
                                && (fiber_largest_contour(fpp).contour.length()> minfiberlength )
                                && (fiber_largest_contour(fpp).contour.area()> (minfiberlength/4.0f * minfiberlength/4.0f ))
                                )
                            {
                                tempfpp = diff_ex(
                                            offset_ex(intersection,-scale_(0*region.config().sparse_infill_line_width)),
                                            offset_ex(fpp,scale_((offsetfp)*resolved.perimeter_width_mm)));
                                if(! tempfpp.empty())
                                {
                                    ExPolygons ffpp;
                                    for(auto onepp:tempfpp)
                                    {
                                        if (onepp.contour.length() <= minfiberlength*1.2)
                                            ffpp.emplace_back(onepp);
                                    }
                                    if(! ffpp.empty())
                                        layerm->fill_surfaces.append(ffpp,stInternalSolid);
                                }

                                layerm->fill_surfaces.append(fpp, templ);
                            #if 0
                                {
                                    BoundingBox bbox_svg;
                                    bbox_svg.merge(get_extents(intersection));
                                    {
                                        std::stringstream stri;
                                        stri << "stFiberPerimeter"<<layer_idx<<"_" << realcount << ".svg";
                                        SVG svg(stri.str(), bbox_svg);
                                        svg.draw(to_polylines(intersection), "blue", unscaled(0.5));
                                        svg.draw(to_polylines(fpp), "red",unscaled(0.5) );
                                        svg.Close();
                                    }
                                }
                            #endif
                                intersection = fpp;

                            }
                            else
                            {
                                layerm->fill_surfaces.append(intersection, stInternalSolid);
                                continue;
                            }
                        }
                        else
                        {
                            layerm->fill_surfaces.append(intersection, stInternalSolid);
                            continue;
                        }

                        if(offseted == false)
                        {
                            int count = std::max(region.config().outer_reinforced_perimeters_counts,
                                            region.config().inner_reinforced_perimeters_counts);
                            if (region.config().reinforced_infill_pattern == InfillPattern::ipConcentric &&
                                region.config().generate_reinforced_infills) {
                                count = 999;
                            }
                            if(count>0)
                            {
                                auto last = offset2_ex(intersection,
                                    -scale_(resolved.perimeter_width_mm+
                                        ratio*resolved.perimeter_width_mm),
                                    scale_(ratio*resolved.perimeter_width_mm), jtMiter, 2.0);


                                if (!last.empty() && count>1)
                                {
                                    realcount++;
                                    layerm->fill_surfaces.append(diff_ex(
                                            offset_ex(intersection,scale_((-0.5-offsetfp)*resolved.perimeter_width_mm)),
                                            offset_ex(last,scale_((0.5-offsetfp)*resolved.perimeter_width_mm))),
                                             stInternalSolid);
                                    ExPolygons ex;
                                    while (! last.empty() && realcount < count)
                                    {
                                        fiber_throw_if_canceled(*object.print());
                                        //int width = resolved.perimeter_width_mm;
                                        auto templast = offset2_ex(last,
                                            -scale_(resolved.perimeter_width_mm+
                                            ratio*resolved.perimeter_width_mm),
                                        scale_(ratio*resolved.perimeter_width_mm), jtMiter, 2.0);

                                        if(! templast.empty())
                                        {
                                            realcount++;
                                            ex = diff_ex(
                                                offset_ex(last,scale_((-0.5-offsetfp)*resolved.perimeter_width_mm)),
                                                offset_ex(templast,scale_((0.5-offsetfp)*resolved.perimeter_width_mm)));
                                            layerm->fill_surfaces.append(ex, stInternalSolid);
                                            last = templast;
                                        }else
                                            last = templast;

                                    }

                                }
                                else
                                {
                                    last = intersection;
                                }


                                if(realcount>0)
                                    intersection = last;
                            }
                        }

                    }


                    if (region.config().generate_reinforced_infills &&region.config().reinforced_infill_pattern !=InfillPattern::ipConcentric)
                    {
                        templ.surface_type = stInternalFiber;
                        if (region.config().generate_reinforced_perimeters)
                            intersection = offset_ex(intersection,
                                - scale_(resolved.perimeter_width_mm) * offsetinfill);
                    } else{
                        templ.surface_type = stInternal;
                        if(region.config().generate_reinforced_perimeters && realcount>0)
                        {
                            intersection = offset_ex(intersection,
                                                     -scale_(resolved.perimeter_width_mm * offsetinfill));

                           /* auto infillsection = offset_ex(intersection,
                            - scale_(resolved.perimeter_width_mm) * 2.5);

                            layerm->fill_surfaces.append(diff_ex(
                                            offset_ex(intersection,-scale_(0.8*resolved.perimeter_width_mm)),
                                            infillsection), stInternalSolid);

                            intersection = infillsection;  */

                            templ.surface_type     = stInternal;
                            templ.thickness = layerm->layer()->height;
                            templ.thickness_layers = 1;
                            for (LayerRegion *templayerm : layerms)
                            {
                                templayerm->fill_surfaces.append(intersection, stInternal);
                            }
                            continue;
                        }
                    }
                    layerm->fill_surfaces.append(intersection, templ);
                }
                else
                {
                    // Save void surfaces.
                    layerm->fill_surfaces.append(
                        intersection_ex(internal, intersection_with_clearance),
                        stInternalVoid);
                }
            }
        }
    }
}

bool fiber_config_key(const std::string& key)
{
    static const std::set<std::string> keys={"cut_fiber_gcode","filament_is_ccf","generate_reinforced_perimeters","outer_reinforced_perimeters_counts","inner_reinforced_perimeters_counts","generate_reinforced_infills","reinforced_infill_density","reinforced_infill_pattern","reinforced_infill_filament","reinforced_perimeters_filament","fiber_travel_max_length","fiber_offset_infill_ratio","fiber_internal_offset_infill_ratio","fiber_offset_perimeters_ratio","fiber_layer_height_ratio","fibercut_length","fiber_restart_extra_length","fiber_restart_speed","fiber_z_hop","fiber_z_down_speed","fiber_travel_speed","fiber_z_hop_pause_adhesion","fiber_tension_length","fiber_angle_extend_ratio","fiber_start_length","fiber_infill_arc_ratio","fiber_infill_length_ratio","fiber_corner_overshoot","fiber_corner_trim_length","fiber_perimeters_length_ratio","fiber_start_min_length","fiber_end_min_length","fiber_middle_min_length","fiber_slow_length","fiber_finish_ironing_distance","fiber_start_max_speed","fiber_start_min_speed","fiber_start_min_limit_speed","fiber_normal_max_speed","fiber_normal_min_speed","fiber_normal_min_limit_speed","fiber_finish_max_speed","fiber_finish_min_speed","fiber_finish_min_limit_speed","reinforced_perimeters_extrusion_width","reinforced_infill_extrusion_width","fiber_infill_acceleration","fiber_perimeter_acceleration"};
    return keys.count(key)!=0;
}
// Model-owned layer strategies and ordinary attachment materials are independent
// of the regional fiber recipe, but share its fixed dual-tool boundary.
static void validate_fiber_model_scope(const Print& print)
{
    int resin = -1;
    for (const PrintObject* object : print.objects())
        for (size_t r = 0; r < object->num_printing_regions(); ++r)
            if (fiber_active(object->printing_region(r).config())) {
                resin = object->printing_region(r).config().inner_wall_filament_id.value;
                break;
            }
    auto fail = [](const std::string& key) {
        throw std::runtime_error("Continuous fiber configuration conflict: " + key);
    };
    for (const PrintObject* object : print.objects()) {
        if (!object->model_object()->layer_height_profile.empty() ||
            !object->model_object()->layer_config_ranges.empty())
            fail("model variable layer height / layer_config_ranges");
        if (object->config().enable_support.value) {
            for (const char* key : {"support_filament", "support_interface_filament"}) {
                const int material = object->config().option(key)->getInt();
                if (material != 0 && material != resin) fail(std::string(key) + " must use the source resin");
            }
        }
        for (size_t r = 0; r < object->num_printing_regions(); ++r) {
            const auto& c = object->printing_region(r).config();
            for (int material : {c.inner_wall_filament_id.value, c.outer_wall_filament_id.value,
                 c.sparse_infill_filament_id.value, c.internal_solid_filament_id.value,
                 c.top_surface_filament_id.value, c.bottom_surface_filament_id.value})
                if (material != resin) fail("ordinary region material differs from the common source resin");
        }
    }
}
void validate_fiber_configuration(const Print& print)
{
    if(!fiber_active(print))return;
    validate_fiber_model_scope(print);
    const auto& config=print.config();
    auto fail=[](const std::string& key){throw std::runtime_error("Continuous fiber configuration conflict: "+key);};
    if(config.print_sequence.value==PrintSequence::ByObject)fail("print_sequence=by_object");
    if(config.spiral_mode.value)fail("spiral_mode");
    if(config.printer_structure.value==psBelt)fail("printer_structure=belt");
    if(const auto* option=config.option("use_volumetric_e"))if(option->serialize()=="1")fail("use_volumetric_e");
    if(const auto* option=config.option("post_process"))if(!option->serialize().empty())fail("post_process");
    if(const auto* option=config.option("gcode_substitutions"))if(!option->serialize().empty())fail("gcode_substitutions");
    std::set<int> fibers,resins;
    const auto maps=print.get_filament_maps();
    for(const auto* object:print.objects()) {
        if(object->config().raft_layers.value>0)fail("raft_layers");
        for(size_t r=0;r<object->num_printing_regions();++r){const auto& c=object->printing_region(r).config();if(!fiber_active(c))continue;
            if(c.fiber_layer_height_ratio.value<=0)fail("fiber_layer_height_ratio");
            if(c.generate_reinforced_infills.value && c.reinforced_infill_pattern.value!=ipRectilinear &&
               c.reinforced_infill_pattern.value!=ipConcentric)
                fail("reinforced_infill_pattern");
            if(c.generate_reinforced_infills.value)fibers.insert(c.reinforced_infill_filament.value);
            if(c.generate_reinforced_perimeters.value)fibers.insert(c.reinforced_perimeters_filament.value);
            for(int material:{c.inner_wall_filament_id.value,c.outer_wall_filament_id.value,c.sparse_infill_filament_id.value,c.internal_solid_filament_id.value,c.top_surface_filament_id.value,c.bottom_surface_filament_id.value})resins.insert(material);
            for(const std::string& key:c.keys())if(fiber_config_key(key)){const auto* option=c.option(key);if(option->type()==coFloat || option->type()==coFloatOrPercent || option->type()==coPercent){double value=option->getFloat();if(!std::isfinite(value)||value<0)fail(key);}}
        }
    }
    if(fibers.size()!=1 || resins.size()!=1)fail("one resin and one common fiber material are required");
    int fiber=*fibers.begin()-1,resin=*resins.begin()-1;
    if(fiber<0 || resin<0 || size_t(fiber)>=maps.size() || size_t(resin)>=maps.size())fail("material reference");
    if(maps[fiber]<=0 || maps[resin]<=0 || maps[fiber]==maps[resin])fail("fiber and resin need distinct physical tools");
    if(size_t(maps[fiber])>config.nozzle_diameter.values.size() || size_t(maps[resin])>config.nozzle_diameter.values.size())fail("physical tool reference");
}
void prepare_fiber_regions(PrintObject& object)
{
    size_t object_index=0;for(const auto* other:object.print()->objects()){if(other==&object)break;++object_index;}
    for(auto* layer:object.layers())for(auto* region:layer->regions()) {
        region->fiber_recipes.clear();region->fiber_resin_requests.clear();region->fiber_paths.clear();region->fiber_repairs.clear();
        region->fiber_original_surfaces=region->fill_surfaces;
    }
    prepare_legacy_regions(object);
    for(size_t l=0;l<object.layers().size();++l)for(size_t r=0;r<object.layers()[l]->regions().size();++r) {
        auto* region=object.layers()[l]->regions()[r];if(!fiber_active(region->region().config()))continue;
        SurfaceCollection ordinary;
        for(const auto& surface:region->fill_surfaces.surfaces){
            if(surface.surface_type!=stInternalFiber && surface.surface_type!=stFiberPerimeter){ordinary.surfaces.push_back(surface);continue;}
            FiberRegionRecipe recipe;recipe.candidate=surface;
            recipe.source.object=object_index;recipe.source.region=r;recipe.source.layer=l;
            recipe.source.recipe_ordinal=region->fiber_recipes.size();
            recipe.source.island=std::numeric_limits<size_t>::max();
            double largest_island_overlap=0;
            for(size_t island=0;island<object.layers()[l]->lslices.size();++island){
                const auto overlap=intersection_ex(ExPolygons{surface.expolygon},ExPolygons{object.layers()[l]->lslices[island]});
                double area=0;for(const auto& part:overlap)area+=part.area();
                if(area>largest_island_overlap){largest_island_overlap=area;recipe.source.island=island;}
            }
            for(size_t s=0;s<region->fiber_original_surfaces.surfaces.size();++s)if(!intersection_ex(ExPolygons{surface.expolygon},ExPolygons{region->fiber_original_surfaces.surfaces[s].expolygon}).empty()){recipe.original=region->fiber_original_surfaces.surfaces[s];recipe.source.surface=s;break;}
            recipe.source_is_fiber_perimeter=surface.surface_type==stFiberPerimeter;
            recipe.purpose=recipe.source_is_fiber_perimeter?FiberPurpose::Perimeter:FiberPurpose::Infill;
            if(region->region().config().generate_reinforced_infills.value && region->region().config().reinforced_infill_pattern.value==ipConcentric)recipe.purpose=FiberPurpose::ConcentricAll;
            recipe.config=resolve_fiber_config(*region,recipe.purpose!=FiberPurpose::Infill);
            recipe.fill_bounding_box=object.bounding_box();
            recipe.no_overlap_expolygons=region->fill_no_overlap_expolygons;
            recipe.print_config=&object.print()->config();
            recipe.print_object_config=&object.config();
            recipe.print_z=object.layers()[l]->print_z;
            recipe.resolution=object.print()->config().resolution.value;
            recipe.dont_alternate_fill_direction=region->region().config().zaa_enabled.value && region->region().config().zaa_dont_alternate_fill_direction.value;
            if(region->region().config().align_infill_direction_to_model.value){const auto matrix=object.trafo().matrix();recipe.align_angle_rad=std::atan2(double(matrix(1,0)),double(matrix(0,0)));}
            recipe.anchor_length=float(region->region().config().infill_anchor);
            if(region->region().config().infill_anchor.percent)recipe.anchor_length=float(recipe.anchor_length*0.01*recipe.config.width_mm);
            recipe.anchor_length_max=float(region->region().config().infill_anchor_max);
            if(region->region().config().infill_anchor_max.percent)recipe.anchor_length_max=float(recipe.anchor_length_max*0.01*recipe.config.width_mm);
            recipe.anchor_length=std::min(recipe.anchor_length,recipe.anchor_length_max);
            region->fiber_recipes.push_back(std::move(recipe));
        }
        // Preserve every region-preparation addition as a Base request with its original source.
        for(const auto& surface:ordinary.surfaces)if(surface.surface_type==stInternalSolid){
            FiberSource source;source.object=object_index;source.region=r;source.layer=l;
            source.island=std::numeric_limits<size_t>::max();
            ResinRequest request;request.source_ids={source};request.region=r;request.phase=FiberPhase::ResinBase;
            auto config=resolve_fiber_config(*region,true);request.material=config.resin_material;request.fill_recipe=Flow(config.resin_width_mm,config.height_mm,config.resin_nozzle_mm);
            request.area={surface.expolygon};request.reason="region cap / inter-loop band / small corner / vanished offset";region->fiber_resin_requests.push_back(std::move(request));
        }
        region->fill_surfaces=std::move(ordinary);
    }
}
static size_t fiber_extreme(const Points& points,size_t ordinal)
{
    size_t selected=0;for(size_t i=1;i<points.size();++i){bool use=false;switch(ordinal%4){case 0:use=points[i].y()<points[selected].y();break;case 1:use=points[i].x()>points[selected].x();break;case 2:use=points[i].y()>points[selected].y();break;case 3:use=points[i].x()<points[selected].x();break;}if(use)selected=i;}return selected;
}
static Polylines fiber_concentric(const FiberRegionRecipe& recipe,double seam_gap_mm,ExPolygons& other,const std::function<void()>& cancel)
{
    const auto& config=recipe.config;const double w=scale_(config.perimeter_width_mm),minimum=scale_(config.length_budget_mm());
    const auto& expolygon=recipe.candidate.expolygon;
    if(expolygon.area()<=minimum*minimum/16 || expolygon.contour.length()<=minimum){other.push_back(expolygon);return {};}
    const int max_count=recipe.purpose==FiberPurpose::ConcentricAll?666:std::max(config.region.outer_reinforced_perimeters_counts.value,config.region.inner_reinforced_perimeters_counts.value);
    int count=max_count-1;Polygons loops=to_polygons(expolygon);ExPolygons last{expolygon};
    while(!last.empty() && count>0){cancel();const ExPolygons previous=last;last=offset2_ex(last,-1.5*w,0.5*w,jtMiter,2.0);
        if(last.empty()){append(other,offset_ex(previous,(config.region.fiber_internal_offset_infill_ratio.value-1)*w));break;}
        ExPolygons retained;
        for(const auto& candidate:last){if(candidate.area()>minimum*minimum/16 && candidate.contour.length()>minimum)retained.push_back(candidate);
            else{auto fallback=offset_ex(candidate,config.region.fiber_internal_offset_infill_ratio.value*w);
                for(const auto& parent:previous){auto intersection=intersection_ex(ExPolygons{parent},ExPolygons{candidate});
                    const bool contained=parent.contour.bounding_box().contains(candidate.contour.bounding_box()) && parent.contains(candidate.contour.centroid()) && parent.contains(candidate.contour.first_point()) && !intersection.empty() && intersection.front().area()>=candidate.area()*0.99;
                    if(contained && parent.area()/candidate.area()>3){auto cap=offset_ex(parent,(config.region.fiber_internal_offset_infill_ratio.value-1)*w);if(!cap.empty() && cap.front().area()<=minimum*minimum/16 && cap.front().contour.length()<=minimum)fallback=std::move(cap);break;}}
                append(other,fallback);}}
        last=std::move(retained);append(loops,to_polygons(last));--count;
    }
    other=union_ex(other);loops=union_pt_chained_outside_in(loops);Polylines paths;Point exit(0,0);
    for(const auto& loop:loops){cancel();if(loop.points.empty())continue;const size_t index=paths.empty()?fiber_extreme(loop.points,recipe.source.recipe_ordinal):size_t(exit.nearest_point_index(loop.points));paths.push_back(loop.split_at_index(index));exit=paths.back().last_point();}
    for(auto& path:paths)path.clip_end(scale_(seam_gap_mm));
    paths.erase(std::remove_if(paths.begin(),paths.end(),[](const Polyline& p){return !p.is_valid();}),paths.end());
    if(max_count<=1 || paths.size()==1)return paths;
    Polylines joined;std::vector<bool> eligible(paths.size(),false);
    for(size_t i=0;i<paths.size();++i)if(paths[i].length()>2*w)eligible[i]=true;else joined.push_back(paths[i]);
    bool pushed=false;for(size_t i=0;i<paths.size();++i){if(!eligible[i])continue;if(!pushed){joined.push_back(paths[i]);pushed=true;continue;}
        auto& previous=joined.back();const double distance=(previous.last_point()-paths[i].first_point()).cast<double>().norm()/w;
        if(distance<0.7)continue;if(distance<2){previous.clip_end(w);paths[i].clip_start(w);previous.append(paths[i]);}else joined.push_back(paths[i]);}
    return joined;
}
static ResinRequest fiber_repair_request(const FiberRegionRecipe& recipe,Polyline rejected,const std::string& reason)
{
    ResinRequest request;request.source_ids={recipe.source};request.region=recipe.source.region;request.material=recipe.config.resin_material;request.phase=FiberPhase::ResinRepair;
    request.fill_recipe=Flow(recipe.config.resin_width_mm,recipe.config.height_mm,recipe.config.resin_nozzle_mm);request.reason=reason;
    const double radius=scale_(recipe.config.region.reinforced_infill_extrusion_width.get_abs_value(recipe.config.height_mm))/2;
    const double resin=scale_(recipe.config.resin_width_mm)/2;
    // A rejected fragment shorter than both endpoint clearances is printed as
    // resin in full, matching the legacy fallback. Clipping both ends would
    // erase the path and make Polyline::clip_end access an empty vector.
    if(rejected.length()>4*radius) {
        rejected.clip_start(2*radius);
        rejected.clip_end(2*radius);
    }
    if(rejected.is_valid())request.repair_paths.push_back(std::move(rejected));
    request.repair_radius=coord_t(radius);request.repair_gap=coord_t(resin);
    // Polyline::angle(0) in the fixed source returns pi. Fill then adds pi/2;
    // the resulting line orientation is equivalent to the legacy repair fill.
    request.angle_rad=M_PI;return request;
}
FiberPathResult plan_fiber_paths(const FiberRegionRecipe& recipe,double angle_rad,bool fixed_angle,size_t layer_id,double seam_gap_mm,const std::function<void()>& cancel)
{
    FiberPathResult result;ExPolygons pattern_other;
    if(recipe.purpose!=FiberPurpose::Infill)result.candidates=fiber_concentric(recipe,seam_gap_mm,pattern_other,cancel);
    else if(recipe.config.region.reinforced_infill_density.value>0){
        const InfillPattern pattern=recipe.config.region.reinforced_infill_pattern.value;
        std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));filler->set_bounding_box(recipe.fill_bounding_box);
        filler->spacing=recipe.config.width_mm;filler->layer_id=layer_id;filler->z=recipe.print_z;
        filler->angle=angle_rad+recipe.align_angle_rad;
        if(pattern==ipRectilinear && !fixed_angle)
            filler->angle+=(layer_id/std::max<unsigned>(1,recipe.candidate.thickness_layers)%4)*M_PI/4;
        filler->fixed_angle=pattern==ipRectilinear || fixed_angle;filler->loop_clipping=scale_(seam_gap_mm);
        filler->link_max_length=recipe.config.region.reinforced_infill_density.value>80?scale_(3*filler->spacing):0;
        filler->dont_alternate_fill_direction=recipe.dont_alternate_fill_direction;
        filler->print_config=recipe.print_config;filler->print_object_config=recipe.print_object_config;
        filler->no_overlap_expolygons=intersection_ex(recipe.no_overlap_expolygons,ExPolygons{recipe.candidate.expolygon},ApplySafetyOffset::Yes);
        FillParams params;params.density=recipe.config.region.reinforced_infill_density.value*0.01;params.config=&recipe.config.region;params.dont_adjust=false;params.flow=Flow(recipe.config.width_mm,recipe.config.height_mm,recipe.config.nozzle_mm);
        params.anchor_length=recipe.anchor_length;params.anchor_length_max=recipe.anchor_length_max;params.resolution=recipe.resolution;
        params.layer_height=recipe.config.height_mm;params.multiline=1;params.extrusion_role=erInternalInfill;params.using_internal_flow=true;params.pattern=pattern;
        result.candidates=filler->fill_surface(&recipe.candidate,params);
    }
    // prepare_fiber_fill_sources owns _polygon_other because it must reserve
    // the surrounding ordinary surfaces before Orca groups their fills.
    auto reject=[&](const Polyline& path,const std::string& reason){
        try {
            result.rejected.push_back(path);result.rejection_reasons.push_back(reason);result.resin_requests.push_back(fiber_repair_request(recipe,path,reason));
        } catch(const std::exception& error) {
            throw std::runtime_error("Fiber repair candidate="+std::to_string(result.rejected.size())+" reason="+reason+": "+error.what());
        }
    };
    for(size_t c=0;c<result.candidates.size();++c){cancel();Polyline candidate=result.candidates[c];Polylines retained;
        auto filter=[&](int type,Polylines& output) {
            try { return filter_fiber_legacy(candidate,recipe.config.region.fiber_start_min_length.value,type,output); }
            catch(const std::exception& error) { throw std::runtime_error("Fiber filter type="+std::to_string(type)+" candidate="+std::to_string(c)+": "+error.what()); }
        };
        if(recipe.purpose==FiberPurpose::Infill){if(candidate.length()<=scale_(recipe.config.length_budget_mm())){reject(candidate,"internal initial L <= Lmin");continue;}
            candidate.simplify(scale_(0.1));
            for(int type:{0,2}) {
                Polylines ignored;
                for(const auto& path:filter(type,ignored))
                    reject(path,type==0?"legacy start filter":"legacy end filter");
                if(candidate.points.size()<2 || candidate.length()<scale_(recipe.config.length_budget_mm())) {
                    if(candidate.is_valid())
                        reject(candidate,type==0?"post-start L < Lmin":"post-end L < Lmin");
                    candidate.points.clear();
                    break;
                }
            }
            if(candidate.points.size()<2)continue;
            for(const auto& path:filter(1,retained))reject(path,"legacy middle filter");
        }else retained.push_back(candidate);
        for(size_t n=0;n<retained.size();++n){if(recipe.purpose==FiberPurpose::Infill && retained[n].length()<=scale_(recipe.config.length_budget_mm())){reject(retained[n],"final internal L <= Lmin");continue;}
            FiberSource source=recipe.source;source.candidate=c*100000+n;std::string rejection;
            auto prepared=prepare_fiber_path(retained[n],recipe.config,source,recipe.purpose,recipe.source_is_fiber_perimeter,recipe.purpose!=FiberPurpose::Infill,rejection);
            if(prepared)result.retained.push_back(std::move(prepared));else reject(retained[n],rejection);}
    }
    // Legacy contour ordering examines the first two chains before committing their order.
    if(result.retained.size()>1 && recipe.purpose!=FiberPurpose::Infill && result.retained[0]->length_mm>result.retained[1]->length_mm)std::reverse(result.retained.begin(),result.retained.end());
    return result;
}
extern double calculate_infill_rotation_angle(const PrintObject*,size_t,const double&,const std::string&);
static void fiber_fill_resin(ResinRequest& request,ExtrusionEntityCollection& out)
{
    if(request.area.empty())return;
    if(request.requires_explicit_fill && ++request.explicit_fill_count!=1)throw std::runtime_error("Duplicate explicit fiber resin request consumer: "+request.request_id);
    std::unique_ptr<Fill> fill(Fill::new_from_type(ipRectilinear));fill->spacing=request.fill_recipe.spacing();fill->angle=request.angle_rad;fill->fixed_angle=true;fill->set_bounding_box(get_extents(request.area));
    FillParams params;params.density=request.density;params.flow=request.fill_recipe;params.extrusion_role=erSolidInfill;
    auto collection=std::make_unique<ExtrusionEntityCollection>();
    for(const auto& polygon:request.area){Surface surface(stInternalSolid,polygon);auto paths=fill->fill_surface(&surface,params);extrusion_entities_append_paths(collection->entities,std::move(paths),erSolidInfill,request.fill_recipe.mm3_per_mm(),request.fill_recipe.width(),request.fill_recipe.height());}
    if(!collection->entities.empty())out.entities.push_back(collection.release());
}
void make_fiber_fills(Layer& layer)
{
    struct Pending {ExtrusionEntityCollection fiber,repair,base;std::vector<ResinRequest> requests;};
    std::vector<Pending> pending(layer.regions().size());
    auto cancel=[&](){fiber_throw_if_canceled(*layer.object()->print());};
    ExPolygons retained_coverage;std::vector<ResinRequest> repair_inputs;
    for(size_t r=0;r<layer.regions().size();++r){const auto* region=layer.regions()[r];auto& output=pending[r];
        for(const auto& recipe:region->fiber_recipes){cancel();const auto& config=recipe.config.region;
            const double angle=calculate_infill_rotation_angle(layer.object(),layer.id(),config.solid_infill_direction.value,config.solid_infill_rotate_template.value);
            auto result=plan_fiber_paths(recipe,angle,!config.solid_infill_rotate_template.value.empty(),layer.id(),config.seam_gap.get_abs_value(recipe.config.nozzle_mm),cancel);
            for(const auto& path:result.retained){auto entity=std::make_unique<ExtrusionFiberPath>(path);append(retained_coverage,union_ex(entity->polygons_covered_by_width()));output.fiber.entities.push_back(entity.release());}
            append(repair_inputs,std::move(result.resin_requests));
        }
    }
    struct RepairBatch {ResinRequest request;size_t island=0,fallback_region=0,rejected_count=0;};
    std::vector<RepairBatch> batches;
    for(auto& input:repair_inputs){cancel();
        const size_t island=input.source_ids.empty()?std::numeric_limits<size_t>::max():input.source_ids.front().island;
        const size_t fallback=island==std::numeric_limits<size_t>::max()?input.region:std::numeric_limits<size_t>::max();
        auto compatible=[&](const RepairBatch& batch){return batch.island==island && batch.fallback_region==fallback &&
            batch.request.material==input.material && batch.request.fill_recipe==input.fill_recipe &&
            batch.request.repair_radius==input.repair_radius && batch.request.repair_gap==input.repair_gap &&
            std::abs(batch.request.angle_rad-input.angle_rad)<=EPSILON;};
        auto found=std::find_if(batches.begin(),batches.end(),compatible);
        if(found==batches.end()){
            RepairBatch batch;batch.request=input;batch.request.source_ids.clear();batch.request.repair_paths.clear();
            batch.island=island;batch.fallback_region=fallback;batches.push_back(std::move(batch));found=std::prev(batches.end());
        }
        append(found->request.source_ids,std::move(input.source_ids));
        found->rejected_count+=input.repair_paths.size();append(found->request.repair_paths,std::move(input.repair_paths));
    }
    for(size_t index=0;index<batches.size();++index){cancel();auto& grouped=batches[index];auto& batch=grouped.request;
        Polygons expanded;for(const auto& path:batch.repair_paths)append(expanded,offset(path,batch.repair_radius+batch.repair_gap));
        if(!expanded.empty())batch.area=offset_ex(union_ex(expanded),-batch.repair_gap);
        batch.original_area=batch.area;batch.area=diff_ex(batch.area,retained_coverage);
        batch.reason="batched rejected fiber paths in one layer island (count="+std::to_string(grouped.rejected_count)+")";
        batch.request_id=(batch.source_ids.empty()?std::to_string(layer.id()):batch.source_ids.front().id())+":repair-batch:"+std::to_string(index);
        batch.requires_explicit_fill=!batch.area.empty();
        auto& output=pending.at(batch.region);fiber_fill_resin(batch,output.repair);output.requests.push_back(std::move(batch));
    }
    cancel();
    for(size_t r=0;r<layer.regions().size();++r){auto* region=layer.regions()[r];region->fiber_paths=std::move(pending[r].fiber);region->fiber_paths.no_sort=true;region->fiber_repairs=std::move(pending[r].repair);
        append(region->fiber_resin_requests,std::move(pending[r].requests));
        auto& base=pending[r].base;region->fills.entities.insert(region->fills.entities.end(),base.entities.begin(),base.entities.end());base.entities.clear();}
}
std::vector<FiberVisit> fiber_visits(const Layer& layer,size_t instance)
{
    std::vector<FiberVisit> visits;
    if (!fiber_active(*layer.object()->print())) return visits;
    size_t object_index=0;
    for(const auto* object:layer.object()->print()->objects()){if(object==layer.object())break;++object_index;}
    for(FiberPhase phase:{FiberPhase::ResinBase,FiberPhase::Fiber,FiberPhase::ResinRepair}){
        FiberVisit visit;visit.object=object_index;visit.instance=instance;visit.phase=phase;visit.visit_index=visits.size();
        for(size_t r=0;r<layer.regions().size();++r){const auto* region=layer.regions()[r];if(phase != FiberPhase::ResinBase && !fiber_active(region->region().config()))continue;
            const auto& roots=phase==FiberPhase::Fiber?region->fiber_paths:phase==FiberPhase::ResinRepair?region->fiber_repairs:region->fills;
            if(roots.empty() && !(phase==FiberPhase::ResinBase && (!region->perimeters.empty() || !region->fiber_paths.empty() || !region->fiber_repairs.empty())))continue;
            const auto config=resolve_fiber_config(*region,region->region().config().generate_reinforced_perimeters.value);visit.material=phase==FiberPhase::Fiber?config.material:config.resin_material;visit.tool=phase==FiberPhase::Fiber?config.tool:config.resin_tool;visit.root_ids.push_back(r);
        }
        if(!visit.root_ids.empty())visits.push_back(std::move(visit));
    }return visits;
}
}

namespace Slic3r {
// Legacy thick callback and ccf_gap_fill ownership migrated out of mutable Fill callbacks.
void prepare_fiber_fill_sources(Layer& layer)
{
    for (auto* region : layer.regions()) {
        if (region->fiber_recipes.empty()) continue;
        ExPolygons thick_coverage, pattern_other;
        auto cancel = [&]() { fiber_throw_if_canceled(*layer.object()->print()); };
        for (const auto& recipe : region->fiber_recipes) {
            if (recipe.purpose == FiberPurpose::Infill) continue;
            ExPolygons other;
            const auto paths = fiber_concentric(recipe, recipe.config.region.seam_gap.get_abs_value(recipe.config.nozzle_mm), other, cancel);
            // to_thick_polylines in old FillConcentricCorners used min_spacing as width.front().
            const double first_width = scale_(Flow(recipe.config.width_mm, recipe.config.height_mm, recipe.config.nozzle_mm).spacing());
            for (const auto& path : paths) append(thick_coverage, union_ex(offset(path, 1.5 * first_width)));
            append(pattern_other, other);
        }
        SurfaceCollection updated;
        const auto config = resolve_fiber_config(*region, true);
        // At this boundary requests contain the region-stage solid caps. The
        // old diff_gap_surface callback changes these obligations along with
        // the actual solid surfaces, reserving the thick fiber pattern.
        for (auto& request : region->fiber_resin_requests) {
            if (request.phase != FiberPhase::ResinBase) continue;
            request.original_area = request.area;
            request.area = diff_ex(request.area, thick_coverage);
        }
        for (const auto& surface : region->fill_surfaces.surfaces) {
            ExPolygons area{surface.expolygon};
            if (surface.surface_type == stInternalSolid) {
                area = diff_ex(area, thick_coverage);
            }
            const bool mergeable = surface.surface_type == stInternal || surface.is_external() || surface.surface_type == stInternalSolid;
            if (!pattern_other.empty() && mergeable && !intersection_ex(pattern_other, area).empty()) {
                append(pattern_other, area);
            } else updated.append(area, surface);
        }
        if (!pattern_other.empty()) {
            pattern_other=union_ex(pattern_other);
            // The merged ccf_gap_fill becomes the sole owner of every base
            // obligation it absorbs. Keep the older diagnostic requests only
            // for the portions that remain in Orca's ordinary fill surfaces.
            for(auto& existing:region->fiber_resin_requests)if(existing.phase==FiberPhase::ResinBase)
                existing.area=diff_ex(existing.area,pattern_other);
            ResinRequest request;request.source_ids={region->fiber_recipes.front().source};request.region=request.source_ids.front().region;
            request.material=config.resin_material;request.phase=FiberPhase::ResinBase;request.area=pattern_other;
            request.fill_recipe=Flow::new_from_config_width(frSolidInfill,config.region.sparse_infill_line_width,config.resin_nozzle_mm,config.height_mm);
            request.reason="ccf_gap_fill union of _polygon_other and intersecting internal/external/gap surfaces";
            // ccf_gap_fill creates its own rectilinear Fill with default effective angle (+pi/2).
            request.angle_rad=0;request.density=1;
            request.request_id=request.source_ids.front().id()+":ccf-gap";request.requires_explicit_fill=true;
            fiber_fill_resin(request,region->fills);
            region->fiber_resin_requests.push_back(std::move(request));
        }
        region->fill_surfaces=std::move(updated);
    }
}
}

namespace Slic3r {
void check_fiber_resin_coverage(const Layer& layer)
{
    if(std::none_of(layer.regions().begin(),layer.regions().end(),
        [](const LayerRegion* region){return fiber_active(region->region().config());}))return;
    // Orca group_fills merges equal fill recipes across regions and publishes
    // them under the first region. Continuous-fiber validation guarantees one common resin
    // for all ordinary regions. Check deposited geometry at that layer scope,
    // retaining the request's original region identity for diagnostics.
    Polygons base_paths,repair_paths;
    for(const auto* region:layer.regions()) {
        append(base_paths,region->fills.polygons_covered_by_width());
        append(repair_paths,region->fiber_repairs.polygons_covered_by_width());
    }
    const ExPolygons base=union_ex(base_paths),repair=union_ex(repair_paths);
    std::set<std::string> explicit_request_ids;
    for(const auto* region:layer.regions()) {
        if(!fiber_active(region->region().config()))continue;
        for(const auto& request:region->fiber_resin_requests){
            if(request.requires_explicit_fill){
                if(request.request_id.empty() || !explicit_request_ids.insert(request.request_id).second)
                    throw std::runtime_error("Duplicate or unidentified explicit fiber resin request");
                if(request.explicit_fill_count!=1)
                    throw std::runtime_error("Explicit fiber resin request must have exactly one consumer: "+request.request_id);
            }
            if(request.area.empty())continue;
            const auto& coverage=request.phase==FiberPhase::ResinRepair?repair:base;
            // Legacy rectilinear filling may produce no centerline for a
            // line-width-scale fragment. Keep that compatibility behavior,
            // while still rejecting a macroscopic omission below.
            const auto missing=diff_ex(request.area,coverage);
            if(!offset_ex(missing,-scale_(request.fill_recipe.width())).empty())throw std::runtime_error("Macroscopic uncovered fiber resin request: "+request.reason+
                " (layer="+std::to_string(layer.id())+", region="+std::to_string(request.region)+")");
        }
    }
}
}

#include "GCode/GCodeProcessor.hpp"
namespace Slic3r {
void audit_fiber_final_file(const Print& print,const std::string& filename)
{
    if(!fiber_active(print))return;
    struct Expected {const PreparedFiberPath* path;Point offset;size_t starts=0,cuts=0;bool saw_position=false;Vec3f last_position=Vec3f::Zero();};
    std::map<std::pair<std::string,size_t>,Expected> expected;
    for(const auto* object:print.objects())for(const auto* layer:object->layers())for(const auto* region:layer->regions())
        for(const auto* entity:region->fiber_paths.entities){const auto& path=*static_cast<const ExtrusionFiberPath*>(entity)->prepared;
            for(size_t instance=0;instance<object->instances().size();++instance)if(!expected.emplace(std::make_pair(path.source.id(),instance),Expected{&path,object->instances()[instance].shift}).second)
                throw std::runtime_error("Fiber plan contains a duplicate path occurrence identity");}
    size_t begin_markers=0,finish_markers=0,end_markers=0;std::ifstream raw(filename);std::string line;
    if(!raw)throw std::runtime_error("Unable to open final G-code for fiber audit");
    enum class RawFiberState { Outside, Begun, Started, Cut, Tail, Finished };
    RawFiberState raw_state=RawFiberState::Outside;
    while(std::getline(raw,line)){
        if(line.rfind(";FIBER_BEGIN",0)==0){if(raw_state!=RawFiberState::Outside)throw std::runtime_error("Nested or incomplete final fiber block");raw_state=RawFiberState::Begun;++begin_markers;}
        else if(line.rfind(";FIBER_START",0)==0){if(raw_state!=RawFiberState::Begun)throw std::runtime_error("Final fiber Start is out of sequence");raw_state=RawFiberState::Started;}
        else if(line.rfind(";FIBER_CUT",0)==0){if(raw_state!=RawFiberState::Started)throw std::runtime_error("Final fiber Cut is out of sequence");raw_state=RawFiberState::Cut;}
        else if(line.rfind(";FIBER_TAIL",0)==0){if(raw_state!=RawFiberState::Cut)throw std::runtime_error("Final fiber Tail is out of sequence");raw_state=RawFiberState::Tail;}
        else if(line.rfind(";FIBER_FINISH",0)==0){if(raw_state!=RawFiberState::Cut && raw_state!=RawFiberState::Tail)throw std::runtime_error("Final fiber Finish is out of sequence");raw_state=RawFiberState::Finished;++finish_markers;}
        else if(line.rfind(";FIBER_END",0)==0){if(raw_state!=RawFiberState::Finished)throw std::runtime_error("Final fiber End is out of sequence");raw_state=RawFiberState::Outside;++end_markers;}
    }
    if(raw_state!=RawFiberState::Outside)throw std::runtime_error("Final fiber block is incomplete");
    if(begin_markers!=expected.size() || finish_markers!=expected.size() || end_markers!=expected.size())
        throw std::runtime_error("Each final fiber occurrence requires exactly one Begin, Finish, and End boundary");
    GCodeProcessor processor;processor.init_filament_maps_and_nozzle_type_when_import_only_gcode();
    processor.process_file(filename,[&](){fiber_throw_if_canceled(print);});
    for(const auto& move:processor.get_result().moves){if(move.fiber_path_id.empty())continue;
        auto found=expected.find({move.fiber_path_id,move.fiber_instance});
        if(found==expected.end())throw std::runtime_error("Final G-code contains an unknown fiber occurrence");
        auto& item=found->second;item.saw_position=true;item.last_position=move.position;
        if(move.fiber_event==1)++item.starts;
        if(move.fiber_event==2){++item.cuts;const Point point=item.path->segments[item.path->cut_index-1].end+item.offset;
            const Vec2d nominal=point.cast<double>()*SCALING_FACTOR;
            if((move.position.head<2>().cast<double>()-nominal).norm()>0.0015)throw std::runtime_error("Final fiber Cut is not at the analytical cut point");}
        if(move.fiber_tail && std::abs(move.delta_extruder)>0.00001)throw std::runtime_error("Final fiber tail contains E");
        if(move.fiber_deposition && (!std::isfinite(move.feedrate)||move.feedrate<=0))throw std::runtime_error("Final fiber feedrate is invalid");
        if(move.fiber_deposition && move.extruder_id!=item.path->config.material)throw std::runtime_error("Final fiber material is incorrect");
    }
    for(const auto& pair:expected){const auto& item=pair.second;
        if(item.starts!=1||item.cuts!=1)throw std::runtime_error("Each final fiber occurrence requires exactly one Start and one Cut");
        const Vec2d nominal=(item.path->exit+item.offset).cast<double>()*SCALING_FACTOR;
        if(!item.saw_position || (item.last_position.head<2>().cast<double>()-nominal).norm()>0.0015)
            throw std::runtime_error("Final fiber occurrence does not end at its planned exit point");
    }
}
}
