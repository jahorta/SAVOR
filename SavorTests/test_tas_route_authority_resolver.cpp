#include "Analysis/TasRouteAuthorityResolver.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <string>

namespace {

class SqliteMemoryDb {
public:
    SqliteMemoryDb() { EXPECT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    ~SqliteMemoryDb() { if (db_ != nullptr) sqlite3_close(db_); }

    sqlite3* get() const { return db_; }

    void exec(const char* sql)
    {
        char* error = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        const std::string diagnostic = error != nullptr ? error : "";
        sqlite3_free(error);
        ASSERT_EQ(rc, SQLITE_OK) << diagnostic;
    }

private:
    sqlite3* db_ = nullptr;
};

void CreateExecutionSchema(SqliteMemoryDb& db)
{
    db.exec(
        "CREATE TABLE exec_workflow_instance_input_binding("
        "workflow_instance_input_binding_id INTEGER PRIMARY KEY,"
        "workflow_instance_id INTEGER NOT NULL,ref_kind TEXT NOT NULL,ref_id INTEGER NOT NULL);"
        "CREATE TABLE exec_workflow_expansion("
        "workflow_expansion_id INTEGER PRIMARY KEY,source_ref_kind TEXT,source_ref_id INTEGER);"
        "CREATE TABLE exec_workflow_expansion_member("
        "workflow_expansion_member_id INTEGER PRIMARY KEY,workflow_expansion_id INTEGER,"
        "workflow_instance_id INTEGER);"
    );
}

void CreateAnalysisSchema(SqliteMemoryDb& db)
{
    db.exec(
        "CREATE TABLE tmv_input_epoch_annotation_attempt("
        "annotation_attempt_id INTEGER PRIMARY KEY,succeeded INTEGER NOT NULL,"
        "root_establishment_attempt_id INTEGER);"
        "CREATE TABLE atr_route_node(route_node_id INTEGER PRIMARY KEY,root_establishment_attempt_id INTEGER);"
        "CREATE TABLE atr_victory_branch(checkpoint_savestate_id INTEGER,checkpoint_route_node_id INTEGER);"
    );
}

} // namespace

TEST(TasRouteAuthorityResolver, UsesWorkflowBindingInsteadOfSharedExpansionMembership)
{
    SqliteMemoryDb execution;
    SqliteMemoryDb analysis;
    CreateExecutionSchema(execution);
    CreateAnalysisSchema(analysis);
    execution.exec(
        "INSERT INTO exec_workflow_instance_input_binding VALUES(1,41,'tmv_input_epoch_annotation_attempt',11);"
        "INSERT INTO exec_workflow_expansion VALUES(1,'tmv_input_epoch_annotation_attempt',21);"
        "INSERT INTO exec_workflow_expansion VALUES(2,'tmv_input_epoch_annotation_attempt',22);"
        "INSERT INTO exec_workflow_expansion_member VALUES(101,1,41);"
        "INSERT INTO exec_workflow_expansion_member VALUES(102,2,41);"
    );
    analysis.exec("INSERT INTO tmv_input_epoch_annotation_attempt VALUES(11,1,101);");

    const auto resolved = savor::db::analysis::ResolveTasRouteRootAuthority(
        execution.get(), analysis.get(), 41, 900);
    ASSERT_TRUE(resolved.root_establishment_attempt_id.has_value()) << resolved.diagnostic;
    EXPECT_EQ(*resolved.root_establishment_attempt_id, 101);
}

TEST(TasRouteAuthorityResolver, RejectsConflictingWorkflowBindings)
{
    SqliteMemoryDb execution;
    SqliteMemoryDb analysis;
    CreateExecutionSchema(execution);
    CreateAnalysisSchema(analysis);
    execution.exec(
        "INSERT INTO exec_workflow_instance_input_binding VALUES(1,41,'tmv_input_epoch_annotation_attempt',11);"
        "INSERT INTO exec_workflow_instance_input_binding VALUES(2,41,'tmv_input_epoch_annotation_attempt',12);"
    );
    analysis.exec(
        "INSERT INTO tmv_input_epoch_annotation_attempt VALUES(11,1,101);"
        "INSERT INTO tmv_input_epoch_annotation_attempt VALUES(12,1,102);"
    );

    const auto resolved = savor::db::analysis::ResolveTasRouteRootAuthority(
        execution.get(), analysis.get(), 41, 900);
    EXPECT_FALSE(resolved.root_establishment_attempt_id.has_value());
    EXPECT_NE(resolved.diagnostic.find("conflicting"), std::string::npos);
}
