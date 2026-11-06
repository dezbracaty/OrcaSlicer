// Reference only. Fixed original Orca GUI; never compiled.
if (en_3mf_file_type == En3mfType::From_Prusa) {
    // do not reset the model config
    load_config = false;
    if (load_type != LoadType::LoadGeometry)
        log_and_show_3mf_info(msg_unsupported_geometry, load_3mf_title);
}
else if (en_3mf_file_type == En3mfType::From_Orca) {
    // OrcaSlicer file (has OrcaSlicer tag) - compare file_version with SoftFever_VERSION
    // Migration fix for OrcaSlicer 2.3.1-alpha sparse infill rotation template
    if (load_config && (file_version < app_version) && file_version == Semver("2.3.1-alpha")) {
        if (!config_loaded.opt_string("sparse_infill_rotate_template").empty()) {
            const auto _sparse_infill_pattern =
                config_loaded.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value;
            bool is_safe_to_rotate = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                     _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                     _sparse_infill_pattern == ipLockedZag;
            if (!is_safe_to_rotate) {
                wxString msg_text = _(
                    L("This project was created with an OrcaSlicer 2.3.1-alpha and uses "
                      "infill rotation template settings that may not work properly with your current infill pattern. "
                      "This could result in weak support or print quality issues."));
                msg_text += "\n\n" +
                            _(L("Would you like OrcaSlicer to automatically fix this by clearing the rotation template settings?"));
                MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
                dialog.SetButtonLabel(wxID_YES, _L("Yes"));
                dialog.SetButtonLabel(wxID_NO, _L("No"));
                if (dialog.ShowModal() == wxID_YES) {
                    config_loaded.opt_string("sparse_infill_rotate_template") = "";
                }
            }
        }
    } else if (load_config && (file_version > app_version)) {
        if (config_substitutions.unrecogized_keys.size() > 0) {
            wxString text  = wxString::Format(_L("The 3MF file version %s is newer than %s's version %s, found the following unrecognized keys:"),
                                             file_version.to_string_sf(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string_sf());
            text += "\n";
            wxString context = text;
            wxString append = _L("You should update your software.\n");
            context += "\n\n";
            context += append;
            log_and_show_3mf_info(context, newer_3mf_title);
        }
        else {
            //if the minor version is not matched
            if (file_version.min() != app_version.min()) {
                wxString text  = wxString::Format(_L("The 3MF file version %s is newer than %s's version %s, we suggest to upgrade your software."),
                                 file_version.to_string_sf(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string_sf());
                text += "\n";
                log_and_show_3mf_info(text, newer_3mf_title);
            }
        }
    }
    else if (load_config && config_loaded.empty()) {
        load_config = false;
        log_and_show_3mf_info(msg_old_orca_geometry, load_3mf_title);
    }
}
else if (en_3mf_file_type == En3mfType::From_BBS) {
    // No OrcaSlicer tag - check Bambu/Application version
    Semver orca_tag_start_version(2, 3, 2);
    if (file_version <= orca_tag_start_version) {
        // Compatible old version (before OrcaSlicer tagging was introduced after 2.3.2).
        // Any version prior or equal to 2.3.2 is older than the current one, no version warnings needed.
        // Still apply migration fixes for known old versions.
        if (load_config && (file_version == Semver("2.3.1-alpha"))) {
            if (!config_loaded.opt_string("sparse_infill_rotate_template").empty()) {
                const auto _sparse_infill_pattern =
                    config_loaded.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value;
                bool is_safe_to_rotate = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                         _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                         _sparse_infill_pattern == ipLockedZag;
                if (!is_safe_to_rotate) {
                    wxString msg_text = _(
                        L("This project was created with an OrcaSlicer 2.3.1-alpha and uses "
                          "infill rotation template settings that may not work properly with your current infill pattern. "
                          "This could result in weak support or print quality issues."));
                    msg_text += "\n\n" +
                                _(L("Would you like OrcaSlicer to automatically fix this by clearing the rotation template settings?"));
                    MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
                    dialog.SetButtonLabel(wxID_YES, _L("Yes"));
                    dialog.SetButtonLabel(wxID_NO, _L("No"));
                    if (dialog.ShowModal() == wxID_YES) {
                        config_loaded.opt_string("sparse_infill_rotate_template") = "";
                    }
                }
            }
        }
        else if (load_config && config_loaded.empty()) {
            load_config = false;
            log_and_show_3mf_info(msg_older_geometry, load_3mf_title);
        }
    } else {
        // BambuStudio project (version > 2.3.2 without OrcaSlicer tag)
        // Report that a BambuStudio project is being imported and compare with SLIC3R_VERSION
        Semver slic3r_version = *(Semver::parse(SLIC3R_VERSION));
        if (load_config && config_loaded.empty()) {
            load_config = false;
            log_and_show_3mf_info(msg_bambu_geometry, load_3mf_title);
        }
        else if (load_config && (file_version > slic3r_version)) {
            // BambuStudio file version is newer than our compatible SLIC3R_VERSION
            if (config_substitutions.unrecogized_keys.size() > 0) {
                wxString text  = wxString::Format(_L("The 3MF was created by BambuStudio (version %s), which is newer than the compatible version %s. Found unrecognized settings:"),
                                                 file_version.to_string(), slic3r_version.to_string());
                text += "\n";
                wxString context = text;
                wxString append = _L("You should update your software.\n");
                context += "\n\n";
                context += append;
                log_and_show_3mf_info(context, bambu_project_title);
            } else {
                wxString text  = wxString::Format(_L("The 3MF was created by BambuStudio (version %s), which is newer than the compatible version %s. Some settings may not be fully compatible."),
                                 file_version.to_string(), slic3r_version.to_string());
                text += "\n";
                log_and_show_3mf_info(text, bambu_project_title);
            }
        } else if (load_config) {
            // BambuStudio version is older or same as our SLIC3R_VERSION
            wxString text = _L("The 3MF was created by BambuStudio. Some settings may differ from OrcaSlicer.");
            log_and_show_3mf_info(text, bambu_project_title);
        }
    }
}
else if (en_3mf_file_type == En3mfType::From_Other) {
    // Generic CAD/other 3MF without slicer metadata: import geometry silently.
    if (load_config && config_loaded.empty()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__
                                << " "
                                << boost::format("3MF has no slicer metadata/project config, importing geometry only: %1%") % path.string();
        load_config = false;
    }
}
else if (load_config && config_loaded.empty()) {
    load_config = false;
    log_and_show_3mf_info(msg_old_orca_geometry, load_3mf_title);
}
else if (!load_config) {
    // reset config except color
    for (ModelObject *model_object : model.objects) {
        bool has_extruder = model_object->config.has("extruder");
        int  extruder_id  = -1;
        // save the extruder information before reset
        if (has_extruder) { extruder_id = model_object->config.extruder(); }

        model_object->config.reset();

        // restore the extruder after reset
        if (has_extruder) { model_object->config.set("extruder", extruder_id); }

        // Is there any modifier or advanced config data?
        for (ModelVolume *model_volume : model_object->volumes) {
            has_extruder = model_volume->config.has("extruder");
            if (has_extruder) { extruder_id = model_volume->config.extruder(); }

            model_volume->config.reset();

            if (has_extruder) { model_volume->config.set("extruder", extruder_id); }
        }
    }
}
