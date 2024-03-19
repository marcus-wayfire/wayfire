#include <memory>
#include <wayfire/config/types.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>

#include "ipc-helpers.hpp"
#include "plugins/ipc/ipc-method-repository.hpp"
#include "tree-controller.hpp"
#include "tree.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/object.hpp"
#include "wayfire/option-wrapper.hpp"
#include "wayfire/plugin.hpp"
#include "wayfire/plugins/common/input-grab.hpp"
#include "wayfire/plugins/common/shared-core-data.hpp"
#include "wayfire/scene-input.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-provider.hpp"
#include "wayfire/toplevel-view.hpp"
#include "wayfire/view.hpp"

#include "tile-wset.hpp"

static bool can_tile_view(wayfire_toplevel_view view)
{
    if (view->parent)
    {
        return false;
    }

    return true;
}

namespace wf
{
class tile_output_plugin_t : public wf::pointer_interaction_t, public wf::custom_data_t
{
  private:
    wf::view_matcher_t tile_by_default{"simple-tile/tile_by_default"};
    wf::option_wrapper_t<bool> keep_fullscreen_on_adjacent{"simple-tile/keep_fullscreen_on_adjacent"};
    wf::option_wrapper_t<wf::buttonbinding_t> button_move{"simple-tile/button_move"};
    wf::option_wrapper_t<wf::buttonbinding_t> button_resize{"simple-tile/button_resize"};

    wf::option_wrapper_t<wf::keybinding_t> key_toggle_tile{"simple-tile/key_toggle"};
    wf::option_wrapper_t<wf::keybinding_t> key_focus_left{"simple-tile/key_focus_left"};
    wf::option_wrapper_t<wf::keybinding_t> key_focus_right{"simple-tile/key_focus_right"};
    wf::option_wrapper_t<wf::keybinding_t> key_focus_above{"simple-tile/key_focus_above"};
    wf::option_wrapper_t<wf::keybinding_t> key_focus_below{"simple-tile/key_focus_below"};
    wf::output_t *output;

  public:
    std::unique_ptr<wf::input_grab_t> input_grab;

    static std::unique_ptr<wf::tile::tile_controller_t> get_default_controller()
    {
        return std::make_unique<wf::tile::tile_controller_t>();
    }

    std::unique_ptr<wf::tile::tile_controller_t> controller =
        get_default_controller();

    /**
     * Translate coordinates from output-local coordinates to the coordinate
     * system of the tiling trees, depending on the current workspace
     */
    wf::point_t get_global_input_coordinates()
    {
        wf::pointf_t local = output->get_cursor_position();

        auto vp   = output->wset()->get_current_workspace();
        auto size = output->get_screen_size();
        local.x += size.width * vp.x;
        local.y += size.height * vp.y;

        return {(int)local.x, (int)local.y};
    }

    /** Check whether we currently have a fullscreen tiled view */
    bool has_fullscreen_view()
    {
        int count_fullscreen = 0;
        for_each_view(tile_workspace_set_data_t::get_current_root(output), [&] (wayfire_toplevel_view view)
        {
            count_fullscreen += view->pending_fullscreen();
        });

        return count_fullscreen > 0;
    }

    /** Check whether the current pointer focus is tiled view */
    bool has_tiled_focus()
    {
        auto focus = wf::get_core().get_cursor_focus_view();

        return focus && tile::view_node_t::get_node(focus);
    }

    template<class Controller>
    void start_controller()
    {
        /* No action possible in this case */
        if (has_fullscreen_view() || !has_tiled_focus())
        {
            return;
        }

        if (!output->activate_plugin(&grab_interface))
        {
            return;
        }

        input_grab->grab_input(wf::scene::layer::OVERLAY);
        controller = std::make_unique<Controller>(tile_workspace_set_data_t::get_current_root(output),
            get_global_input_coordinates());
    }

    void stop_controller(bool force_stop)
    {
        if (!output->is_plugin_active(grab_interface.name))
        {
            return;
        }

        input_grab->ungrab_input();

        // Deactivate plugin, so that others can react to the events
        output->deactivate_plugin(&grab_interface);
        if (!force_stop)
        {
            controller->input_released();
        }

        controller = get_default_controller();
    }

    bool tile_window_by_default(wayfire_toplevel_view view)
    {
        return tile_by_default.matches(view) && can_tile_view(view);
    }

    void attach_view(wayfire_toplevel_view view, std::optional<wf::point_t> vp = {})
    {
        if (!view->get_wset())
        {
            return;
        }

        stop_controller(true);
        tile_workspace_set_data_t::get(view->get_wset()).attach_view(view, vp);
    }

    void detach_view(nonstd::observer_ptr<tile::view_node_t> view, bool reinsert = true)
    {
        stop_controller(true);
        tile_workspace_set_data_t::get(view->view->get_wset()).detach_views({view}, reinsert);
    }

    wf::signal::connection_t<view_mapped_signal> on_view_mapped = [=] (view_mapped_signal *ev)
    {
        if (auto toplevel = toplevel_cast(ev->view))
        {
            if (tile_window_by_default(toplevel))
            {
                attach_view(toplevel);
            }
        }
    };

    wf::signal::connection_t<view_unmapped_signal> on_view_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        stop_controller(true);
        auto node = wf::tile::view_node_t::get_node(ev->view);
        if (node)
        {
            detach_view(node);
        }
    };

    wf::signal::connection_t<view_tile_request_signal> on_tile_request = [=] (view_tile_request_signal *ev)
    {
        if (ev->carried_out || !tile::view_node_t::get_node(ev->view))
        {
            return;
        }

        // we ignore those requests because we manage the tiled state manually
        ev->carried_out = true;
    };

    wf::signal::connection_t<view_fullscreen_request_signal> on_fullscreen_request =
        [=] (view_fullscreen_request_signal *ev)
    {
        if (ev->carried_out || !tile::view_node_t::get_node(ev->view))
        {
            return;
        }

        ev->carried_out = true;
        tile_workspace_set_data_t::get(ev->view->get_wset()).set_view_fullscreen(ev->view, ev->state);
    };

    void change_view_workspace(wayfire_toplevel_view view, std::optional<wf::point_t> vp = {})
    {
        auto existing_node = wf::tile::view_node_t::get_node(view);
        if (existing_node)
        {
            detach_view(existing_node);
            attach_view(view, vp);
        }
    }

    wf::signal::connection_t<view_change_workspace_signal> on_view_change_workspace =
        [=] (view_change_workspace_signal *ev)
    {
        if (ev->old_workspace_valid)
        {
            change_view_workspace(ev->view, ev->to);
        }
    };

    wf::signal::connection_t<view_minimized_signal> on_view_minimized = [=] (view_minimized_signal *ev)
    {
        auto existing_node = wf::tile::view_node_t::get_node(ev->view);

        if (ev->view->minimized && existing_node)
        {
            detach_view(existing_node);
        }

        if (!ev->view->minimized && tile_window_by_default(ev->view))
        {
            attach_view(ev->view);
        }
    };

    /**
     * Execute the given function on the focused view iff we can activate the
     * tiling plugin, there is a focused view and the focused view is a tiled
     * view
     *
     * @param need_tiled Whether the view needs to be tiled
     */
    bool conditioned_view_execute(bool need_tiled,
        std::function<void(wayfire_toplevel_view)> func)
    {
        auto view = wf::get_core().seat->get_active_view();
        if (!toplevel_cast(view) || (view->get_output() != output))
        {
            return false;
        }

        if (need_tiled && !tile::view_node_t::get_node(view))
        {
            return false;
        }

        if (output->can_activate_plugin(&grab_interface))
        {
            func(toplevel_cast(view));
            return true;
        }

        return false;
    }

    wf::key_callback on_toggle_tiled_state = [=] (auto)
    {
        return conditioned_view_execute(false, [=] (wayfire_toplevel_view view)
        {
            auto existing_node = tile::view_node_t::get_node(view);
            if (existing_node)
            {
                detach_view(existing_node);
                wf::get_core().default_wm->tile_request(view, 0);
            } else
            {
                attach_view(view);
            }
        });
    };

    bool focus_adjacent(tile::split_insertion_t direction)
    {
        return conditioned_view_execute(true, [=] (wayfire_toplevel_view view)
        {
            auto adjacent = tile::find_first_view_in_direction(
                tile::view_node_t::get_node(view), direction);

            bool was_fullscreen = view->pending_fullscreen();
            if (adjacent)
            {
                /* This will lower the fullscreen status of the view */
                view_bring_to_front(adjacent->view);
                wf::get_core().seat->focus_view(adjacent->view);

                if (was_fullscreen && keep_fullscreen_on_adjacent)
                {
                    wf::get_core().default_wm->fullscreen_request(adjacent->view, output, true);
                }
            }
        });
    }

    wf::key_callback on_focus_adjacent = [=] (wf::keybinding_t binding)
    {
        if (binding == key_focus_left)
        {
            return focus_adjacent(tile::INSERT_LEFT);
        }

        if (binding == key_focus_right)
        {
            return focus_adjacent(tile::INSERT_RIGHT);
        }

        if (binding == key_focus_above)
        {
            return focus_adjacent(tile::INSERT_ABOVE);
        }

        if (binding == key_focus_below)
        {
            return focus_adjacent(tile::INSERT_BELOW);
        }

        return false;
    };

    wf::button_callback on_move_view = [=] (auto)
    {
        start_controller<tile::move_view_controller_t>();
        return false; // pass button to the grab node
    };

    wf::button_callback on_resize_view = [=] (auto)
    {
        start_controller<tile::resize_view_controller_t>();
        return false; // pass button to the grab node
    };

    void handle_pointer_button(const wlr_pointer_button_event& event) override
    {
        if (event.state == WLR_BUTTON_RELEASED)
        {
            stop_controller(false);
        }
    }

    void handle_pointer_motion(wf::pointf_t pointer_position, uint32_t time_ms) override
    {
        controller->input_motion(get_global_input_coordinates());
    }

    void setup_callbacks()
    {
        output->add_button(button_move, &on_move_view);
        output->add_button(button_resize, &on_resize_view);
        output->add_key(key_toggle_tile, &on_toggle_tiled_state);

        output->add_key(key_focus_left, &on_focus_adjacent);
        output->add_key(key_focus_right, &on_focus_adjacent);
        output->add_key(key_focus_above, &on_focus_adjacent);
        output->add_key(key_focus_below, &on_focus_adjacent);
    }

    wf::plugin_activation_data_t grab_interface = {
        .name = "simple-tile",
        .capabilities = CAPABILITY_MANAGE_COMPOSITOR,
    };

  public:
    tile_output_plugin_t(wf::output_t *wo)
    {
        this->output = wo;
        input_grab   = std::make_unique<wf::input_grab_t>("simple-tile", output, nullptr, this, nullptr);
        output->connect(&on_view_mapped);
        output->connect(&on_view_unmapped);
        output->connect(&on_tile_request);
        output->connect(&on_fullscreen_request);
        output->connect(&on_view_change_workspace);
        output->connect(&on_view_minimized);
        setup_callbacks();
    }

    ~tile_output_plugin_t()
    {
        output->rem_binding(&on_move_view);
        output->rem_binding(&on_resize_view);
        output->rem_binding(&on_toggle_tiled_state);
        output->rem_binding(&on_focus_adjacent);
    }
};

class tile_plugin_t : public wf::plugin_interface_t, wf::per_output_tracker_mixin_t<>
{
    shared_data::ref_ptr_t<ipc::method_repository_t> ipc_repo;

  public:
    void init() override
    {
        init_output_tracking();
        wf::get_core().connect(&on_view_pre_moved_to_wset);
        wf::get_core().connect(&on_view_moved_to_wset);
        wf::get_core().connect(&on_focus_changed);
        ipc_repo->register_method("simple-tile/get-layout", ipc_get_layout);
        ipc_repo->register_method("simple-tile/set-layout", ipc_set_layout);
    }

    void fini() override
    {
        fini_output_tracking();
        for (auto wset : workspace_set_t::get_all())
        {
            wset->erase_data<tile_workspace_set_data_t>();
        }

        for (auto wo : wf::get_core().output_layout->get_outputs())
        {
            wo->erase_data<tile_output_plugin_t>();
        }
    }

    void stop_controller(std::shared_ptr<wf::workspace_set_t> wset)
    {
        if (auto wo = wset->get_attached_output())
        {
            auto tile = wo->get_data<tile_output_plugin_t>();
            if (tile)
            {
                tile->stop_controller(true);
            }
        }
    }

    wf::signal::connection_t<view_pre_moved_to_wset_signal> on_view_pre_moved_to_wset =
        [=] (view_pre_moved_to_wset_signal *ev)
    {
        auto node = wf::tile::view_node_t::get_node(ev->view);
        if (node)
        {
            ev->view->store_data(std::make_unique<wf::view_auto_tile_t>());
            if (ev->old_wset)
            {
                stop_controller(ev->old_wset);
                tile_workspace_set_data_t::get(ev->old_wset).detach_views({node});
            }
        }
    };

    wf::signal::connection_t<keyboard_focus_changed_signal> on_focus_changed =
        [=] (keyboard_focus_changed_signal *ev)
    {
        if (auto toplevel = toplevel_cast(wf::node_to_view(ev->new_focus)))
        {
            if (toplevel->get_wset())
            {
                tile_workspace_set_data_t::get(toplevel->get_wset()).consider_exit_fullscreen(toplevel);
            }
        }
    };


    wf::signal::connection_t<view_moved_to_wset_signal> on_view_moved_to_wset =
        [=] (view_moved_to_wset_signal *ev)
    {
        if (ev->view->has_data<view_auto_tile_t>() && ev->new_wset)
        {
            stop_controller(ev->new_wset);
            tile_workspace_set_data_t::get(ev->new_wset).attach_view(ev->view);
        }
    };

    void handle_new_output(wf::output_t *output) override
    {
        output->store_data(std::make_unique<tile_output_plugin_t>(output));
    }

    void handle_output_removed(wf::output_t *output) override
    {
        output->erase_data<tile_output_plugin_t>();
    }

    ipc::method_callback ipc_get_layout = [=] (const nlohmann::json& params)
    {
        WFJSON_EXPECT_FIELD(params, "wset-index", number_unsigned);
        WFJSON_EXPECT_FIELD(params, "workspace", object);
        WFJSON_EXPECT_FIELD(params["workspace"], "x", number_unsigned);
        WFJSON_EXPECT_FIELD(params["workspace"], "y", number_unsigned);

        int x   = params["workspace"]["x"].get<int>();
        int y   = params["workspace"]["y"].get<int>();
        auto ws = ipc::find_workspace_set_by_index(params["wset-index"].get<int>());
        if (ws)
        {
            auto grid_size = ws->get_workspace_grid_size();
            if ((x >= grid_size.width) || (y >= grid_size.height))
            {
                return wf::ipc::json_error("invalid workspace coordinates");
            }

            auto response = wf::ipc::json_ok();

            auto cur_ws     = ws->get_current_workspace();
            auto resolution = ws->get_last_output_geometry().value_or(tile::default_output_resolution);
            wf::point_t offset = {cur_ws.x * resolution.width, cur_ws.y * resolution.height};

            response["layout"] =
                tree_to_json(tile_workspace_set_data_t::get(ws->shared_from_this()).roots[x][y], offset);
            return response;
        }

        return wf::ipc::json_error("wset-index not found");
    };

    ipc::method_callback ipc_set_layout = [=] (nlohmann::json params) -> nlohmann::json
    {
        WFJSON_EXPECT_FIELD(params, "wset-index", number_unsigned);
        WFJSON_EXPECT_FIELD(params, "workspace", object);
        WFJSON_EXPECT_FIELD(params["workspace"], "x", number_unsigned);
        WFJSON_EXPECT_FIELD(params["workspace"], "y", number_unsigned);
        WFJSON_EXPECT_FIELD(params, "layout", object);
        int x = params["workspace"]["x"].get<int>();
        int y = params["workspace"]["y"].get<int>();

        auto ws = ipc::find_workspace_set_by_index(params["wset-index"].get<int>());
        if (!ws)
        {
            return wf::ipc::json_error("wset-index not found");
        }

        auto grid_size = ws->get_workspace_grid_size();
        if ((x >= grid_size.width) || (y >= grid_size.height))
        {
            return wf::ipc::json_error("invalid workspace coordinates");
        }

        auto& tile_ws = tile_workspace_set_data_t::get(ws->shared_from_this());

        tile::json_builder_data_t data;
        data.gaps = tile_ws.get_gaps();
        auto workarea = tile_ws.roots[x][y]->geometry;
        if (auto err = tile::verify_json_tree(params["layout"], data, wf::dimensions(workarea)))
        {
            return wf::ipc::json_error(*err);
        }

        // Step 1: detach any views which are currently present in the layout, but should no longer be
        // in the layout
        std::vector<nonstd::observer_ptr<tile::view_node_t>> views_to_remove;
        tile::for_each_view(tile_ws.roots[x][y], [&] (wayfire_toplevel_view view)
        {
            if (!data.touched_views.count(view))
            {
                views_to_remove.push_back(tile::view_node_t::get_node(view));
            }
        });

        tile_ws.detach_views(views_to_remove);

        {
            autocommit_transaction_t tx;
            data.touched_wsets.erase(nullptr);

            // Step 2: temporarily detach some of the nodes
            for (auto& touched_view : data.touched_views)
            {
                auto tile = wf::tile::view_node_t::get_node(touched_view);
                if (tile)
                {
                    tile->parent->remove_child(tile, tx.tx);
                }

                if (touched_view->get_wset().get() != ws)
                {
                    auto old_wset = touched_view->get_wset();
                    wf::emit_view_pre_moved_to_wset_pre(touched_view,
                        touched_view->get_wset(), ws->shared_from_this());

                    if (old_wset)
                    {
                        old_wset->remove_view(touched_view);
                    }

                    ws->add_view(touched_view);
                    wf::emit_view_moved_to_wset(touched_view, old_wset, ws->shared_from_this());
                }
            }

            // Step 3: set up the new layout
            tile_ws.roots[x][y] = build_tree_from_json(params["layout"], &tile_ws, {x, y});
            tile::flatten_tree(tile_ws.roots[x][y]);
            tile_ws.roots[x][y]->set_gaps(tile_ws.get_gaps());
            tile_ws.roots[x][y]->set_geometry(workarea, tx.tx);
        }

        data.touched_wsets.insert(ws);

        // Step 4: flatten roots, set gaps, trigger resize everywhere
        for (auto& touched_ws : data.touched_wsets)
        {
            auto& tws = tile_workspace_set_data_t::get(touched_ws->shared_from_this());
            tws.flatten_roots();
            // will also trigger resize everywhere
            tws.update_gaps();
        }

        return wf::ipc::json_ok();
    };

    /**
     * Build a tiling tree from a json description.
     *
     * Note that the tree description first has to be verified and pre-processed by verify_json_tree().
     */
    std::unique_ptr<tile::tree_node_t> build_tree_from_json(const nlohmann::json& json,
        tile_workspace_set_data_t *wdata, wf::point_t vp)
    {
        auto root = build_tree_from_json_rec(json, wdata, vp);
        if (root->as_view_node())
        {
            // Handle cases with a single view.
            auto split_root = std::make_unique<tile::split_node_t>(tile_workspace_set_data_t::default_split);
            split_root->children.push_back(std::move(root));
            return split_root;
        }

        return root;
    }

    std::unique_ptr<tile::tree_node_t> build_tree_from_json_rec(const nlohmann::json& json,
        tile_workspace_set_data_t *wdata, wf::point_t vp)
    {
        std::unique_ptr<tile::tree_node_t> root;

        if (json.count("view-id"))
        {
            auto view = toplevel_cast(wf::ipc::find_view_by_id(json["view-id"]));
            root = wdata->setup_view_tiling(view, vp);
        } else
        {
            const bool is_horiz_split = json.count("horizontal-split");
            auto& children_list = is_horiz_split ? json["horizontal-split"] : json["vertical-split"];
            auto split_parent   = std::make_unique<tile::split_node_t>(
                is_horiz_split ? tile::SPLIT_HORIZONTAL : tile::SPLIT_VERTICAL);

            for (auto& child : children_list)
            {
                split_parent->children.push_back(build_tree_from_json_rec(child, wdata, vp));
                split_parent->children.back()->parent = {split_parent.get()};
            }

            root = std::move(split_parent);
        }

        root->geometry.x     = 0;
        root->geometry.y     = 0;
        root->geometry.width = json["width"];
        root->geometry.height = json["height"];
        return root;
    }
};
}

DECLARE_WAYFIRE_PLUGIN(wf::tile_plugin_t);
