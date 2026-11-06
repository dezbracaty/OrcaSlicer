
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "../test_utils.hpp"

#include <boost/filesystem/operations.hpp>

#include <catch2/catch_tostring.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <fstream>
#include <iterator>
#include <type_traits> // for std::enable_if_t
#include <typeinfo>    // for typeid
#include <miniz.h>
#include <iomanip>

namespace Catch {
    template <typename T>
    struct is_eigen_matrix : std::is_base_of<Eigen::MatrixBase<T>, T> {};

    template <typename T>
    struct StringMaker<T, std::enable_if_t<is_eigen_matrix<T>::value>> {
        static std::string convert(const T& eigen_obj) {
            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            ss << "Matrix<" << typeid(eigen_obj).name() << "> = \n";
            ss << eigen_obj.format(fmt);
            return ss.str();
        }
    };
    
    // We must manually specialize for Eigen::Transform as it doesn't derive from MatrixBase.
    // It's defined as: Eigen::Transform<Scalar, Dim, Mode, Options>
    template <typename Scalar, int Dim, int Mode, int Options>
    struct StringMaker<Eigen::Transform<Scalar, Dim, Mode, Options>> {
        static std::string convert(const Eigen::Transform<Scalar, Dim, Mode, Options>& trafo) {
            // We print the underlying matrix 
            const auto& matrix = trafo.matrix();

            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            
            ss << "Transform<Mode=" << Mode << ", Dim=" << Dim << "> = \n"; 
            ss << matrix.format(fmt);
            return ss.str();
        }
    };
    
    // Quaternions also need an explicit specialization
    template <typename Scalar, int Options>
    struct StringMaker<Eigen::Quaternion<Scalar, Options>> {
        static std::string convert(const Eigen::Quaternion<Scalar, Options>& quat) {
            std::stringstream ss;
            ss << "Quaternion(w=" << quat.w() << ", x=" << quat.x() << ", y=" << quat.y() << ", z=" << quat.z() << ")";
            return ss.str();
        }
    };
} // end namespace Catch

#include <catch2/catch_all.hpp>

using namespace Slic3r;

TEST_CASE("Production mesh identity includes the model part and preserves shared transforms", "[3mf][production-cache]") {
    const bool mirror=GENERATE(false,true);CAPTURE(mirror);
    const auto path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("orcaslicer-production-mesh-cache-%%%%-%%%%.3mf");
    struct RemoveArchive { boost::filesystem::path path;~RemoveArchive(){boost::system::error_code error;boost::filesystem::remove(path,error);} } remove{path};
    const auto temporary_data=boost::filesystem::path(path.string()+".data");boost::filesystem::create_directories(temporary_data);
    struct RestoreTemporary { std::string previous;boost::filesystem::path path;~RestoreTemporary(){set_temporary_dir(previous);boost::system::error_code error;boost::filesystem::remove_all(path,error);} } restore{temporary_dir(),temporary_data};
    set_temporary_dir(temporary_data.string());
    const std::vector<Vec3d> first{{11,21,31},{15,21,31},{11,25,31},{11,21,35}};
    const std::vector<Vec3d> second{{51,61,71},{57,61,71},{51,69,71},{51,61,81}};
    const std::vector<Vec3i32> faces{{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    const auto mesh_xml=[&](int id,const std::vector<Vec3d>& vertices){
        std::ostringstream text;text<<std::setprecision(17);
        text<<"<model unit=\"millimeter\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><resources><object id=\""<<id<<"\" type=\"model\"><mesh><vertices>";
        for(const auto& v:vertices)text<<"<vertex x=\""<<v.x()<<"\" y=\""<<v.y()<<"\" z=\""<<v.z()<<"\"/>";
        text<<"</vertices><triangles>";
        for(std::size_t i=0;i<faces.size();++i){const auto& t=faces[i];text<<"<triangle v1=\""<<t[0]<<"\" v2=\""<<t[1]<<"\" v3=\""<<t[2]<<"\"";if(i==0)text<<" paint_supports=\"4\" paint_seam=\"4\" paint_color=\"4\" paint_fuzzy_skin=\"4\"";text<<"/>";}
        text<<"</triangles></mesh></object></resources></model>";return text.str();
    };
    Transform3d rotated=Transform3d::Identity();
    rotated.linear()<<0,-2,0,mirror?-1:1,0,0,0,0,1;rotated.translation()=Vec3d(100,200,300);
    const auto matrix_xml=[](const Transform3d& matrix){std::ostringstream text;text<<std::setprecision(17);for(int c=0;c<4;++c)for(int r=0;r<3;++r){if(c||r)text<<' ';text<<matrix(r,c);}return text.str();};
    std::ostringstream root;root<<R"xml(<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p"><metadata name="Application">BambuStudio-2.0.0</metadata><metadata name="BambuStudio:3mfVersion">1</metadata><resources>)xml";
    for(int id:{10,20,30,40})root<<"<object id=\""<<id<<"\" type=\"model\"><components><component p:path=\"/3D/Objects/"<<(id==20?'b':id==40?'c':'a')<<".model\" objectid=\""<<(id==40?7:1)<<"\" transform=\""<<matrix_xml(id==30?rotated:Transform3d::Identity())<<"\"/></components></object>";
    root<<"</resources><build>";for(int id:{10,20,30,40})root<<"<item objectid=\""<<id<<"\"/>";root<<"</build></model>";
    const std::string relationships=R"xml(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="root" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)xml";
    std::ostringstream children;children<<"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";for(char name:{'a','b','c'})children<<"<Relationship Target=\"/3D/Objects/"<<name<<".model\" Id=\""<<name<<"\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>";children<<"</Relationships>";
    const std::string metadata=R"xml(<config><object id="40"><part id="7" subtype="normal_part"><metadata key="mesh_shared" value="1"/></part></object></config>)xml";
    mz_zip_archive zip{};REQUIRE(mz_zip_writer_init_file(&zip,path.string().c_str(),0));
    const auto add=[&](const char* name,const std::string& text){REQUIRE(mz_zip_writer_add_mem(&zip,name,text.data(),text.size(),MZ_DEFAULT_COMPRESSION));};
    add("[Content_Types].xml",R"xml(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)xml");
    add("_rels/.rels",relationships);add("3D/_rels/3dmodel.model.rels",children.str());add("3D/3dmodel.model",root.str());
    add("3D/Objects/a.model",mesh_xml(1,first));add("3D/Objects/b.model",mesh_xml(1,second));add("3D/Objects/c.model",mesh_xml(7,first));add("Metadata/model_settings.config",metadata);
    REQUIRE(mz_zip_writer_finalize_archive(&zip));REQUIRE(mz_zip_writer_end(&zip));
    DynamicPrintConfig config;ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs plates;std::vector<Preset*> presets;Semver version;En3mfType type=En3mfType::From_Other;
    auto model=Model::read_from_archive(path.string(),&config,&substitutions,type,LoadStrategy::LoadModel|LoadStrategy::LoadConfig|LoadStrategy::AddDefaultInstances,&plates,&presets,&version);
    release_PlateData_list(plates);CHECK(presets.empty());REQUIRE(model.objects.size()==4);
    for(const auto* object:model.objects)REQUIRE(object->volumes.size()==1);
    CHECK(model.objects[0]->volumes.front()->mesh_ptr()!=model.objects[1]->volumes.front()->mesh_ptr());
    CHECK(model.objects[0]->volumes.front()->mesh_ptr()==model.objects[2]->volumes.front()->mesh_ptr());
    CHECK(model.objects[0]->volumes.front()->mesh_ptr()==model.objects[3]->volumes.front()->mesh_ptr());
    for(std::size_t i=0;i<model.objects.size();++i){
        const auto* object=model.objects[i];REQUIRE(object->volumes.size()==1);REQUIRE(object->instances.size()==1);const auto* volume=object->volumes.front();
        const auto world=object->instances.front()->get_matrix()*volume->get_matrix();const auto& source=i==1?second:first;
        REQUIRE(volume->mesh().its.indices.size()==faces.size());
        for(std::size_t t=0;t<faces.size();++t)for(int corner=0;corner<3;++corner){const auto index=volume->mesh().its.indices[t][corner];CHECK(index==faces[t][corner]);const Vec3d actual=world*volume->mesh().its.vertices[index].cast<double>();Vec3d expected=source[index];if(i==2)expected=rotated*expected;CHECK((actual-expected).norm()<1e-9);}
        CHECK(volume->supported_facets.get_triangle_as_string(0)=="4");CHECK(volume->seam_facets.get_triangle_as_string(0)=="4");
        CHECK(volume->mmu_segmentation_facets.get_triangle_as_string(0)=="4");CHECK(volume->fuzzy_skin_facets.get_triangle_as_string(0)=="4");
    }
}


SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
            std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
            DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("Slice 3mf file to G-code", "[3mf][gcode]") {
    GIVEN("a model loaded through 3mf") {
        boost::filesystem::path resources_dir =
            boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
        Slic3r::set_resources_dir(resources_dir.string());
        Slic3r::set_data_dir(boost::filesystem::temp_directory_path().string());

        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &src_model));
        REQUIRE(src_model.add_default_instances());

        ScopedTemporaryFile temp_3mf(".3mf");
        REQUIRE(store_3mf(temp_3mf.string().c_str(), &src_model, nullptr, false));

        Model model;
        DynamicPrintConfig loaded_config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
        REQUIRE(load_3mf(temp_3mf.string().c_str(), loaded_config, ctxt, &model, false));
        REQUIRE(model.add_default_instances());
        model.center_instances_around_point({ 100.0, 100.0 });

        DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
        full_config.apply(loaded_config, true);
        full_config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0"));

        WHEN("the model is applied to a Print and exported") {
            Print print;
            print.apply(model, full_config);

            StringObjectException warning;
            StringObjectException error = print.validate(&warning);
            CAPTURE(error.string);
            CAPTURE(warning.string);
            REQUIRE(error.string.empty());

            print.process();

            ScopedTemporaryFile temp_gcode(".gcode");
            GCodeProcessorResult result;
            std::string output_path = print.export_gcode(temp_gcode.string(), &result);

            THEN("a non-empty G-code file is generated") {
                REQUIRE(output_path == temp_gcode.string());
                REQUIRE(boost::filesystem::exists(temp_gcode.path()));
                REQUIRE(boost::filesystem::file_size(temp_gcode.path()) > 0);

                std::ifstream gcode(temp_gcode.string());
                REQUIRE(gcode.good());

                std::string content((std::istreambuf_iterator<char>(gcode)), std::istreambuf_iterator<char>());
                CHECK(content.find("G1") != std::string::npos);
                CHECK(content.find("; filament used") != std::string::npos);
            }
        }
    }
}

SCENARIO("2D convex hull of sinking object", "[3mf][.]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects[0];
            object->center_around_origin(false);

	    // This outputs the same exact data as the Prusaslicer test
	    object->volumes[0]->mesh().write_ascii("/tmp/orca.ascii");

            // set instance's attitude so that it is rotated, scaled (and sinking? how is it sinking? the rotation? does it matter if it's sinking?)
            ModelInstance* instance = object->instances[0];
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
	    auto trafo = instance->get_transformation().get_matrix();

	    // This matrix is the same exact matrix as the Prusaslicer test
	    CAPTURE(trafo);
            Polygon hull_2d = object->convex_hull_2d(trafo);

	    // But we get different hull_2d.points here (and somehow decimal numbers despite being int64_t values, but that's probabaly printing configuration somewhere -- Prusaslicer's prints out with newlines between the X&Y and not one between coordinates, which is about the worse possible output).
	    // I think it's something to do with PrusaSlicer ignoring everything under the Z plane, which makes sense from the results.
	    // See the comments added to ModelObject::convex_hull_2d for more information.

            // verify result
            Points result = {
                { -91501496, -15914144 },
                { 91501496, -15914144 },
                { 91501496, 4243 },
                { 78229680, 4246883 },
                { 56898100, 4246883 },
                { -85501496, 4242641 },
                { -91501496, 4243 }
            };

            THEN("2D convex hull should match with reference") {
                // Allow 1um error due to floating point rounding.
                bool res = hull_2d.points.size() == result.size();
                if (res) {
                    for (size_t i = 0; i < result.size(); ++ i) {
                        const Point &p1 = result[i];
                        const Point &p2 = hull_2d.points[i];
                        CHECK((std::abs(p1.x() - p2.x()) > 1 || std::abs(p1.y() - p2.y()) > 1));
                    }
                }

                CAPTURE(hull_2d.points);
                REQUIRE(res);
            }
        }
    }
}

TEST_CASE("Cloned painted volumes rescan their material cache", "[3mf][painting-cache]") {
    Model model;auto* object=model.add_object();
    auto* volume=object->add_volume(TriangleMesh(its_make_cube(10,10,10)));
    volume->mmu_segmentation_facets.set_triangle_from_string(0,"2C"); // material 5
    CHECK(volume->get_extruders()==std::vector<int>{5,1});
    Model explicit_clone;auto* cloned_object=explicit_clone.add_object();
    auto* cloned_volume=cloned_object->add_volume(*volume);
    CHECK(cloned_volume->get_extruders()==std::vector<int>{5,1});
    cloned_volume->mmu_segmentation_facets.reset();
    CHECK(cloned_volume->get_extruders()==std::vector<int>{1});
    for(int i=0;i<32;++i) {
        Model clone=model;auto* copied=clone.objects.front()->volumes.front();
        CHECK(copied->get_extruders()==std::vector<int>{5,1});
        CHECK(copied->get_extruders()==std::vector<int>{5,1});
        copied->mmu_segmentation_facets.reset();
        CHECK(copied->get_extruders()==std::vector<int>{1});
    }
    CHECK(volume->get_extruders()==std::vector<int>{5,1});
}

TEST_CASE("Facet forest validation precedes selector mutation", "[3mf][painting-validation]") {
    const TriangleMesh mesh(its_make_cube(10,10,10));
    TriangleSelector selector(mesh);
    TriangleSelector::TriangleSplittingData valid;
    valid.triangles_to_split.emplace_back(0,0);
    valid.bitstream={false,false,true,false}; // one unsplit enforcer face
    REQUIRE_NOTHROW(valid.validate(mesh.its.indices.size()));
    selector.deserialize(valid);
    const auto before=selector.serialize();
    CHECK(TriangleSelector::has_facets(valid,EnforcerBlockerType::ENFORCER));
    auto multiple=valid;
    multiple.triangles_to_split.emplace_back(1,4);
    multiple.bitstream.insert(multiple.bitstream.end(),valid.bitstream.begin(),valid.bitstream.end());
    CHECK_NOTHROW(multiple.validate(mesh.its.indices.size()));
    for(int split=1;split<=3;++split) {
        auto divided=valid;divided.bitstream.clear();
        const auto append=[&](int code){for(int bit=0;bit<4;++bit)divided.bitstream.push_back((code>>bit)&1);};
        append(split);append(4);
        for(int child=1;child<=split;++child)append(0);
        REQUIRE_NOTHROW(selector.deserialize(divided));
        CHECK(selector.serialize().bitstream==divided.bitstream);
    }
    auto extended=valid;extended.bitstream={false,false,true,true,false,true,false,false}; // state5
    REQUIRE_NOTHROW(selector.deserialize(extended,true,EnforcerBlockerType::Extruder4,EnforcerBlockerType::Extruder1));
    CHECK(selector.get_facets(EnforcerBlockerType::Extruder4).indices.size()==1);
    selector.deserialize(valid); // failure cases start from the same known state

    const int failure=GENERATE(0,1,2,3,4,5,6,7);
    CAPTURE(failure);
    auto incomplete=valid;
    switch(failure) {
    case 0: incomplete.bitstream.pop_back();break;
    case 1: incomplete.bitstream={true,false,false,false};break;
    case 2: incomplete.bitstream={true,false,true,true};break;
    case 3: incomplete.bitstream={false,false,true,true};break;
    case 4: incomplete.bitstream.insert(incomplete.bitstream.end(),4,false);break;
    case 5: incomplete.triangles_to_split.front().triangle_idx=-1;break;
    case 6: incomplete.triangles_to_split.front().bitstream_start_idx=4;break;
    case 7:
        incomplete.bitstream={true,true,true,false}; // three sides, invalid special side1
        incomplete.bitstream.insert(incomplete.bitstream.end(),16,false); // four complete children
        break;
    }
    CHECK_THROWS_AS(incomplete.validate(mesh.its.indices.size()),std::invalid_argument);
    CHECK_THROWS_AS(selector.deserialize(incomplete),std::invalid_argument);
    CHECK(selector.serialize()==before);
    CHECK_THROWS_AS(TriangleSelector::has_facets(incomplete,EnforcerBlockerType::ENFORCER),std::invalid_argument);
}
