// Persistent, validated application preferences. UI edits a draft until OK.
#pragma once
#include "core/Params.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <thread>
namespace strata::ui::prefs {
inline json defaults() {
    return json::object(
        {{"autosave_idle", 5.0},
         {"autosave_force", 30.0},
         {"autosave_filename", "${settings}/autosave/${project}-${hash}.weld"},
         {"path_substitutions", json::array()},
         {"suppress_crash_report", false},
         {"channel_warning", 1023},
         {"color_mode", "Welder"},
         {"ocio_config", ""},
         {"working_space", "linear"},
         {"viewer_transform", "sRGB"},
         {"thumbnail_transform", "sRGB"},
         {"monitor_transform", "rec709"},
         {"integer_transform", "sRGB"},
         {"integer16_transform", "sRGB"},
         {"log_transform", "Cineon"},
         {"float_transform", "linear"},
         {"ocio_roles", true},
         {"ocio_roles_first", true},
         {"views", json::array({{{"name", "main"}, {"color", {1., 1., 1., 1.}}}})},
         {"hero_view", "main"},
         {"view_colors", false},
         {"cache_dir", ""},
         {"disk_cache_gb", 20.0},
         {"paint_cache_gb", 10.0},
         {"playback_cache_gb", 0.0},
         {"comp_cache_percent", 50.0},
         {"playback_cache_percent", 50.0},
         {"paint_cache_percent", 60.0},
         {"pause_background_cache", false},
         {"clear_background_cache", false},
         {"undo_size", 100},
         {"undo_min", 4},
         {"expression_mode", "Lazy"},
         {"gpu_texture_mb", 256},
         {"gpu_enabled", true},
         {"gpu_cache_disabled", false},
         {"gpu_cache_percent", 40.0},
         {"localization_mode", "off"},
         {"localization_policy", "from auto-localize path"},
         {"localization_source", ""},
         {"localization_dir", ""},
         {"localization_limit_gb", 10.0},
         {"localization_check_minutes", 30},
         {"localization_read_source", false},
         {"localization_hide_progress", false},
         {"localization_pause", false},
         {"frame_server", false},
         {"frame_server_timeout", 1.0},
         {"focus_background_renders", true},
         {"frame_server_processes", std::max(1u, std::thread::hardware_concurrency() / 2)},
         {"render_limits", "limit renderer (more responsive ui)"},
         {"documentation_source", "local"},
         {"documentation_host", "https://github.com/anishsreedhar/welder/tree/main/docs"},
         {"documentation_port", 443},
         {"frame_number_style", "Hashes (#)"},
         {"fixed_frame_width", true},
         {"alembic_all_in_one", false},
         {"merge_connect_a", true},
         {"autokey_roto", true},
         {"viewer_delete_on_close", false},
         {"viewer_double_click_settings", true},
         {"search_weighting", true},
         {"search_favorites", true},
         {"ofx_trial", false},
         {"menus_previous_item", true},
         {"script_default_tcl", false},
         {"startup_workspace", 1},
         {"splash_enabled", true},
         {"usage_statistics", false},
         {"check_updates", true},
         {"scale_mode", "auto"},
         {"ui_scale", 0.0},
         {"display_scales", json::array()},
         {"ui_font", "Verdana"},
         {"ui_font_size", 11.0},
         {"ui_font_bold", false},
         {"ui_font_italic", false},
         {"theme", "graphite"},
         {"ui_background", {.196, .196, .196, 1.}},
         {"ui_base", {.157, .157, .157, 1.}},
         {"ui_highlight", {.98, .616, .098, 1.}},
         {"ui_highlight_text", {0., 0., 0., 1.}},
         {"ui_label", {.804, .804, .804, 1.}},
         {"ui_button", {.255, .255, .255, 1.}},
         {"animated_color", {.29, .46, .6, 1.}},
         {"keyframe_color", {.2, .5, .94, 1.}},
         {"disk_color", {.98, .616, .098, 1.}},
         {"ram_color", {.8, .8, .8, 1.}},
         {"playhead_color", {.98, .616, .098, 1.}},
         {"inout_color", {1., .3, .1, 1.}},
         {"curves_visible", 100},
         {"curve_background", {.07, .08, .09, 1.}},
         {"curve_unselected", {.6, .6, .6, 1.}},
         {"curve_part_selected", {.6, .5, .35, 1.}},
         {"curve_selected", {.98, .616, .098, 1.}},
         {"curve_timeline", {0., 0., 0., 1.}},
         {"curve_text", {.8, .8, .8, 1.}},
         {"curve_shadow", {0., 0., 0., 1.}},
         {"curve_time_label", {.25, .5, .3, 1.}},
         {"curve_current", {.98, .616, .098, 1.}},
         {"curve_project", {.7, .7, .7, 1.}},
         {"max_panels", 10},
         {"reopen_new_panel", false},
         {"panels_match_selection", false},
         {"input_button_action", "select input node only"},
         {"channel_menu_limit", 100},
         {"color_picker_mode", "in-panel color picker"},
         {"browser_recent_dir", true},
         {"node_autocolor", true},
         {"node_shade", true},
         {"node_colors", json::array({{{"match", "3D"}, {"color", {.5, .05, .05, 1.}}},
                                      {{"match", "Image Read Viewer"}, {"color", {195. / 255, 195. / 255, 195. / 255, 1.}}},
                                      {{"match", "Cache"}, {"color", {0., 1., 1., 1.}}},
                                      {{"match", "Deep"}, {"color", {0., 0., .3, 1.}}},
                                      {{"match", "Roto RotoPaint"}, {"color", {.4, .75, .4, 1.}}},
                                      {{"match", "Draw"}, {"color", {.7, .7, .7, 1.}}},
                                      {{"match", "Color"}, {"color", {125. / 255, 158. / 255, 207. / 255, 1.}}},
                                      {{"match", "Time"}, {"color", {.6, .65, .35, 1.}}},
                                      {{"match", "Channel"}, {"color", {166. / 255, 53. / 255, 101. / 255, 1.}}},
                                      {{"match", "Merge"}, {"color", {78. / 255, 83. / 255, 190. / 255, 1.}}},
                                      {{"match", "Keyer"}, {"color", {94. / 255, 179. / 255, 98. / 255, 1.}}},
                                      {{"match", "Write"}, {"color", {180. / 255, 190. / 255, 0., 1.}}},
                                      {{"match", "Filter"}, {"color", {204. / 255, 122. / 255, 66. / 255, 1.}}},
                                      {{"match", "OCIO"}, {"color", {.1, .65, .6, 1.}}},
                                      {{"match", "Transform"}, {"color", {147. / 255, 99. / 255, 151. / 255, 1.}}},
                                      {{"match", ""}, {"color", {.7, .7, .7, 1.}}},
                                      {{"match", ""}, {"color", {.7, .7, .7, 1.}}}})},
         {"node_other", {.55, .55, .55, 1.}},
         {"node_text", {0., 0., 0., 1.}},
         {"node_selected", {1., .73, .36, 1.}},
         {"node_input_selected", {1., .73, .36, 1.}},
         {"node_gl", {.7, .7, .7, 1.}},
         {"graph_autolabel", true},
         {"graph_highlight_running", true},
         {"stamp_mode", "Current frame"},
         {"name_background", .6},
         {"graph_font", "Verdana"},
         {"graph_bold", false},
         {"graph_italic", false},
         {"tile_width", 100.0},
         {"tile_height", 22.0},
         {"snap_node", true},
         {"grid_width", 110.0},
         {"grid_height", 24.0},
         {"snap_grid", false},
         {"show_grid", false},
         {"snap_threshold", 7.0},
         {"graph_color", {.196, .196, .196, 1.}},
         {"overlay_color", {.75, .75, .75, 1.}},
         {"elbow_color", {1., 1., 0., 1.}},
         {"bbox_warning", false},
         {"bbox_threshold", 10.0},
         {"bbox_highlight", {1., 0., 0., 1.}},
         {"bbox_line", {0., 0., 0., 1.}},
         {"arrow_left", {0., 0., 0., 1.}},
         {"arrow_right", {0., 0., 0., 1.}},
         {"arrow_up", {0., 0., 0., 1.}},
         {"arrow_down", {0., 0., 0., 1.}},
         {"deep_arrow", {0., 0., 0., 1.}},
         {"expression_arrow", {.5, 1., .5, 1.}},
         {"link_arrow", {.7, .7, .7, 1.}},
         {"clone_arrow", {.9, .4, .25, 1.}},
         {"expression_arrows", true},
         {"link_arrows", true},
         {"clone_arrows", true},
         {"top_arrow_length", 30.0},
         {"side_arrow_length", 4.0},
         {"output_arrow_length", 8.0},
         {"arrow_head_length", 9.0},
         {"arrow_head_width", 9.0},
         {"arrow_width", 2.0},
         {"pick_arrow_heads", true},
         {"pick_elbows", true},
         {"insert_near_middle", false},
         {"dot_size", 1.0},
         {"shake_disconnect", true},
         {"shake_sensitivity", 3.0},
         {"right_angle_pipes", false},
         {"middle_drag_zooms", false},
         {"alt_drag_cuts", false},
         {"scope_black", 0.0},
         {"scope_white", 1.0},
         {"scope_encoding", "REC 709"},
         {"scope_viewer_transform", true},
         {"scope_full_frame", false},
         {"script_font", "Courier New"},
         {"script_font_size", 10.0},
         {"script_indent", 4},
         {"script_history", true},
         {"script_echo", false},
         {"script_clear_success", false},
         {"python_interpreter", ""},
         {"python_run_init", true},
         {"viewer_renderer", "GL"},
         {"viewer_own_window", false},
         {"viewer_prevent_autozoom", false},
         {"viewer_lut_rgb_only", true},
         {"viewer_bit_depth", "float"},
         {"viewer_gpu", true},
         {"input_gpu", true},
         {"viewer_disable_dither", false},
         {"dither_algorithm", "high frequency"},
         {"viewer_no_incomplete_stereo", false},
         {"viewer_stereo_warning", true},
         {"viewer_flip_stereo", false},
         {"viewer_texture_size", "512x512"},
         {"viewer_texture_mode", "Multiframe"},
         {"flipbook_default", "Flipbook Viewer"},
         {"flipbook_playback", "Play All Frames"},
         {"flipbook_guides", false},
         {"flipbook_display", 0},
         {"flipbook_missing", true},
         {"flipbook_background", "black"},
         {"flipbook_gray", 75.0},
         {"flipbook_checker", 127.0},
         {"flipbook_increment", 12},
         {"flipbook_filter", "Auto"},
         {"audio_latency", 0},
         {"audio_volume", 50.0},
         {"monitor_legal", true},
         {"monitor_topmost", true},
         {"monitor_hmd", false},
         {"monitor_global", false},
         {"monitor_background", "Black"},
         {"monitor_gray", 75.0},
         {"monitor_checker", 127.0},
         {"monitor_ndi", false},
         {"handle_2d_bg", {0., 0., 0., 1.}},
         {"handle_2d_fg", {.75, .75, .75, 1.}},
         {"handle_3d_bg", {0., 0., 0., 1.}},
         {"handle_3d_fg", {.75, .75, .75, 1.}},
         {"handle_sel", {.2, .5, 1., 1.}},
         {"handle_min_sel", {0., 0., 0., 1.}},
         {"spline_width", 1.0},
         {"spline_shadow", false},
         {"handle_expression", {1., 0., 0., 1.}},
         {"handle_focus", {0., 1., 0., 1.}},
         {"roto_points", {1., .8, .1, 1.}},
         {"roto_curves", {1., 0., 0., 1.}},
         {"roto_transform", {.6, .6, .6, 1.}},
         {"roto_locked", {.8, .8, .8, 1.}},
         {"warp_a", {1., 0., 0., 1.}},
         {"warp_b", {0., 0., 1., 1.}},
         {"warp_a_dest", {1., .5, .5, 1.}},
         {"warp_b_dest", {.4, .5, 1., 1.}},
         {"warp_a_stipple", false},
         {"warp_b_stipple", false},
         {"warp_a_dest_stipple", false},
         {"warp_b_dest_stipple", false},
         {"warp_correspondence", {.7, .7, .2, 1.}},
         {"warp_boundary", {.8, .5, .2, 1.}},
         {"warp_hard_boundary", {1., 1., 1., 1.}},
         {"handle_middle_pan", true},
         {"handle_combo_zoom", true},
         {"transform_preview", true},
         {"hotkeys_3d", "QWER (Katana/Maya)"},
         {"navigation_3d", "Welder"},
         {"handle_transform_size", 1.0},
         {"handle_3d_size", 5.0},
         {"handle_3d_max", 10.0},
         {"handle_2d_size", 5.0},
         {"handle_2d_max", 20.0},
         {"handle_icon_size", 50.0},
         {"handle_icon_scale", .1},
         {"object_speed", 1.0},
         {"camera_speed", 1.0},
         {"ask_match_project", true},
         {"unc_paths", true},
         {"roto_show_points", 0},
         {"mcp_enabled", false},
         {"mcp_port", 9471},
         {"mcp_remote", false}});
}
// Unsupported renderer/services stay visible in the reference layout but cannot
// silently save a setting which has no corresponding behavior.
inline bool supported(const std::string &key) {
    static const std::set<std::string> keys = {"autosave_idle",
                                               "autosave_force",
                                               "autosave_filename",
                                               "path_substitutions",
                                               "suppress_crash_report",
                                               "channel_warning",
                                               "color_mode",
                                               "ocio_config",
                                               "working_space",
                                               "viewer_transform",
                                               "integer_transform",
                                               "cache_dir",
                                               "disk_cache_gb",
                                               "playback_cache_gb",
                                               "pause_background_cache",
                                               "clear_background_cache",
                                               "undo_size",
                                               "gpu_enabled",
                                               "frame_server",
                                               "frame_server_processes",
                                               "render_limits",
                                               "documentation_source",
                                               "documentation_host",
                                               "merge_connect_a",
                                               "viewer_double_click_settings",
                                               "viewer_delete_on_close",
                                               "viewer_own_window",
                                               "frame_number_style",
                                               "startup_workspace",
                                               "splash_enabled",
                                               "check_updates",
                                               "scale_mode",
                                               "ui_scale",
                                               "ui_font",
                                               "ui_font_size",
                                               "ui_font_bold",
                                               "ui_font_italic",
                                               "ui_background",
                                               "ui_base",
                                               "ui_highlight",
                                               "ui_label",
                                               "ui_button",
                                               "ram_color",
                                               "arrow_left",
                                               "arrow_right",
                                               "arrow_up",
                                               "arrow_down",
                                               "disk_color",
                                               "keyframe_color",
                                               "curve_background",
                                               "curve_current",
                                               "curves_visible",
                                               "max_panels",
                                               "reopen_new_panel",
                                               "panels_match_selection",
                                               "browser_recent_dir",
                                               "node_autocolor",
                                               "node_shade",
                                               "node_colors",
                                               "node_other",
                                               "node_text",
                                               "node_selected",
                                               "stamp_mode",
                                               "graph_font",
                                               "tile_width",
                                               "tile_height",
                                               "snap_node",
                                               "show_grid",
                                               "grid_width",
                                               "snap_threshold",
                                               "graph_color",
                                               "arrow_head_length",
                                               "arrow_head_width",
                                               "arrow_width",
                                               "dot_size",
                                               "top_arrow_length",
                                               "shake_disconnect",
                                               "graph_bold",
                                               "graph_italic",
                                               "camera_speed",
                                               "graph_autolabel",
                                               "snap_grid",
                                               "grid_height",
                                               "shake_sensitivity",
                                               "side_arrow_length",
                                               "right_angle_pipes",
                                               "middle_drag_zooms",
                                               "roto_show_points",
                                               "alt_drag_cuts",
                                               "script_font",
                                               "script_font_size",
                                               "script_history",
                                               "script_echo",
                                               "script_clear_success",
                                               "python_interpreter",
                                               "python_run_init",
                                               "viewer_prevent_autozoom",
                                               "input_gpu",
                                               "flipbook_background",
                                               "handle_2d_fg",
                                               "handle_transform_size",
                                               "ask_match_project",
                                               "unc_paths",
                                               "mcp_enabled",
                                               "mcp_port",
                                               "mcp_remote"};
    return keys.count(key) != 0;
}
inline json &runtime() {
    static json value = defaults();
    return value;
}
inline json normalize(const json &input) {
    const json baseline = defaults();
    json out = baseline;
    if (!input.is_object())
        return out;
    for (auto &[key, value] : out.items()) {
        const auto i = input.find(key);
        if (i == input.end())
            continue;
        if (value.is_number() && i->is_number()) {
            const double n = i->get<double>();
            if (std::isfinite(n))
                value = value.is_number_integer() || value.is_number_unsigned() ? json((int)std::clamp(n, -1000000., 1000000.))
                                                                                : json(n);
        } else if (value.type() == i->type())
            value = *i;
    }
    auto clamp = [&](const char *k, double lo, double hi) {
        double n = std::clamp(out[k].get<double>(), lo, hi);
        if (out[k].is_number_integer() || out[k].is_number_unsigned())
            out[k] = (int)n;
        else
            out[k] = n;
    };
    clamp("autosave_idle", 0, 3600);
    clamp("autosave_force", 0, 3600);
    clamp("max_panels", 1, 100);
    clamp("undo_size", 1, 10000);
    clamp("ui_scale", 0, 3);
    clamp("ui_font_size", 8, 32);
    clamp("script_font_size", 8, 40);
    clamp("script_indent", 1, 16);
    clamp("disk_cache_gb", 0, 10000);
    clamp("playback_cache_gb", 0, 10000);
    clamp("frame_server_processes", 1, 1024);
    clamp("tile_width", 20, 600);
    clamp("tile_height", 12, 100);
    clamp("arrow_head_length", 1, 40);
    clamp("arrow_head_width", 1, 40);
    clamp("arrow_width", .5, 10);
    clamp("dot_size", .25, 4);
    clamp("snap_threshold", 0, 50);
    clamp("channel_warning", 1, 65535);
    clamp("scope_white", .001, 10000);
    clamp("scope_black", -10000, 9999);
    clamp("mcp_port", 1, 65535);
    for (auto &[k, v] : out.items())
        if (baseline[k].is_array() && baseline[k].size() == 4 && baseline[k][0].is_number()) {
            if (v.size() != 4 ||
                !std::all_of(v.begin(), v.end(), [](const json &n) { return n.is_number() && std::isfinite(n.get<double>()); }))
                v = baseline[k];
            else
                for (auto &n : v)
                    n = std::clamp(n.get<double>(), 0., 1.);
        }
    for (const char *k : {"path_substitutions", "views", "display_scales", "node_colors"}) {
        json clean = json::array();
        for (const auto &row : out[k])
            if (row.is_object())
                clean.push_back(row);
        out[k] = clean;
    }
    for (auto &row : out["path_substitutions"])
        for (const char *k : {"windows", "osx", "linux"})
            if (!row.contains(k) || !row[k].is_string())
                row[k] = "";
    for (auto &row : out["node_colors"]) {
        if (!row.contains("match") || !row["match"].is_string())
            row["match"] = "";
        auto &c = row["color"];
        if (!c.is_array() || c.size() != 4 ||
            !std::all_of(c.begin(), c.end(), [](const json &n) { return n.is_number() && std::isfinite(n.get<double>()); }))
            c = json::array({.5, .5, .5, 1.});
        else
            for (auto &n : c)
                n = std::clamp(n.get<double>(), 0., 1.);
    }
    if (out["views"].empty())
        out["views"] = baseline["views"];
    clamp("startup_workspace", 1, 6);
    clamp("grid_width", 1, 500);
    clamp("grid_height", 1, 500);
    clamp("curves_visible", 1, 10000);
    clamp("handle_transform_size", .5, 10);
    clamp("top_arrow_length", 0, 40);
    clamp("side_arrow_length", 0, 40);
    clamp("output_arrow_length", 0, 40);
    return out;
}
} // namespace strata::ui::prefs
