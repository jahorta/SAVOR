#include "ExplorerRunsTurnInputsDialog.h"

#include "ExplorerRunsJobsTableModel.h"

#include "Core/Input/AppliedTurnTapeBlob.h"
#include "DB/DBCore/ObjectStore.h"
#include "DB/ProgramDB/BattleSingleTurnRunDBCodec.h"
#include "DB/Scheduling/JobEventsRepo.h"
#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobSetsRepo.h"
#include "Core/Input/InputPlanFmt.h"
#include "Utils/IniDoc.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <optional>
#include <unordered_set>

using namespace simcore::db;
using namespace simcore::db::codec::battle::singleturn;

namespace {

struct TurnInputNode {
    qint64 jobSetId = 0;
    qint64 jobId = 0;
    quint32 turnNumber = 0;
    std::vector<QString> inputLines;
};

QString makeTurnNodeLabel(const TurnInputNode& node)
{
    return QStringLiteral("job_set_id=%1 · job_id=%2 · turn=%3")
        .arg(node.jobSetId)
        .arg(node.jobId)
        .arg(node.turnNumber);
}

std::optional<JobIni> readSingleTurnJob(qint64 jobId)
{
    auto jobResult = JobsRepo::Get(jobId);
    if (!jobResult.ok || !jobResult.value.vm_kv.has_value()) {
        return std::nullopt;
    }
    return JobIni::from_section(IniDoc::parse(*jobResult.value.vm_kv));
}

std::optional<ResultsIni> readSingleTurnResults(qint64 jobId)
{
    auto payload = JobEventsRepo::GetLatestPayload(jobId, "RESULTS");
    if (!payload.ok || !payload.value.has_value()) {
        return std::nullopt;
    }
    IniDoc doc = IniDoc::parse(*payload.value);
    if (!doc.has_section(ResultsIni::SECTION_NAME)) {
        return std::nullopt;
    }
    return ResultsIni::from_section(doc);
}

std::optional<simcore::inputtape::TurnChunk> selectChunkForTurn(
    const std::vector<simcore::inputtape::TurnChunk>& chunks,
    quint32 turnNumber)
{
    for (const auto& chunk : chunks) {
        if (chunk.turn_number == turnNumber) {
            return chunk;
        }
    }
    if (chunks.empty()) {
        return std::nullopt;
    }
    return chunks.back();
}

std::vector<QString> renderInputsForChunk(const simcore::inputtape::TurnChunk& chunk)
{
    struct InputLine {
        quint32 viStart = 0;
        QString text;
    };

    std::vector<InputLine> lines;
    lines.reserve(chunk.frames.size());
    quint32 viCursor = chunk.vi_start;
    for (size_t i = 0; i < chunk.frames.size(); ++i) {
        const quint32 viDur = i < chunk.vi_durations.size() ? chunk.vi_durations[i] : 0u;
        lines.push_back(InputLine{
            viCursor,
            QStringLiteral("vi_start=%1 vi_dur=%2 : %3")
                .arg(viCursor)
                .arg(viDur)
                .arg(QString::fromStdString(DescribeFrameCompact(chunk.frames[i])))
        });
        viCursor += viDur;
    }

    std::sort(lines.begin(), lines.end(), [](const InputLine& a, const InputLine& b) {
        return a.viStart < b.viStart;
    });

    std::vector<QString> out;
    out.reserve(lines.size());
    for (const InputLine& line : lines) {
        out.push_back(line.text);
    }
    return out;
}

std::optional<TurnInputNode> buildTurnInputNode(qint64 jobId)
{
    const auto jobIni = readSingleTurnJob(jobId);
    const auto resultsIni = readSingleTurnResults(jobId);
    auto jobRow = JobsRepo::Get(jobId);
    if (!jobIni.has_value() || !resultsIni.has_value() || !jobRow.ok) {
        return std::nullopt;
    }
    if (resultsIni->applied_input_artifact_id <= 0) {
        return std::nullopt;
    }

    auto artifactText = ObjectStore::GetText(resultsIni->applied_input_artifact_id);
    if (!artifactText.ok) {
        return std::nullopt;
    }

    std::vector<simcore::inputtape::TurnChunk> chunks;
    if (!simcore::inputtape::decode_turn_chunks(artifactText.value, chunks)) {
        return std::nullopt;
    }

    auto chunk = selectChunkForTurn(chunks, jobIni->turn_index);
    if (!chunk.has_value()) {
        return std::nullopt;
    }

    TurnInputNode node{};
    node.jobSetId = jobRow.value.job_set_id;
    node.jobId = jobId;
    node.turnNumber = jobIni->turn_index;
    node.inputLines = renderInputsForChunk(*chunk);
    return node;
}

std::optional<qint64> resolveParentJobId(qint64 childJobId)
{
    auto childJobRow = JobsRepo::Get(childJobId);
    if (!childJobRow.ok || !childJobRow.value.vm_kv.has_value()) {
        return std::nullopt;
    }
    if (childJobRow.value.parent_job_id.has_value() && *childJobRow.value.parent_job_id > 0) {
        return childJobRow.value.parent_job_id;
    }
    const JobIni childJob = JobIni::from_section(IniDoc::parse(*childJobRow.value.vm_kv));
    if (childJob.turn_index <= 1) {
        return std::nullopt;
    }

    auto jobSet = JobSetsRepo::GetParent(childJobRow.value.job_set_id);
    if (!jobSet.ok) {
        return std::nullopt;
    }

    auto candidates = JobsRepo::GetByJobSet(*jobSet.value);
    if (!candidates.ok || candidates.value.empty()) {
        return std::nullopt;
    }

    std::vector<qint64> matched;
    for (const auto& parent : candidates.value) {
        if (!parent.vm_kv.has_value()) {
            continue;
        }
        const JobIni parentJob = JobIni::from_section(IniDoc::parse(*parent.vm_kv));
        if (parentJob.turn_index + 1 != childJob.turn_index || parentJob.delta_seed_id != childJob.delta_seed_id) {
            continue;
        }

        const auto parentResults = readSingleTurnResults(parent.job_id);
        if (!parentResults.has_value()) {
            continue;
        }
        if (parentResults->output_savestate_id != childJob.savestate_id) {
            continue;
        }
        if (parentResults->fake_attacks_used != childJob.fake_attacks_used_before) {
            continue;
        }
        matched.push_back(parent.job_id);
    }

    if (matched.empty()) {
        return std::nullopt;
    }
    std::sort(matched.begin(), matched.end());
    return matched.front();
}

}

ExplorerRunsTurnInputsDialog::ExplorerRunsTurnInputsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("View Turn Inputs"));
    resize(920, 640);

    QVBoxLayout* layout = new QVBoxLayout(this);
    banner_ = new QLabel(QStringLiteral("Loading turn inputs…"), this);
    banner_->setObjectName("jobsMetaText");
    layout->addWidget(banner_);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabel(QStringLiteral("Turn Inputs"));
    layout->addWidget(tree_, 1);
}

void ExplorerRunsTurnInputsDialog::loadForJob(const ExplorerRunsJobRow& row)
{
    banner_->setText(QStringLiteral("Loading turn inputs…"));
    tree_->clear();

    std::unordered_set<qint64> visitedJobIds;
    std::vector<TurnInputNode> turns;
    qint64 currentJobId = row.jobId;
    while (currentJobId > 0 && !visitedJobIds.contains(currentJobId)) {
        visitedJobIds.insert(currentJobId);

        if (const std::optional<TurnInputNode> node = buildTurnInputNode(currentJobId); node.has_value()) {
            turns.push_back(*node);
            banner_->setText(QStringLiteral("Loading turn inputs… loaded %1 turn(s)").arg(turns.size()));
            QApplication::processEvents();
        }

        const std::optional<qint64> parentJobId = resolveParentJobId(currentJobId);
        if (!parentJobId.has_value()) {
            break;
        }
        currentJobId = *parentJobId;
    }

    std::sort(turns.begin(), turns.end(), [](const TurnInputNode& a, const TurnInputNode& b) {
        return a.turnNumber < b.turnNumber;
    });

    QTreeWidgetItem* parentTurnItem = nullptr;
    for (const TurnInputNode& turn : turns) {
        QTreeWidgetItem* turnItem = new QTreeWidgetItem(QStringList{ makeTurnNodeLabel(turn) });
        tree_->addTopLevelItem(turnItem);
        for (const QString& input : turn.inputLines) {
            turnItem->addChild(new QTreeWidgetItem(QStringList{ input }));
        }
    }

    tree_->expandAll();
    banner_->setText(QStringLiteral("Completely loaded %1 turn(s).").arg(turns.size()));
}
