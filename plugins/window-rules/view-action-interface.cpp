#include "view-action-interface.hpp"

#include "wayfire/output.hpp"
#include "wayfire/view.hpp"
#include "wayfire/workspace-manager.hpp"
#include "wayfire/plugins/grid.hpp"
#include "wayfire/util/log.hpp"
#include "wayfire/view-transform.hpp"
#include "../wm-actions/wm-actions-signals.hpp"

#include <algorithm>
#include <cfloat>
#include <iostream>
#include <string>
#include <vector>

namespace wf
{
view_action_interface_t::~view_action_interface_t()
{}

bool view_action_interface_t::execute(const std::string & name,
    const std::vector<variant_t> & args)
{
    if (name == "set")
    {
        auto id = wf::get_string(args.at(0));

        if (id == "sticky")
        {
            _make_sticky();
            return false;
        } else if (id == "always_on_top")
        {
            _always_on_top();
            return false;
        }

        if ((args.size() < 2) || (wf::is_string(args.at(0)) == false))
        {
            LOGE(
                "View action interface: Set execution requires at least 2 arguments, the first of which should be an identifier.");

            return true;
        }

        if (id == "alpha")
        {
            auto alpha = _validate_alpha(args);
            if (std::get<0>(alpha))
            {
                _set_alpha(std::get<1>(alpha));
            }
        } else if (id == "geometry")
        {
            auto geometry = _validate_geometry(args);
            if (std::get<0>(geometry))
            {
                _set_geometry(std::get<1>(geometry), std::get<2>(geometry),
                    std::get<3>(geometry), std::get<4>(geometry));
            }
        } else
        {
            LOGE("View action interface: Unsupported set operation to identifier ",
                id);

            return true;
        }

        return false;
    } else if (name == "maximize")
    {
        _maximize();

        return false;
    } else if (name == "unmaximize")
    {
        _unmaximize();

        return false;
    } else if (name == "minimize")
    {
        _minimize();

        return false;
    } else if (name == "unminimize")
    {
        _unminimize();

        return false;
    } else if (name == "snap")
    {
        if ((args.size() < 1) || (wf::is_string(args.at(0)) == false))
        {
            LOGE(
                "View action interface: Snap execution requires 1 string as argument.");

            return true;
        }

        auto output = _view->get_output();
        if (output == nullptr)
        {
            LOGE("View action interface: Output associated with view was null.");

            return true;
        }

        auto location = wf::get_string(args.at(0));

        wf::grid::grid_snap_view_signal data;
        data.view = _view;

        if (location == "top")
        {
            data.slot = grid::SLOT_TOP;
        } else if (location == "top_right")
        {
            data.slot = grid::SLOT_TR;
        } else if (location == "right")
        {
            data.slot = grid::SLOT_RIGHT;
        } else if (location == "bottom_right")
        {
            data.slot = grid::SLOT_BR;
        } else if (location == "bottom")
        {
            data.slot = grid::SLOT_BOTTOM;
        } else if (location == "bottom_left")
        {
            data.slot = grid::SLOT_BL;
        } else if (location == "left")
        {
            data.slot = grid::SLOT_LEFT;
        } else if (location == "top_left")
        {
            data.slot = grid::SLOT_TL;
        } else if (location == "center")
        {
            data.slot = grid::SLOT_CENTER;
        } else
        {
            LOGE(
                "View action interface: Incorrect string literal for snap location: ", location,
                ".");

            return true;
        }

        LOGI("View action interface: Snap to ", location, ".");

        output->emit_signal("grid-snap-view", &data);

        return false;
    } else if (name == "move")
    {
        auto position = _validate_position(args);
        if (std::get<0>(position))
        {
            _move(std::get<1>(position), std::get<2>(position));
            return false;
        }

        LOGE("View action interface: invalid arguments for move");
        return true;
    } else if (name == "resize")
    {
        auto size = _validate_size(args);
        if (std::get<0>(size))
        {
            _resize(std::get<1>(size), std::get<2>(size));
            return false;
        }

        LOGE("View action interface: invalid arguments for resize");
        return true;
    } else if (name == "assign_workspace")
    {
        auto [ok, ws] = _validate_ws(args);
        if (ok)
        {
            _assign_ws(ws);
            return false;
        }

        return true;
    }

    LOGE("View action interface: Unsupported action execution requested. Name: ",
        name, ".");

    return true;
}

void view_action_interface_t::set_view(wayfire_view view)
{
    _view = view;
}

void view_action_interface_t::_maximize()
{
    _view->tile_request(wf::TILED_EDGES_ALL);
}

void view_action_interface_t::_unmaximize()
{
    _view->tile_request(0);
}

void view_action_interface_t::_minimize()
{
    _view->set_minimized(true);
}

void view_action_interface_t::_unminimize()
{
    _view->set_minimized(false);
}

void view_action_interface_t::_make_sticky()
{
    _view->set_sticky(1);
}

void view_action_interface_t::_always_on_top()
{
    wf::wm_actions_set_above_state data;

    auto output = _view->get_output();
    if (!output)
    {
        return;
    }

    data.view  = _view;
    data.above = true;
    output->emit_signal("wm-actions-set-above-state", &data);
}

std::tuple<bool, float> view_action_interface_t::_expect_float(
    const std::vector<variant_t> & args, std::size_t position)
{
    if ((args.size() > position) && (wf::is_float(args.at(position))))
    {
        return {true, wf::get_float(args.at(position))};
    }

    return {false, 0.0f};
}

std::tuple<bool, double> view_action_interface_t::_expect_double(
    const std::vector<variant_t> & args, std::size_t position)
{
    if ((args.size() > position) && (wf::is_double(args.at(position))))
    {
        return {true, wf::get_double(args.at(position))};
    }

    return {false, 0.0};
}

std::tuple<bool, int> view_action_interface_t::_expect_int(
    const std::vector<variant_t> & args, std::size_t position)
{
    if ((args.size() > position) && (wf::is_int(args.at(position))))
    {
        return {true, wf::get_int(args.at(position))};
    }

    return {false, 0};
}

std::tuple<bool, float> view_action_interface_t::_validate_alpha(
    const std::vector<variant_t> & args)
{
    auto arg_float = _expect_float(args, 1);
    if (std::get<0>(arg_float))
    {
        return arg_float;
    } else
    {
        auto arg_double = _expect_double(args, 1);
        if (std::get<0>(arg_double))
        {
            return {true, static_cast<float>(std::get<1>(arg_double))};
        }
    }

    LOGE(
        "View action interface: Invalid arguments. Expected 'set alpha [float|double].");

    return {false, 1.0f};
}

std::tuple<bool, int, int, int, int> view_action_interface_t::_validate_geometry(
    const std::vector<variant_t> & args)
{
    auto arg_x = _expect_int(args, 1);
    auto arg_y = _expect_int(args, 2);
    auto arg_w = _expect_int(args, 3);
    auto arg_h = _expect_int(args, 4);

    if (std::get<0>(arg_x) && std::get<0>(arg_y) && std::get<0>(arg_w) &&
        std::get<0>(arg_h))
    {
        return {true, std::get<1>(arg_x), std::get<1>(arg_y), std::get<1>(arg_w),
            std::get<1>(arg_h)};
    }

    LOGE(
        "View action interface: Invalid arguments. Expected 'set geometry int int int int.");

    return {false, 0, 0, 0, 0};
}

std::tuple<bool, int, int> view_action_interface_t::_validate_position(
    const std::vector<variant_t> & args)
{
    auto arg_x = _expect_int(args, 0);
    auto arg_y = _expect_int(args, 1);

    if (std::get<0>(arg_x) && std::get<0>(arg_y))
    {
        return {true, std::get<1>(arg_x), std::get<1>(arg_y)};
    }

    LOGE("View action interface: Invalid arguments. Expected 'move int int.");

    return {false, 0, 0};
}

std::tuple<bool, int, int> view_action_interface_t::_validate_size(
    const std::vector<variant_t> & args)
{
    auto arg_w = _expect_int(args, 0);
    auto arg_h = _expect_int(args, 1);

    if (std::get<0>(arg_w) && std::get<0>(arg_h))
    {
        return {true, std::get<1>(arg_w), std::get<1>(arg_h)};
    }

    LOGE("View action interface: Invalid arguments. Expected 'resize int int.");

    return {false, 0, 0};
}

void view_action_interface_t::_set_alpha(float alpha)
{
    alpha = std::clamp(alpha, 0.1f, 1.0f);

    // Apply view transformer if needed and set alpha.
    wf::view_2D *transformer;

    if (!_view->get_transformer("alpha"))
    {
        _view->add_transformer(std::make_unique<wf::view_2D>(_view), "alpha");
    }

    transformer = dynamic_cast<wf::view_2D*>(_view->get_transformer("alpha").get());
    if (fabs(transformer->alpha - alpha) > FLT_EPSILON)
    {
        transformer->alpha = alpha;
        _view->damage();

        LOGI("View action interface: Alpha set to ", alpha, ".");
    }
}

void view_action_interface_t::_set_geometry(int x, int y, int w, int h)
{
    _resize(w, h);
    _move(x, y);
}

std::tuple<bool, wf::point_t> view_action_interface_t::_validate_ws(
    const std::vector<variant_t>& args)
{
    if (!this->_view->get_output())
    {
        return {false, {}};
    }

    if (args.size() != 2)
    {
        LOGE("Invalid workspace identifier, expected <x> <y>");
    }

    auto [ok1, x] = _expect_int(args, 0);
    auto [ok2, y] = _expect_int(args, 1);
    if (!ok1 || !ok2)
    {
        LOGE("Workspace coordinates should be integers!");
        return {false, {}};
    }

    auto wsize = _view->get_output()->workspace->get_workspace_grid_size();
    if (((0 <= x) && (x < wsize.width)) && ((0 <= y) && (y < wsize.height)))
    {
        return {true, {x, y}};
    }

    LOGE("Workspace coordinates out of bounds!");
    return {false, {}};
}

wf::geometry_t view_action_interface_t::_get_workspace_grid_geometry(
    wf::output_t *output) const
{
    auto vsize = output->workspace->get_workspace_grid_size();
    auto vp    = output->workspace->get_current_workspace();
    auto res   = output->get_screen_size();

    return wf::geometry_t{
        -vp.x * res.width,
        -vp.y * res.height,
        vsize.width * res.width,
        vsize.height * res.height,
    };
}

void view_action_interface_t::_move(int x, int y)
{
    // Clamp x and y to sane values. Do not allow to move outside of workspace grid.
    auto output = _view->get_output();
    if (output != nullptr)
    {
        auto grid = this->_get_workspace_grid_geometry(output);
        auto view_geometry = _view->get_wm_geometry();
        view_geometry.x = x;
        view_geometry.y = y;

        view_geometry = wf::clamp(view_geometry, grid);
        _view->move(view_geometry.x, view_geometry.y);
    }
}

void view_action_interface_t::_resize(int w, int h)
{
    // Clamp w and h to sane values. Do not allow to get bigger then output. Do not
    // allow to get smaller then 40x30.
    auto output = _view->get_output();
    if (output != nullptr)
    {
        auto dimensions = output->get_screen_size();

        w = std::clamp(w, 40, dimensions.width);
        h = std::clamp(h, 30, dimensions.height);

        _view->resize(w, h);
    }
}

void view_action_interface_t::_assign_ws(wf::point_t point)
{
    auto output = _view->get_output();

    auto delta = point - output->workspace->get_current_workspace();
    auto size  = output->get_screen_size();

    auto wm = _view->get_wm_geometry();
    _view->move(wm.x + delta.x * size.width, wm.y + delta.y * size.height);
}
} // End namespace wf.
