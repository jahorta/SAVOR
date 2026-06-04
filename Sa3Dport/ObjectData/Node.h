#pragma once

#include "ObjectData/Enums/ModelFormat.h"
#include "ObjectData/Enums/NodeAttributes.h"
#include "ObjectData/Enums/RotationUpdateMode.h"
#include "Mesh/AttachDispatch.h"
#include "Structs/EndianStackReader.h"
#include "Structs/EndianIOExtensions.h"
#include "Structs/FloatIOType.h"
#include "Structs/Matrix4x4.h"
#include "Structs/MatrixUtilities.h"
#include "Structs/PointerIO.h"
#include "Structs/Quaternion.h"
#include "Structs/QuaternionUtilities.h"
#include "Structs/Vector3.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Sa3Dport::ObjectData {

class Node;
using NodePtr = std::shared_ptr<Node>;

struct NodeReadContext {
    std::uint32_t image_base = 0;
    bool read_attach = false;
    Sa3Dport::Mesh::AttachReadContext attach_context {};
    std::unordered_map<std::uint32_t, NodePtr> nodes;
};

struct NodeGraphValidation {
    bool ok = true;
    std::vector<std::string> diagnostics;
};

class Node : public std::enable_shared_from_this<Node> {
public:
    static constexpr std::uint32_t StructSize = 0x34;

    std::string label = "object_";
    Enums::NodeAttributes attributes = Enums::NodeAttributes::None;
    std::uint32_t source_address = 0;
    std::uint32_t attach_address = 0;
    Sa3Dport::Mesh::AttachPtr attach;
    Structs::Vector3 position = Structs::Vector3::zero();
    Structs::Vector3 euler_rotation = Structs::Vector3::zero();
    Structs::Quaternion quaternion_rotation = Structs::Quaternion::identity();
    Structs::Vector3 scale = Structs::Vector3::one();
    Structs::Matrix4x4 local_matrix = Structs::identity();

    [[nodiscard]] NodePtr child() const { return child_; }
    [[nodiscard]] NodePtr parent() const { return parent_.lock(); }
    [[nodiscard]] NodePtr next() const { return next_; }
    [[nodiscard]] NodePtr previous() const { return previous_.lock(); }

    [[nodiscard]] bool has_attribute(Enums::NodeAttributes attribute) const {
        return Enums::has_flag(attributes, attribute);
    }

    void set_attribute(Enums::NodeAttributes attribute, bool state) {
        if (state) {
            attributes |= attribute;
        } else {
            attributes &= ~attribute;
        }
    }

    [[nodiscard]] bool no_position() const { return has_attribute(Enums::NodeAttributes::NoPosition); }
    [[nodiscard]] bool no_rotation() const { return has_attribute(Enums::NodeAttributes::NoRotation); }
    [[nodiscard]] bool no_scale() const { return has_attribute(Enums::NodeAttributes::NoScale); }
    [[nodiscard]] bool skip_draw() const { return has_attribute(Enums::NodeAttributes::SkipDraw); }
    [[nodiscard]] bool skip_children() const { return has_attribute(Enums::NodeAttributes::SkipChildren); }
    [[nodiscard]] bool rotate_zyx() const { return has_attribute(Enums::NodeAttributes::RotateZYX); }
    [[nodiscard]] bool no_animate() const { return has_attribute(Enums::NodeAttributes::NoAnimate); }
    [[nodiscard]] bool no_morph() const { return has_attribute(Enums::NodeAttributes::NoMorph); }
    [[nodiscard]] bool clip() const { return has_attribute(Enums::NodeAttributes::Clip); }
    [[nodiscard]] bool modifier() const { return has_attribute(Enums::NodeAttributes::Modifier); }
    [[nodiscard]] bool use_quaternion_rotation() const { return has_attribute(Enums::NodeAttributes::UseQuaternionRotation); }
    [[nodiscard]] bool cache_rotation() const { return has_attribute(Enums::NodeAttributes::CacheRotation); }
    [[nodiscard]] bool apply_cached_rotation() const { return has_attribute(Enums::NodeAttributes::ApplyCachedRotation); }
    [[nodiscard]] bool envelope() const { return has_attribute(Enums::NodeAttributes::Envelope); }

    void set_all_node_attributes(Enums::NodeAttributes value,
                                 Enums::RotationUpdateMode mode = Enums::RotationUpdateMode::UpdateEuler) {
        const bool oldRotateZYX = rotate_zyx();
        attributes = value;
        if (oldRotateZYX != rotate_zyx() && mode != Enums::RotationUpdateMode::Keep) {
            update_transforms(std::nullopt, std::nullopt, std::nullopt, std::nullopt, mode);
        }
    }

    void update_transforms(std::optional<Structs::Vector3> newPosition,
                           std::optional<Structs::Vector3> newEulerRotation,
                           std::optional<Structs::Quaternion> newQuaternionRotation,
                           std::optional<Structs::Vector3> newScale,
                           Enums::RotationUpdateMode mode = Enums::RotationUpdateMode::Keep) {
        bool updated = false;
        if (newPosition.has_value()) {
            position = *newPosition;
            updated = true;
        }

        if (newEulerRotation.has_value()) {
            euler_rotation = *newEulerRotation;
            quaternion_rotation = Structs::QuaternionUtilities::euler_to_quaternion(euler_rotation, rotate_zyx());
            updated = true;
        }

        if (newQuaternionRotation.has_value()) {
            quaternion_rotation = *newQuaternionRotation;
            euler_rotation = Structs::QuaternionUtilities::quaternion_to_euler(quaternion_rotation, rotate_zyx());
            updated = true;
        }

        if (newScale.has_value()) {
            scale = *newScale;
            updated = true;
        }

        switch (mode) {
        case Enums::RotationUpdateMode::UpdateQuaternion:
            quaternion_rotation = Structs::QuaternionUtilities::euler_to_quaternion(euler_rotation, rotate_zyx());
            break;
        case Enums::RotationUpdateMode::UpdateEuler:
            euler_rotation = Structs::QuaternionUtilities::quaternion_to_euler(quaternion_rotation, rotate_zyx());
            break;
        case Enums::RotationUpdateMode::Keep:
        default:
            break;
        }

        if (updated) {
            local_matrix = Structs::MatrixUtilities::create_transform_matrix(position, quaternion_rotation, scale);
        }
    }

    void detach() {
        const auto parentNode = parent();
        const auto previousNode = previous();

        if (previousNode) {
            previousNode->next_ = next_;
        } else if (parentNode) {
            parentNode->child_ = next_;
        }

        if (next_) {
            next_->previous_ = previous_;
        }

        parent_.reset();
        previous_.reset();
        next_.reset();
    }

    void detach_children(bool remainSiblings) {
        NodePtr current = child_;
        while (current) {
            NodePtr nextNode = current->next_;
            current->parent_.reset();
            if (!remainSiblings) {
                current->previous_.reset();
                current->next_.reset();
            }
            current = nextNode;
        }
        child_.reset();
    }

    void detach_successors(bool remainSiblings) {
        NodePtr current = next_;
        while (current) {
            NodePtr nextNode = current->next_;
            current->parent_.reset();
            if (!remainSiblings) {
                current->previous_.reset();
                current->next_.reset();
            }
            current = nextNode;
        }
        if (next_) {
            next_->previous_.reset();
        }
        next_.reset();
    }

    void append_child(const NodePtr& node) {
        if (!node) {
            return;
        }

        if (!child_) {
            node->detach();
            node->parent_ = shared_from_this();
            child_ = node;
            return;
        }

        NodePtr target = child_;
        while (target->next_) {
            target = target->next_;
        }
        target->insert_after(node);
    }

    void insert_after(const NodePtr& node) {
        if (!node) {
            return;
        }

        node->detach();
        if (next_) {
            next_->previous_ = node;
        }

        node->parent_ = parent_;
        node->previous_ = shared_from_this();
        node->next_ = next_;
        next_ = node;
    }

    void set_child(const NodePtr& node) {
        if (child_ == node) {
            return;
        }
        if (node && node->previous()) {
            throw std::invalid_argument("child root sibling cannot have previous node");
        }

        detach_children(true);
        if (!node) {
            return;
        }

        NodePtr current = node;
        while (current) {
            current->parent_ = shared_from_this();
            current = current->next_;
        }
        child_ = node;
    }

    void set_next(const NodePtr& node) {
        if (next_ == node) {
            return;
        }
        if (node && node->parent()) {
            throw std::invalid_argument("successor cannot already have a parent");
        }

        detach_successors(true);
        if (!node) {
            return;
        }

        const auto parentNode = parent();
        if (parentNode) {
            NodePtr current = node;
            while (current) {
                current->parent_ = parentNode;
                current = current->next_;
            }
        }

        next_ = node;
        node->previous_ = shared_from_this();
    }

    [[nodiscard]] std::vector<NodePtr> direct_children() const {
        std::vector<NodePtr> result;
        NodePtr current = child_;
        while (current) {
            result.push_back(current);
            current = current->next_;
        }
        return result;
    }

    [[nodiscard]] std::vector<NodePtr> branch_nodes(bool includeSiblings) const {
        std::vector<NodePtr> result;
        std::vector<NodePtr> stack;

        if (!includeSiblings) {
            result.push_back(const_cast<Node*>(this)->shared_from_this());
            if (child_) {
                stack.push_back(child_);
            }
        } else {
            stack.push_back(root_sibling());
        }

        while (!stack.empty()) {
            NodePtr node = stack.back();
            stack.pop_back();
            result.push_back(node);
            if (node->next_) {
                stack.push_back(node->next_);
            }
            if (node->child_) {
                stack.push_back(node->child_);
            }
        }

        return result;
    }

    [[nodiscard]] std::vector<NodePtr> tree_nodes() const {
        return root_parent()->branch_nodes(true);
    }

    [[nodiscard]] std::size_t tree_node_count() const {
        return tree_nodes().size();
    }

    [[nodiscard]] NodeGraphValidation validate_graph() const {
        NodeGraphValidation result;
        std::unordered_set<const Node*> seen;
        std::vector<NodePtr> stack;
        stack.push_back(const_cast<Node*>(this)->shared_from_this());

        while (!stack.empty()) {
            NodePtr node = stack.back();
            stack.pop_back();
            if (!seen.insert(node.get()).second) {
                result.ok = false;
                result.diagnostics.push_back("cycle_or_duplicate_node");
                continue;
            }

            if (node->child_ && node->child_->previous()) {
                result.ok = false;
                result.diagnostics.push_back("child_has_previous");
            }

            if (node->next_ && node->next_->previous().get() != node.get()) {
                result.ok = false;
                result.diagnostics.push_back("next_previous_mismatch");
            }

            if (node->child_) {
                if (node->child_->parent().get() != node.get()) {
                    result.ok = false;
                    result.diagnostics.push_back("child_parent_mismatch");
                }
                stack.push_back(node->child_);
            }

            if (node->next_) {
                if (node->next_->parent().get() != node->parent().get()) {
                    result.ok = false;
                    result.diagnostics.push_back("sibling_parent_mismatch");
                }
                stack.push_back(node->next_);
            }
        }

        return result;
    }

    [[nodiscard]] NodePtr root_parent() const {
        NodePtr root = const_cast<Node*>(this)->shared_from_this();
        while (root->parent()) {
            root = root->parent();
        }
        return root;
    }

    [[nodiscard]] NodePtr root_sibling() const {
        NodePtr root = const_cast<Node*>(this)->shared_from_this();
        while (root->previous()) {
            root = root->previous();
        }
        return root;
    }

    [[nodiscard]] static NodePtr read(const Structs::EndianStackReader& reader,
                                      std::uint32_t address,
                                      Enums::ModelFormat format,
                                      NodeReadContext& context) {
        (void)format;
        if (const auto cached = context.nodes.find(address); cached != context.nodes.end()) {
            return cached->second;
        }

        auto result = std::make_shared<Node>();
        result->source_address = address;
        result->label = "object_0x" + hex_address(address);
        context.nodes.emplace(address, result);

        const auto attributes = static_cast<Enums::NodeAttributes>(reader.read_u32(address));
        result->set_all_node_attributes(attributes, Enums::RotationUpdateMode::Keep);
        if (const auto attachAddress = read_pointer(reader, address + 4u, context.image_base)) {
            result->attach_address = *attachAddress;
            if (context.read_attach) {
                context.attach_context.image_base = context.image_base;
                result->attach = Sa3Dport::Mesh::Attach::read(reader, *attachAddress, format, context.attach_context);
            }
        }

        std::uint32_t cursor = address + 8u;
        const Structs::Vector3 position = Structs::EndianIOExtensions::read_vector3(reader, cursor);
        std::optional<Structs::Vector3> eulerRotation;
        std::optional<Structs::Quaternion> quaternionRotation;
        if (result->use_quaternion_rotation()) {
            const Structs::Vector3 vectorPart = Structs::EndianIOExtensions::read_vector3(reader, cursor);
            const float scalarPart = reader.read_float(cursor + 20u);
            quaternionRotation = Structs::Quaternion(vectorPart.x, vectorPart.y, vectorPart.z, scalarPart);
        } else {
            eulerRotation = Structs::EndianIOExtensions::read_vector3(reader, cursor, Structs::FloatIOType::BAMS32);
        }

        const Structs::Vector3 scale = Structs::EndianIOExtensions::read_vector3(reader, cursor);
        result->update_transforms(position, eulerRotation, quaternionRotation, scale, Enums::RotationUpdateMode::Keep);

        if (const auto childAddress = read_pointer(reader, cursor, context.image_base)) {
            result->set_child(read(reader, *childAddress, format, context));
        }

        if (const auto siblingAddress = read_pointer(reader, cursor + 4u, context.image_base)) {
            result->set_next(read(reader, *siblingAddress, format, context));
        }

        return result;
    }

private:
    static std::optional<std::uint32_t> read_pointer(const Structs::EndianStackReader& reader,
                                                     std::uint32_t address,
                                                     std::uint32_t imageBase) {
        return Structs::PointerIO::read_nullable_pointer_subtract_base(reader, address, imageBase);
    }

    static std::string hex_address(std::uint32_t address) {
        constexpr char digits[] = "0123456789abcdef";
        std::string result(8, '0');
        for (int i = 7; i >= 0; --i) {
            result[static_cast<std::size_t>(i)] = digits[address & 0xFu];
            address >>= 4;
        }
        return result;
    }

    NodePtr child_;
    std::weak_ptr<Node> parent_;
    NodePtr next_;
    std::weak_ptr<Node> previous_;
};

} // namespace Sa3Dport::ObjectData
