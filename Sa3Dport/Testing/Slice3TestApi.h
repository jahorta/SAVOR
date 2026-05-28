#pragma once

#include "ObjectData/Node.h"

namespace Sa3Dport::Testing::Slice3 {

using ModelFormat = ObjectData::Enums::ModelFormat;
using Node = ObjectData::Node;
using NodeAttributes = ObjectData::Enums::NodeAttributes;
using NodeGraphValidation = ObjectData::NodeGraphValidation;
using NodePtr = ObjectData::NodePtr;
using NodeReadContext = ObjectData::NodeReadContext;
using RotationUpdateMode = ObjectData::Enums::RotationUpdateMode;

inline NodePtr ReadNode(const Structs::EndianStackReader& reader,
                        std::uint32_t address,
                        ModelFormat format,
                        NodeReadContext& context) {
    return Node::read(reader, address, format, context);
}

} // namespace Sa3Dport::Testing::Slice3
