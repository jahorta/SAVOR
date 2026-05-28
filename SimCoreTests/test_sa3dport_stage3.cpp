#include "gtest/gtest.h"

#include "Testing/Slice3TestApi.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace Sa3Dport::Testing::Slice3;
namespace S = Sa3Dport::Structs;

void WriteU32(std::vector<std::byte>& data, std::uint32_t offset, std::uint32_t value) {
    if (data.size() < offset + 4u) {
        data.resize(offset + 4u);
    }
    data[offset] = std::byte(value & 0xFFu);
    data[offset + 1u] = std::byte((value >> 8u) & 0xFFu);
    data[offset + 2u] = std::byte((value >> 16u) & 0xFFu);
    data[offset + 3u] = std::byte((value >> 24u) & 0xFFu);
}

void WriteF32(std::vector<std::byte>& data, std::uint32_t offset, float value) {
    WriteU32(data, offset, std::bit_cast<std::uint32_t>(value));
}

void WriteBams32(std::vector<std::byte>& data, std::uint32_t offset, float radians) {
    WriteU32(data, offset, static_cast<std::uint32_t>(S::MathHelper::rad_to_bams(radians)));
}

void WriteVec3(std::vector<std::byte>& data, std::uint32_t offset, S::Vector3 value) {
    WriteF32(data, offset, value.x);
    WriteF32(data, offset + 4u, value.y);
    WriteF32(data, offset + 8u, value.z);
}

void WriteNode(std::vector<std::byte>& data,
               std::uint32_t offset,
               std::uint32_t attributes,
               std::uint32_t child,
               std::uint32_t next,
               S::Vector3 position = {},
               S::Vector3 rotation = {},
               S::Vector3 scale = S::Vector3::one()) {
    WriteU32(data, offset, attributes);
    WriteU32(data, offset + 4u, 0);
    WriteVec3(data, offset + 8u, position);
    WriteBams32(data, offset + 20u, rotation.x);
    WriteBams32(data, offset + 24u, rotation.y);
    WriteBams32(data, offset + 28u, rotation.z);
    WriteVec3(data, offset + 32u, scale);
    WriteU32(data, offset + 44u, child);
    WriteU32(data, offset + 48u, next);
    WriteU32(data, offset + 52u, 0);
}

TEST(Sa3DportStage3, ReadsNodeTreeAndMaintainsRelationships) {
    std::vector<std::byte> data(0x34 * 3);
    WriteNode(data, 0x00, 0, 0x34, 0, {1.0f, 2.0f, 3.0f});
    WriteNode(data, 0x34, static_cast<std::uint32_t>(NodeAttributes::NoAnimate), 0, 0x68, {4.0f, 5.0f, 6.0f});
    WriteNode(data, 0x68, static_cast<std::uint32_t>(NodeAttributes::NoMorph), 0, 0, {7.0f, 8.0f, 9.0f});

    S::EndianStackReader reader(data, S::Endian::Little);
    NodeReadContext context;
    const auto root = ReadNode(reader, 0, ModelFormat::SA2, context);

    ASSERT_NE(root, nullptr);
    ASSERT_NE(root->child(), nullptr);
    ASSERT_NE(root->child()->next(), nullptr);
    EXPECT_EQ(root->tree_node_count(), 3u);
    EXPECT_EQ(root->child()->parent().get(), root.get());
    EXPECT_EQ(root->child()->next()->previous().get(), root->child().get());
    EXPECT_TRUE(root->child()->no_animate());
    EXPECT_TRUE(root->child()->next()->no_morph());
    EXPECT_TRUE(root->validate_graph().ok);
}

TEST(Sa3DportStage3, ReadsEulerTransformsFromBams32) {
    std::vector<std::byte> data(0x34);
    WriteNode(data, 0, 0, 0, 0, {1.0f, 2.0f, 3.0f}, {S::MathHelper::HalfPi, 0.0f, 0.0f}, {2.0f, 3.0f, 4.0f});

    S::EndianStackReader reader(data, S::Endian::Little);
    NodeReadContext context;
    const auto node = ReadNode(reader, 0, ModelFormat::SA2, context);

    EXPECT_FLOAT_EQ(node->position.x, 1.0f);
    EXPECT_FLOAT_EQ(node->scale.z, 4.0f);
    EXPECT_NEAR(node->euler_rotation.x, S::MathHelper::HalfPi, 0.001f);
    EXPECT_NEAR(node->quaternion_rotation.x, std::sin(S::MathHelper::HalfPi * 0.5f), 0.001f);
}

TEST(Sa3DportStage3, ReadsQuaternionRotationLayout) {
    std::vector<std::byte> data(0x34);
    const auto attrs = static_cast<std::uint32_t>(NodeAttributes::UseQuaternionRotation);
    WriteU32(data, 0, attrs);
    WriteU32(data, 4, 0);
    WriteVec3(data, 8, {});
    WriteF32(data, 20, 0.0f);
    WriteF32(data, 24, std::sin(S::MathHelper::HalfPi * 0.5f));
    WriteF32(data, 28, 0.0f);
    WriteVec3(data, 32, S::Vector3::one());
    WriteU32(data, 44, 0);
    WriteU32(data, 48, 0);
    WriteF32(data, 52, std::cos(S::MathHelper::HalfPi * 0.5f));

    S::EndianStackReader reader(data, S::Endian::Little);
    NodeReadContext context;
    const auto node = ReadNode(reader, 0, ModelFormat::SA2, context);

    EXPECT_TRUE(node->use_quaternion_rotation());
    EXPECT_NEAR(node->quaternion_rotation.y, std::sin(S::MathHelper::HalfPi * 0.5f), 0.001f);
    EXPECT_NEAR(node->quaternion_rotation.w, std::cos(S::MathHelper::HalfPi * 0.5f), 0.001f);
    EXPECT_NEAR(node->euler_rotation.y, S::MathHelper::HalfPi, 0.001f);
}

TEST(Sa3DportStage3, SetChildRequiresRootSiblingAndAssignsParentAcrossSiblings) {
    const auto root = std::make_shared<Node>();
    const auto child = std::make_shared<Node>();
    const auto sibling = std::make_shared<Node>();
    child->set_next(sibling);

    root->set_child(child);

    EXPECT_EQ(child->parent().get(), root.get());
    EXPECT_EQ(sibling->parent().get(), root.get());
    EXPECT_TRUE(root->validate_graph().ok);
}

} // namespace
