#include "view-keyboard-interaction.cpp"
#include <memory>
#include <wayfire/debug.hpp>
#include <wayfire/output.hpp>
#include "../core/core-impl.hpp"
#include "../core/seat/input-manager.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/opengl.hpp"
#include "wayfire/region.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-provider.hpp"
#include "wayfire/view.hpp"

wf::scene::view_node_t::view_node_t(wayfire_view _view) :
    floating_inner_node_t(false), view(_view)
{
    this->kb_interaction = std::make_unique<view_keyboard_interaction_t>(view);
}

wf::scene::view_node_t::view_node_t() : floating_inner_node_t(false)
{}

std::string wf::scene::view_node_t::stringify() const
{
    std::ostringstream out;
    out << this->view;
    return out.str() + " " + stringify_flags();
}

wf::keyboard_interaction_t& wf::scene::view_node_t::keyboard_interaction()
{
    return *kb_interaction;
}

wf::pointf_t wf::scene::view_node_t::to_local(const wf::pointf_t& point)
{
    auto local = view->global_to_local_point(point, nullptr);
    return local;
}

wf::pointf_t wf::scene::view_node_t::to_global(const wf::pointf_t& point)
{
    auto local = point;
    local.x += view->get_output_geometry().x;
    local.y += view->get_output_geometry().y;
    return view->transform_point(local);
}

std::optional<wf::scene::input_node_t> wf::scene::view_node_t::find_node_at(
    const wf::pointf_t& at)
{
    if (view->minimized ||
        !wf::get_core_impl().input->can_focus_surface(view.get()))
    {
        return {};
    }

    return floating_inner_node_t::find_node_at(at);
}

void wf::scene::view_node_t::collect_visible_nodes(
    std::vector<node_t*>& nodes, wf::geometry_t region)
{
    if (auto wo = view->get_output())
    {
        auto g = view->get_output()->get_relative_geometry();
        if (view->sticky)
        {
            region.x = (region.x % g.width + g.width) % g.width;
            region.y = (region.y % g.height + g.height) % g.height;
        }
    }

    bool visible = false;
    if (view->has_transformer())
    {
        visible = view->intersects_region(region);
    } else
    {
        visible = region & view->get_wm_geometry();
    }

    if (visible)
    {
        nodes.push_back(this);
    }
}

namespace wf
{
namespace scene
{
class view_render_instance_t : public render_instance_t
{
    std::vector<render_instance_uptr> children;
    wayfire_view view;
    damage_callback push_damage;

  public:
    view_render_instance_t(wayfire_view view, damage_callback push_damage)
    {
        this->view = view;
        this->push_damage = push_damage;
        view->get_surface_root_node()->connect(&on_view_damage);

        auto push_damage_child = [=] (wf::region_t child_damage)
        {
            child_damage += wf::origin(view->get_output_geometry());
            if (view->has_transformer())
            {
                child_damage = view->transform_region(
                    wlr_box_from_pixman_box(child_damage.get_extents()));
            }

            push_damage(child_damage);
        };

        for (auto& ch : view->get_surface_root_node()->get_children())
        {
            if (ch->is_enabled())
            {
                ch->gen_render_instances(children, push_damage_child);
            }
        }
    }

    // FIXME: once transformers are proper nodes, this should be
    // done in the surfaces and then bubbled up.
    wf::signal::connection_t<node_damage_signal> on_view_damage =
        [=] (node_damage_signal *data)
    {
        push_damage(data->region);
    };

    void schedule_instructions(std::vector<render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::region_t& damage) override
    {
        wf::render_target_t our_target = target;
        wf::point_t offset = {0, 0};

        if (view->sticky && view->get_output())
        {
            // Adjust geometry of damage/target so that view is always visible if
            // sticky
            auto output_size = view->get_output()->get_screen_size();
            our_target.geometry.x = target.geometry.x % output_size.width;
            our_target.geometry.y = target.geometry.y % output_size.height;
            offset = wf::origin(target.geometry) - wf::origin(our_target.geometry);
        }

        damage += -offset;

        wf::region_t our_damage = damage & view->get_bounding_box();
        if (!our_damage.empty())
        {
            if (view->has_transformer())
            {
                instructions.push_back(render_instruction_t{
                            .instance = this,
                            .target   = our_target,
                            .damage   = std::move(our_damage),
                        });

                damage ^= view->get_transformed_opaque_region();
            } else
            {
                auto surface_offset = wf::origin(view->get_output_geometry());
                damage += -surface_offset;
                our_target.geometry = our_target.geometry + -surface_offset;
                for (auto& ch : this->children)
                {
                    ch->schedule_instructions(instructions, our_target, damage);
                }

                damage += surface_offset;
            }
        }

        damage += offset;
    }

    void render(const wf::render_target_t& target,
        const wf::region_t& region) override
    {
        view->render_transformed(target, region);
    }

    void presentation_feedback(wf::output_t *output) override
    {
        for (auto& ch : this->children)
        {
            ch->presentation_feedback(output);
        }
    }

    direct_scanout try_scanout(wf::output_t *output) override
    {
        auto og = output->get_relative_geometry();
        if (!(this->view->get_bounding_box() & og))
        {
            return direct_scanout::SKIP;
        }

        // The candidate must cover the whole output
        if (view->get_output_geometry() != output->get_relative_geometry())
        {
            return direct_scanout::OCCLUSION;
        }

        // The view must have only a single surface and no transformers
        if (view->has_transformer() ||
            !view->priv->surface_children_above.empty() ||
            !view->children.empty())
        {
            return direct_scanout::OCCLUSION;
        }

        // Must have a wlr surface with the correct scale and transform
        auto surface = view->get_wlr_surface();
        if (!surface ||
            (surface->current.scale != output->handle->scale) ||
            (surface->current.transform != output->handle->transform))
        {
            return direct_scanout::OCCLUSION;
        }

        // Finally, the opaque region must be the full surface.
        wf::region_t non_opaque = output->get_relative_geometry();
        non_opaque ^= view->get_opaque_region(wf::point_t{0, 0});
        if (!non_opaque.empty())
        {
            return direct_scanout::OCCLUSION;
        }

        wlr_presentation_surface_sampled_on_output(
            wf::get_core().protocols.presentation, surface, output->handle);
        wlr_output_attach_buffer(output->handle, &surface->buffer->base);

        if (wlr_output_commit(output->handle))
        {
            LOGC(SCANOUT, "Scanned out ", view, " on output ", output->to_string());
            return direct_scanout::SUCCESS;
        } else
        {
            LOGC(SCANOUT, "Failed to scan out ", view, " on output ",
                output->to_string());
            return direct_scanout::OCCLUSION;
        }
    }
};

void view_node_t::gen_render_instances(std::vector<render_instance_uptr> & instances,
    damage_callback push_damage, wf::output_t *shown_on)
{
    if ((this->view->role == VIEW_ROLE_DESKTOP_ENVIRONMENT) &&
        this->view->sticky)
    {
        // FIXME: this code should be layer-shell-node-specific
        // Special case: layer-shell views live only inside their outputs and
        // have no benefit from being rendered on other outputs.
        if (shown_on && (this->view->get_output() != shown_on))
        {
            return;
        }
    }

    instances.push_back(std::make_unique<wf::scene::view_render_instance_t>(
        this->view, push_damage));
}

std::optional<wf::texture_t> view_node_t::to_texture() const
{
    if (view->is_mapped() &&
        !view->has_transformer() &&
        view->get_wlr_surface() &&
        (this->get_children().size() == 1))
    {
        return wf::texture_t{view->get_wlr_surface()};
    }

    return {};
}
}
}

wf::geometry_t wf::scene::view_node_t::get_bounding_box()
{
    return view->get_bounding_box();
}