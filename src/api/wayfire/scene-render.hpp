#pragma once

#include <vector>
#include <wayfire/region.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/opengl.hpp>

namespace wf
{
namespace scene
{

class render_instance_t;

/**
 * A single rendering call in a render pass.
 */
struct render_instruction_t
{
    render_instance_t *instance = NULL;
    wf::region_t damage;
    wf::render_target_t target;
};

/**
 * When (parts) of the scenegraph have to be rendered, they have to be
 * 'instantiated' first. The instantiation of a (sub)tree of the scenegraph
 * is a tree of render instances, called a render tree. The purpose of the
 * render trees is to enable damage tracking (each render instance has its own
 * damage), while allowing arbitrary transformations in the scenegraph (e.g. a
 * render instance does not need to export information about how it transforms
 * its children). Due to this design, render trees have to be regenerated every
 * time the relevant portion of the scenegraph changes.
 *
 * Actually painting a render tree (called render pass) is a process involving
 * three steps:
 *
 * 1. A back-to-front iteration through the render tree to calculate the overall
 *   damaged region of the render tree.
 * 2. A front-to-back iteration through the render tree, so that every node
 *   calculates the parts of the destination buffer it should actually repaint.
 * 3. A final back-to-front iteration where the actual rendering happens.
 */
class render_instance_t
{
  public:
    virtual ~render_instance_t() = default;

    /**
     * Handle the first back-to-front iteration in a render pass.
     * Each render instance should add the region of damage for it and its
     * children to @accumulated_damage. It may also subtract from the damaged
     * region, if for example an opaque part of it covers already damaged areas.
     *
     * After collecting the damaged region, the render instance should 'reset'
     * the damage it has internally accumulated so far (but the damage should
     * remain in other render instances of the same node!).
     */
    virtual void collect_damage(wf::region_t& accumulated_damage) = 0;

    /**
     * Handle the second front-to-back iteration from a render pass.
     * Each instance should add the render instructions (calls to
     * render_instance_t::render()) for itself and its children.
     *
     * @param instructions A list of render instructions to be executed.
     *   For efficiency, instructions are evaluated in the reverse order they
     *   are pushed (e.g. from instructions.rbegin() to instructions.rend()).
     * @param damage The damaged region of the node, in node-local coordinates.
     * @param fb The target framebuffer to render the node and its children.
     *   Note that some nodes may cause their children to be rendered to
     *   auxilliary buffers.
     */
    virtual void schedule_instructions(
        std::vector<render_instruction_t>& instructions,
        wf::region_t& damage, const wf::framebuffer_t& fb) {}
};
}
}
