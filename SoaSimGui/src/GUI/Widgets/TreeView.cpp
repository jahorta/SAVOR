#include "TreeView.h"
#include "imgui.h"

void TreeView::Draw(const Node* n)
{
    if (ImGui::TreeNode(n->name.c_str())) { // Creates a collapsible tree node
        for (const Node& child : n->children) {
            Draw(&child); // Recursively call for children
        }
        ImGui::TreePop(); // Must be called after the children are drawn
    }
}

template <typename T>
void TreeView::DrawEx(const NodeEx<T>* n)
{
    if (ImGui::TreeNode(n->name.c_str())) { // Creates a collapsible tree node
        for (const Node& child : n->children) {
            Draw(&child); // Recursively call for children
        }
        ImGui::TreePop(); // Must be called after the children are drawn
    }
}
