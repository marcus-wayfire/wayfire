#include <cstdlib>
#include <memory>
#include <wayfire/config/types.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/output.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

#include "blur.hpp"
#include "plugins/common/wayfire/plugins/common/shared-core-data.hpp"
#include "wayfire/core.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/object.hpp"
#include "wayfire/opengl.hpp"
#include "wayfire/region.hpp"
#include "wayfire/scene-operations.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-provider.hpp"

using blur_algorithm_provider =
    std::function<nonstd::observer_ptr<wf_blur_base>()>;

namespace wf
{
namespace scene
{
class blur_render_instance_t : public render_instance_t
{
    blur_algorithm_provider provider;
    node_t *self;

    wf::framebuffer_t saved_pixels;
    wf::region_t saved_pixels_region;

    std::vector<render_instance_uptr> view_instance;
    wf::render_target_t inner_content;
    wf::region_t cached_damage;

    wf::texture_t get_texture(float scale)
    {
        // Optimization: if we have a single child (usually the surface root node)
        // and we can directly convert it to texture, we don't need a full render
        // pass.
        if (self->get_children().size() == 1)
        {
            if (auto tex = self->get_children().front()->to_texture())
            {
                if (inner_content.fb != (uint) - 1)
                {
                    // If we are optimized, make sure to release the buffer to
                    // avoid holding GL memory unnecessary. This way we'll also
                    // get full damage if the view gets other transformers attached.
                    OpenGL::render_begin();
                    inner_content.release();
                    OpenGL::render_end();
                }

                return *tex;
            }
        }

        auto bbox = self->get_bounding_box();
        int target_width  = scale * self->get_bounding_box().width;
        int target_height = scale * self->get_bounding_box().height;

        OpenGL::render_begin();
        if (inner_content.allocate(target_width, target_height))
        {
            cached_damage |= bbox;
        }

        inner_content.geometry = bbox;
        OpenGL::render_end();

        render_pass_params_t params;
        params.instances = &view_instance;
        params.target    = inner_content;
        params.damage    = cached_damage;
        params.background_color = {0.0f, 0.0f, 0.0f, 0.0f};
        scene::run_render_pass(params, RPASS_CLEAR_BACKGROUND);

        cached_damage.clear();
        return wf::texture_t{inner_content.tex};
    }

    void presentation_feedback(wf::output_t *output) override
    {
        for (auto& ch : view_instance)
        {
            ch->presentation_feedback(output);
        }
    }

  public:
    blur_render_instance_t(node_t *self, blur_algorithm_provider provider,
        damage_callback push_damage)
    {
        this->self     = self;
        this->provider = provider;

        auto push_damage_child = [=] (const wf::region_t& region)
        {
            this->cached_damage |= region;
            push_damage(region);
        };

        this->cached_damage |= self->get_bounding_box();
        for (auto& ch : self->get_children())
        {
            ch->gen_render_instances(view_instance, push_damage_child);
        }
    }

    ~blur_render_instance_t()
    {
        OpenGL::render_begin();
        saved_pixels.release();
        inner_content.release();
        OpenGL::render_end();
    }

    bool is_fully_opaque(wf::region_t damage)
    {
        if (self->get_children().size() == 1)
        {
            if (auto vnode = dynamic_cast<view_node_t*>(
                self->get_children().front().get()))
            {
                auto opaque_region =
                    vnode->get_view()->get_transformed_opaque_region();

                return (damage ^ opaque_region).empty();
            }
        }

        return false;
    }

    wf::region_t calculate_translucent_damage(float target_scale,
        wf::region_t damage)
    {
        if (self->get_children().size() == 1)
        {
            if (auto vnode = dynamic_cast<view_node_t*>(
                self->get_children().front().get()))
            {
                const int padding = std::ceil(
                    provider()->calculate_blur_radius() / target_scale);

                auto opaque_region =
                    vnode->get_view()->get_transformed_opaque_region();
                opaque_region.expand_edges(-padding);

                wf::region_t translucent_region = damage ^ opaque_region;
                return translucent_region;
            }
        }

        return damage;
    }

    void schedule_instructions(
        std::vector<render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::region_t& damage) override
    {
        const int padding = std::ceil(
            provider()->calculate_blur_radius() / target.scale);

        auto bbox = self->get_bounding_box();

        // In order to render a part of the blurred background, we need to sample
        // from area which is larger than the damaged area. However, the edges
        // of the expanded area suffer from the same problem (e.g. the blurred
        // background has artifacts). The solution to this is to expand the
        // damage and keep a copy of the pixels where we redraw, but wouldn't
        // have redrawn if not for blur. After that, we copy those old areas
        // back to the destination framebuffer, giving the illusion that they
        // were never damaged.
        auto padded_region = damage & bbox;

        if (is_fully_opaque(padded_region & target.geometry))
        {
            // If there are no regions to blur, we can directly render them.
            for (auto& ch : view_instance)
            {
                ch->schedule_instructions(instructions, target, damage);
            }

            return;
        }

        padded_region.expand_edges(padding);
        padded_region &= bbox;

        // Don't forget to keep expanded damage within the bounds of the render
        // target, otherwise we may be sampling from outside of it (undefined
        // contents).
        padded_region &= target.geometry;

        // Actual region which will be repainted by this render instance.
        wf::region_t we_repaint = padded_region;

        // Subtract original damage, so that we have only the padded region
        padded_region ^= damage;

        for (auto& rect : padded_region)
        {
            saved_pixels_region |= target.framebuffer_box_from_geometry_box(
                wlr_box_from_pixman_box(rect));
        }

        OpenGL::render_begin();
        saved_pixels.allocate(target.viewport_width, target.viewport_height);
        saved_pixels.bind();
        GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, target.fb));

        /* Copy pixels in padded_region from target_fb to saved_pixels. */
        for (const auto& box : saved_pixels_region)
        {
            GL_CALL(glBlitFramebuffer(
                box.x1, target.viewport_height - box.y2,
                box.x2, target.viewport_height - box.y1,
                box.x1, box.y1, box.x2, box.y2,
                GL_COLOR_BUFFER_BIT, GL_LINEAR));
        }

        OpenGL::render_end();

        // Nodes below should re-render the padded areas so that we can sample
        // from them
        damage |= padded_region;
        instructions.push_back(render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = we_repaint,
                });
    }

    void render(const wf::render_target_t& target,
        const wf::region_t& damage) override
    {
        auto tex = get_texture(target.scale);
        auto bounding_box = self->get_bounding_box();
        if (!damage.empty())
        {
            auto translucent_damage = calculate_translucent_damage(target.scale,
                damage);
            provider()->pre_render(bounding_box, translucent_damage, target);
            for (const auto& rect : damage)
            {
                auto damage_box = wlr_box_from_pixman_box(rect);
                provider()->render(tex, bounding_box, damage_box, target);
            }
        }

        OpenGL::render_begin(target);
        // Setup framebuffer I/O. target_fb contains the frame
        // rendered with expanded damage and artifacts on the edges.
        // saved_pixels has the the padded region of pixels to overwrite the
        // artifacts that blurring has left behind.
        GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_pixels.fb));

        /* Copy pixels back from saved_pixels to target_fb. */
        for (const auto& box : saved_pixels_region)
        {
            GL_CALL(glBlitFramebuffer(
                box.x1, box.y1, box.x2, box.y2,
                box.x1, target.viewport_height - box.y2,
                box.x2, target.viewport_height - box.y1,
                GL_COLOR_BUFFER_BIT, GL_LINEAR));
        }

        /* Reset stuff */
        saved_pixels_region.clear();
        OpenGL::render_end();
    }
};

class blur_node_t : public floating_inner_node_t
{
    blur_algorithm_provider provider;

  public:
    blur_node_t(blur_algorithm_provider provider) : floating_inner_node_t(false)
    {
        this->provider = provider;
    }

    std::string stringify() const override
    {
        return "blur";
    }

    void gen_render_instances(std::vector<render_instance_uptr>& instances,
        damage_callback push_damage, const std::optional<wf::geometry_t>&) override
    {
        instances.push_back(std::make_unique<blur_render_instance_t>(
            this, provider, push_damage));
    }
};
}
}

class blur_transform_node_data : public wf::custom_data_t
{
  public:
    wf::scene::floating_inner_node_t *node = nullptr;
};

class blur_global_data_t
{
    // Before doing a render pass, expand the damage by the blur radius.
    // This is needed, because when blurring, the pixels that changed
    // affect a larger area than the really damaged region, e.g. the region
    // that comes from client damage.
    wf::signal::connection_t<wf::scene::render_pass_begin_signal>
    on_render_pass_begin = [=] (wf::scene::render_pass_begin_signal *ev)
    {
        if (!provider)
        {
            return;
        }

        int padding = std::ceil(
            provider()->calculate_blur_radius() / ev->target.scale);

        ev->damage.expand_edges(padding);
        ev->damage &= ev->target.geometry;
    };

  public:
    blur_algorithm_provider provider;
    blur_global_data_t()
    {
        wf::get_core().connect(&on_render_pass_begin);
    }
};

class wayfire_blur : public wf::plugin_interface_t
{
    wf::button_callback button_toggle;
    wf::signal_connection_t view_attached, view_detached;

    wf::view_matcher_t blur_by_default{"blur/blur_by_default"};
    wf::option_wrapper_t<std::string> method_opt{"blur/method"};
    wf::option_wrapper_t<wf::buttonbinding_t> toggle_button{"blur/toggle"};
    wf::config::option_base_t::updated_callback_t blur_method_changed;
    std::unique_ptr<wf_blur_base> blur_algorithm;

    void add_transformer(wayfire_view view)
    {
        if (view->has_data<blur_transform_node_data>())
        {
            return;
        }

        auto provider = [=] ()
        {
            return blur_algorithm.get();
        };

        auto node = std::make_shared<wf::scene::blur_node_t>(provider);
        view->get_data_safe<blur_transform_node_data>()->node = node.get();

        auto children = view->get_transformed_node()->get_children();
        wf::scene::remove_child(view->get_surface_root_node());
        wf::scene::add_front(node, view->get_surface_root_node());

        // Replace content node with blur
        for (auto& x : children)
        {
            if (x == view->get_surface_root_node())
            {
                x = node;
            }
        }

        view->get_transformed_node()->set_children_list(children);
        wf::scene::update(view->get_transformed_node(),
            wf::scene::update_flag::CHILDREN_LIST);
    }

    void pop_transformer(wayfire_view view)
    {
        if (!view->has_data<blur_transform_node_data>())
        {
            return;
        }

        auto node = view->get_data_safe<blur_transform_node_data>()->node;
        view->erase_data<blur_transform_node_data>();

        auto children = view->get_transformed_node()->get_children();
        wf::scene::remove_child(node->shared_from_this());

        node->set_children_list({});

        // Replace content node with blur
        for (auto& x : children)
        {
            if (x.get() == node)
            {
                x = view->get_surface_root_node();
            }
        }

        view->get_transformed_node()->set_children_list(children);
        wf::scene::update(view->get_transformed_node(),
            wf::scene::update_flag::CHILDREN_LIST);
    }

    void remove_transformers()
    {
        for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
        {
            pop_transformer(view);
        }
    }

    wf::shared_data::ref_ptr_t<blur_global_data_t> global_data;

  public:
    void init() override
    {
        grab_interface->name = "blur";
        grab_interface->capabilities = 0;

        blur_method_changed = [=] ()
        {
            blur_algorithm = create_blur_from_name(output, method_opt);
            output->render->damage_whole();
        };
        /* Create initial blur algorithm */
        blur_method_changed();
        method_opt.set_callback(blur_method_changed);

        /* Toggles the blur state of the view the user clicked on */
        button_toggle = [=] (auto)
        {
            if (!output->can_activate_plugin(grab_interface))
            {
                return false;
            }

            auto view = wf::get_core().get_cursor_focus_view();
            if (!view)
            {
                return false;
            }

            if (view->has_data<blur_transform_node_data>())
            {
                pop_transformer(view);
            } else
            {
                add_transformer(view);
            }

            return true;
        };
        output->add_button(toggle_button, &button_toggle);
        global_data->provider = [=] () { return this->blur_algorithm.get(); };

        // Add blur transformers to views which have blur enabled
        view_attached.set_callback([=] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);
            /* View was just created -> we don't know its layer yet */
            if (!view->is_mapped())
            {
                return;
            }

            if (blur_by_default.matches(view))
            {
                add_transformer(view);
            }
        });

        /* If a view is detached, we remove its blur transformer.
         * If it is just moved to another output, the blur plugin
         * on the other output will add its own transformer there */
        view_detached.set_callback([=] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);
            pop_transformer(view);
        });
        output->connect_signal("view-attached", &view_attached);
        output->connect_signal("view-mapped", &view_attached);
        output->connect_signal("view-detached", &view_detached);

        for (auto& view :
             output->workspace->get_views_in_layer(wf::ALL_LAYERS))
        {
            if (blur_by_default.matches(view))
            {
                add_transformer(view);
            }
        }
    }

    void fini() override
    {
        remove_transformers();
        output->rem_binding(&button_toggle);

        /* Call blur algorithm destructor */
        blur_algorithm = nullptr;
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_blur);
