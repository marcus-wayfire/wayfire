#include <wayfire/scene.hpp>
#include <set>
#include <algorithm>

namespace wf
{
namespace scene
{
node_t::~node_t()
{}

node_t::node_t(bool is_structure)
{
    this->_is_structure = is_structure;
}

inner_node_t::inner_node_t(bool _is_structure) : node_t(_is_structure)
{}

std::optional<input_node_t> inner_node_t::find_node_at(const wf::pointf_t& at)
{
    for (auto& node : get_children())
    {
        auto child_node = node->find_node_at(at);
        if (child_node.has_value())
        {
            return child_node;
        }
    }

    return {};
}

static std::vector<node_t*> extract_structure_nodes(
    const std::vector<node_ptr>& list)
{
    std::vector<node_t*> structure;
    for (auto& node : list)
    {
        if (node->is_structure_node())
        {
            structure.push_back(node.get());
        }
    }

    return structure;
}

bool floating_inner_node_t::set_children_list(std::vector<node_ptr> new_list)
{
    // Structure nodes should be sorted in both sequences and be the same.
    // For simplicity, we just extract the nodes in new vectors and check that
    // they are the same.
    //
    // FIXME: this could also be done with a merge-sort-like algorithm in place,
    // but is it worth it here? The scenegraph is supposed to stay static for
    // most of the time.
    if (extract_structure_nodes(children) != extract_structure_nodes(new_list))
    {
        return false;
    }

    set_children_unchecked(std::move(new_list));
    return true;
}

void inner_node_t::set_children_unchecked(std::vector<node_ptr> new_list)
{
    for (auto& node : new_list)
    {
        node->_parent = this;
    }

    this->children = std::move(new_list);
}

// FIXME: output nodes are actually structure nodes, but we need to add and
// remove them dynamically ...
output_node_t::output_node_t() : inner_node_t(false)
{
    this->_static = std::make_shared<floating_inner_node_t>(true);
    this->dynamic = std::make_shared<floating_inner_node_t>(true);
    set_children_unchecked({dynamic, _static});
}

root_node_t::root_node_t() : inner_node_t(true)
{
    std::vector<node_ptr> children;

    for (int i = (int)layer::ALL_LAYERS - 1; i >= 0; i--)
    {
        layers[i] = std::make_shared<floating_inner_node_t>(true);
        children.push_back(layers[i]);
    }

    set_children_unchecked(children);
}
} // namespace scene
}
