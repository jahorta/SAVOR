#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Model/ProgramModel.h"
#include "Runner/Runtime/ProgramRuntime/Model/ProgramValueArena.h"

#include <limits>
#include <type_traits>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;

ContentHash256 HashFilled(Byte value)
{
    ContentHash256 hash;
    hash.bytes.fill(value);
    return hash;
}

TEST(ProgramModel, StrongIdentifiersRemainNonInterchangeable)
{
    static_assert(!std::is_convertible_v<ProgramFunctionId, ProgramBlockId>);
    static_assert(!std::is_convertible_v<ProgramValueId, ProgramInstructionId>);
    static_assert(!std::is_convertible_v<ProgramScopeId, ProgramResourceHandleId>);
    static_assert(!std::is_convertible_v<WorksetEpoch, ProgramValueId>);

    EXPECT_EQ(ProgramFunctionId(7).value(), 7u);
    EXPECT_NE(ProgramFunctionId(7), ProgramFunctionId(8));
}

TEST(ProgramModel, ContentHashUsesStrictLowercaseHex)
{
    const auto hash = HashFilled(0xab);
    std::string expected;
    for (int index = 0; index < 32; ++index)
        expected += "ab";
    EXPECT_EQ(hash.ToHex(), expected);

    const auto decoded = ContentHash256::FromHex(hash.ToHex());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, hash);

    EXPECT_FALSE(ContentHash256::FromHex(std::string(63, 'a')).has_value());
    EXPECT_FALSE(ContentHash256::FromHex(std::string(64, 'A')).has_value());
    EXPECT_FALSE(ContentHash256::FromHex(std::string(64, 'z')).has_value());
}

TEST(ProgramModel, ValueGraphClosesCompositeReferences)
{
    ProgramValueGraph graph{
        .root = ProgramValueId(3),
        .values = {
            ProgramValue{
                .id = ProgramValueId(1),
                .type = TypeRef::Builtin(BuiltinType::U32),
                .payload = std::uint32_t(42),
            },
            ProgramValue{
                .id = ProgramValueId(2),
                .type = TypeRef::Builtin(BuiltinType::Bool),
                .payload = true,
            },
            ProgramValue{
                .id = ProgramValueId(3),
                .type = TypeRef::Named(SchemaIdentity{
                    .canonical_id = "test.record",
                    .version = 1,
                    .schema_hash = HashFilled(3),
                }),
                .payload = RecordValue{
                    .fields = {ProgramValueId(1), ProgramValueId(2)},
                },
            },
        },
    };

    ASSERT_EQ(graph.values.size(), 3u);
    EXPECT_EQ(std::get<RecordValue>(graph.values.back().payload).fields[0],
              ProgramValueId(1));
}

TEST(ProgramValueArena, ImportsGraphWithFreshIdsAndExportsClosedValues)
{
    ProgramValueArena arena({
        .maximum_values = 8,
        .maximum_value_bytes = 4096,
    });
    const ProgramValueGraph source{
        .root = ProgramValueId(30),
        .values = {
            ProgramValue{
                .id = ProgramValueId(30),
                .type = TypeRef::Named(SchemaIdentity{
                    .canonical_id = "test.record",
                    .version = 1,
                    .schema_hash = HashFilled(3),
                }),
                .payload = RecordValue{
                    .fields = {ProgramValueId(20), ProgramValueId(10)},
                },
            },
            ProgramValue{
                .id = ProgramValueId(10),
                .type = TypeRef::Builtin(BuiltinType::Bool),
                .payload = true,
            },
            ProgramValue{
                .id = ProgramValueId(20),
                .type = TypeRef::Builtin(BuiltinType::U32),
                .payload = std::uint32_t(42),
            },
        },
    };

    const auto imported = arena.Import(source);
    ASSERT_TRUE(imported) << imported.status.message;
    EXPECT_EQ(arena.value_count(), 3u);

    const auto exported = arena.Export(imported.value);
    ASSERT_TRUE(exported) << exported.status.message;
    EXPECT_EQ(exported.graph->values.size(), 3u);
    const auto* root = arena.Lookup(imported.value);
    ASSERT_NE(root, nullptr);
    const auto& record = std::get<RecordValue>(root->payload);
    ASSERT_EQ(record.fields.size(), 2u);
    EXPECT_NE(record.fields[0], ProgramValueId(20));
}

TEST(ProgramValueArena, RejectsCyclesUnreachableValuesAndBudgetsAtomically)
{
    ProgramValueArena arena({
        .maximum_values = 3,
        .maximum_value_bytes = 128,
    });
    ProgramValueGraph cyclic{
        .root = ProgramValueId(1),
        .values = {
            ProgramValue{
                .id = ProgramValueId(1),
                .type = TypeRef::Builtin(BuiltinType::Unit),
                .payload = OptionalValue{ProgramValueId(1)},
            },
        },
    };
    EXPECT_EQ(arena.Import(cyclic).status.error,
              ProgramValueArenaError::CyclicGraph);
    EXPECT_EQ(arena.value_count(), 0u);

    auto unreachable = cyclic;
    unreachable.values[0].payload = UnitValue{};
    unreachable.values.push_back(ProgramValue{
        .id = ProgramValueId(2),
        .type = TypeRef::Builtin(BuiltinType::Unit),
        .payload = UnitValue{},
    });
    EXPECT_EQ(arena.Import(unreachable).status.error,
              ProgramValueArenaError::UnreachableValue);
    EXPECT_EQ(arena.value_count(), 0u);

    const auto too_large = arena.Add(
        TypeRef::Builtin(BuiltinType::Unit),
        std::string(256, 'x'));
    EXPECT_EQ(too_large.status.error,
              ProgramValueArenaError::ByteBudgetExceeded);
    EXPECT_EQ(arena.value_count(), 0u);
}

TEST(ProgramValueArena, BindCloneAndEpochLookupPreserveImmutability)
{
    ProgramValueArena arena({
        .maximum_values = 8,
        .maximum_value_bytes = 4096,
    });
    const auto original = arena.Bind(
        TypeRef::Builtin(BuiltinType::U32),
        std::uint32_t(7));
    ASSERT_TRUE(original);
    const auto clone = arena.CloneWithPayload(
        original.value,
        std::uint32_t(8));
    ASSERT_TRUE(clone);
    EXPECT_EQ(
        std::get<std::uint32_t>(arena.Lookup(original.value)->payload),
        7u);
    EXPECT_EQ(
        std::get<std::uint32_t>(arena.Lookup(clone.value)->payload),
        8u);

    const SchemaIdentity handle_type{
        .canonical_id = "test.handle",
        .version = 1,
        .schema_hash = HashFilled(9),
    };
    const auto handle = arena.Bind(
        TypeRef::Named(handle_type),
        OpaqueHandleValue{
            .handle_id = ProgramResourceHandleId(1),
            .handle_type = handle_type,
            .workset_epoch = WorksetEpoch(4),
        });
    ASSERT_TRUE(handle);
    EXPECT_TRUE(arena.LookupForEpoch(handle.value, WorksetEpoch(4)));
    EXPECT_EQ(
        arena.LookupForEpoch(handle.value, WorksetEpoch(5)).status.error,
        ProgramValueArenaError::StaleEpoch);
}

TEST(ProgramValueArena, RejectsInvalidUtf8AndNonfiniteValues)
{
    ProgramValueArena arena({
        .maximum_values = 8,
        .maximum_value_bytes = 4096,
    });
    EXPECT_EQ(
        arena.Bind(
            TypeRef::Builtin(BuiltinType::Unit),
            std::string("\xc0\xaf", 2)).status.error,
        ProgramValueArenaError::InvalidUtf8);
    EXPECT_EQ(
        arena.Bind(
            TypeRef::Builtin(BuiltinType::F32),
            std::numeric_limits<float>::infinity()).status.error,
        ProgramValueArenaError::NonFiniteValue);
    EXPECT_EQ(arena.value_count(), 0u);
}

TEST(ProgramValueArena, ValidatesNominalGraphsAndNestedEpochs)
{
    const SchemaIdentity handle_payload{
        .canonical_id = "test.resource",
        .version = 1,
        .schema_hash = HashFilled(10),
    };
    const SchemaIdentity handle_schema{
        .canonical_id = "test.resource-handle",
        .version = 1,
        .schema_hash = HashFilled(11),
    };
    const SchemaIdentity record_schema{
        .canonical_id = "test.resource-record",
        .version = 1,
        .schema_hash = HashFilled(12),
    };
    const std::vector<TypeSchemaDefinition> schemas{
        TypeSchemaDefinition{
            .identity = handle_payload,
            .kind = TypeSchemaKind::Record,
        },
        TypeSchemaDefinition{
            .identity = handle_schema,
            .kind = TypeSchemaKind::ResourceHandle,
            .element_type = TypeRef::Named(handle_payload),
        },
        TypeSchemaDefinition{
            .identity = record_schema,
            .kind = TypeSchemaKind::Record,
            .record_fields = {
                RecordFieldDefinition{
                    "resource",
                    TypeRef::Named(handle_schema)},
            },
        },
    };
    const ProgramValueGraph graph{
        .root = ProgramValueId(2),
        .values = {
            ProgramValue{
                .id = ProgramValueId(1),
                .type = TypeRef::Named(handle_schema),
                .payload = ResourceHandleValue{
                    .handle_id = ProgramResourceHandleId(9),
                    .resource_type = handle_payload,
                    .workset_epoch = WorksetEpoch(4),
                },
            },
            ProgramValue{
                .id = ProgramValueId(2),
                .type = TypeRef::Named(record_schema),
                .payload = RecordValue{{ProgramValueId(1)}},
            },
        },
    };

    EXPECT_TRUE(ValidateProgramValueGraph(
        graph,
        TypeRef::Named(record_schema),
        schemas,
        {8, 4096},
        WorksetEpoch(4)));
    EXPECT_EQ(
        ValidateProgramValueGraph(
            graph,
            TypeRef::Named(record_schema),
            schemas,
            {8, 4096},
            WorksetEpoch(5)).error,
        ProgramValueArenaError::SchemaMismatch);
}

TEST(ProgramValueArena, RejectsMalformedSchemasAndBoundaryBudgets)
{
    const SchemaIdentity record_schema{
        .canonical_id = "test.strict-record",
        .version = 1,
        .schema_hash = HashFilled(13),
    };
    const std::vector<TypeSchemaDefinition> schemas{
        TypeSchemaDefinition{
            .identity = record_schema,
            .kind = TypeSchemaKind::Record,
            .record_fields = {
                RecordFieldDefinition{
                    "value",
                    TypeRef::Builtin(BuiltinType::U32)},
            },
        },
    };
    ProgramValueGraph graph{
        .root = ProgramValueId(2),
        .values = {
            ProgramValue{
                .id = ProgramValueId(1),
                .type = TypeRef::Builtin(BuiltinType::Bool),
                .payload = true,
            },
            ProgramValue{
                .id = ProgramValueId(2),
                .type = TypeRef::Named(record_schema),
                .payload = RecordValue{{ProgramValueId(1)}},
            },
        },
    };

    EXPECT_EQ(
        ValidateProgramValueGraph(
            graph,
            TypeRef::Named(record_schema),
            schemas,
            {8, 4096}).error,
        ProgramValueArenaError::TypeMismatch);

    graph.values[0].type = TypeRef::Builtin(BuiltinType::U32);
    graph.values[0].payload = std::uint32_t(7);
    EXPECT_EQ(
        ValidateProgramValueGraph(
            graph,
            TypeRef::Named(record_schema),
            schemas,
            {1, 4096}).error,
        ProgramValueArenaError::ValueBudgetExceeded);
    EXPECT_EQ(
        ValidateProgramValueGraph(
            graph,
            TypeRef::Named(record_schema),
            schemas,
            {8, 1}).error,
        ProgramValueArenaError::ByteBudgetExceeded);

    EXPECT_EQ(
        ValidateProgramValueGraph(
            graph,
            TypeRef::Named(SchemaIdentity{
                .canonical_id = "test.unknown",
                .version = 1,
                .schema_hash = HashFilled(14),
            }),
            schemas,
            {8, 4096}).error,
        ProgramValueArenaError::TypeMismatch);
}

} // namespace
