#include "ExplorerRunsReplicationDialog.h"

#include "Core/Input/AppliedTurnTapeBlob.h"
#include "Core/Input/InputPlanFmt.h"
#include "DB/DBCore/ObjectStore.h"
#include "DB/DeltaSeedRepo.h"
#include "DB/ProgramDB/BattleSingleTurnRunDBCodec.h"
#include "DB/SavestateRepo.h"
#include "DB/Scheduling/JobEventsRepo.h"
#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobSetsRepo.h"
#include "DB/SeedProbeRepo.h"
#include "DB/TasMovieRepo.h"
#include "Utils/IniDoc.h"

#include <QtCore/QStringList>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <optional>

using namespace simcore::db;
using namespace simcore::db::codec::battle::singleturn;

namespace {

struct WaveMeta {
    qint64 seedProbeId = -1;
    qint64 tasMovieId = -1;
};

WaveMeta parseWaveMetaText(const std::optional<std::string>& metaText)
{
    WaveMeta meta{};
    if (!metaText.has_value() || metaText->empty()) {
        return meta;
    }

    IniDoc ini = IniDoc::parse(*metaText);
    constexpr const char* section = "BattleSingleTurn.WaveMeta";
    if (!ini.has_section(section)) {
        return meta;
    }

    IniKV kv = ini.section_kv(section);
    meta.seedProbeId = kv.get_i64("seed_probe_id", -1);
    meta.tasMovieId = kv.get_i64("tas_movie_id", -1);
    return meta;
}

std::optional<ResultsIni> readSingleTurnResults(qint64 jobId)
{
    auto payload = JobEventsRepo::GetLatestPayload(jobId, "RESULTS");
    if (!payload.ok || !payload.value.has_value()) {
        return std::nullopt;
    }

    const IniDoc doc = IniDoc::parse(*payload.value);
    if (!doc.has_section(ResultsIni::SECTION_NAME)) {
        return std::nullopt;
    }
    return ResultsIni::from_section(doc);
}

std::optional<simcore::inputtape::TurnChunk> readTurnChunkForJob(qint64 jobId, quint32 turnIndex)
{
    const std::optional<ResultsIni> results = readSingleTurnResults(jobId);
    if (!results.has_value() || results->applied_input_artifact_id <= 0) {
        return std::nullopt;
    }

    auto tapeText = ObjectStore::GetText(results->applied_input_artifact_id);
    if (!tapeText.ok) {
        return std::nullopt;
    }

    std::vector<simcore::inputtape::TurnChunk> chunks;
    if (!simcore::inputtape::decode_turn_chunks(tapeText.value, chunks) || chunks.empty()) {
        return std::nullopt;
    }

    for (const auto& chunk : chunks) {
        if (chunk.turn_number == turnIndex) {
            return chunk;
        }
    }

    return chunks.front();
}

}

ExplorerRunsReplicationDialog::ExplorerRunsReplicationDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Replication Details"));
    resize(920, 600);
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* materializeLayout = new QHBoxLayout();
    materializeTasMovieButton_ = new QPushButton(QStringLiteral("Materialize TAS movie DTM…"), this);
    materializeStartSavestateButton_ = new QPushButton(QStringLiteral("Materialize start savestate…"), this);
    materializeLayout->addWidget(materializeTasMovieButton_);
    materializeLayout->addWidget(materializeStartSavestateButton_);
    materializeLayout->addStretch();
    layout->addLayout(materializeLayout);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("jobsMetaText");
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    infoText_ = new QPlainTextEdit(this);
    infoText_->setReadOnly(true);
    layout->addWidget(infoText_, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(materializeTasMovieButton_, &QPushButton::clicked, this, &ExplorerRunsReplicationDialog::materializeTasMovieDtm);
    connect(materializeStartSavestateButton_, &QPushButton::clicked, this, &ExplorerRunsReplicationDialog::materializeStartSavestate);

    materializeTasMovieButton_->setEnabled(false);
    materializeStartSavestateButton_->setEnabled(false);
}

void ExplorerRunsReplicationDialog::loadForJob(qint64 jobId, const QString& battlePlanDescription)
{
    tasMovieObjectRefId_ = -1;
    startSavestateObjectRefId_ = -1;

    QStringList lines;
    lines << QStringLiteral("job_id: %1").arg(jobId);

    auto job = JobsRepo::Get(jobId);
    if (!job.ok || !job.value.vm_kv.has_value()) {
        setInfoText(QStringLiteral("Unable to load job vm_kv for job %1.").arg(jobId));
        setStatusMessage(QStringLiteral("Failed to read selected job."), true);
        materializeTasMovieButton_->setEnabled(false);
        materializeStartSavestateButton_->setEnabled(false);
        return;
    }

    const JobIni jobIni = JobIni::from_section(IniDoc::parse(*job.value.vm_kv));
    lines << QStringLiteral("delta_seed_id: %1").arg(jobIni.delta_seed_id);
    lines << QStringLiteral("plan_id: %1").arg(jobIni.plan_id);

    QString initialInputText = QStringLiteral("(unavailable)");
    if (jobIni.delta_seed_id > 0) {
        auto deltaSeed = DeltaSeedRepo::Get(jobIni.delta_seed_id);
        if (deltaSeed.ok && deltaSeed.value.has_value()) {
            initialInputText = QString::fromStdString(DescribeFrameCompact(deltaSeed.value->input));
        }
    }
    lines << QStringLiteral("initial_input_from_delta_seed: %1").arg(initialInputText);

    quint32 firstInputVi = 0;
    const std::optional<simcore::inputtape::TurnChunk> chunk = readTurnChunkForJob(jobId, jobIni.turn_index);
    if (chunk.has_value()) {
        firstInputVi = chunk->vi_start;
        lines << QStringLiteral("first_input_vi_from_input_plan: %1").arg(firstInputVi);
    } else {
        lines << QStringLiteral("first_input_vi_from_input_plan: (unavailable)");
    }

    auto jobSet = JobSetsRepo::Get(job.value.job_set_id);
    if (jobSet.ok) {
        const WaveMeta meta = parseWaveMetaText(jobSet.value.meta_text);
        lines << QStringLiteral("seed_probe_id: %1").arg(meta.seedProbeId);
        lines << QStringLiteral("tas_movie_id: %1").arg(meta.tasMovieId);

        if (meta.tasMovieId > 0) {
            auto tasMovie = TasMovieRepo::Get(meta.tasMovieId);
            if (tasMovie.ok) {
                tasMovieObjectRefId_ = tasMovie.value.base_file_id;
                lines << QStringLiteral("tas_movie_dtm_object_ref_id: %1").arg(tasMovieObjectRefId_);
            }
        }

        if (meta.seedProbeId > 0) {
            auto probe = SeedProbeRepo::Get(meta.seedProbeId);
            if (probe.ok && probe.value.savestate_id > 0) {
                lines << QStringLiteral("movie_start_savestate_id: %1").arg(probe.value.savestate_id);
                auto savestate = SavestateRepo::Get(probe.value.savestate_id);
                if (savestate.ok && savestate.value.has_value()) {
                    startSavestateObjectRefId_ = savestate.value->object_ref_id;
                    lines << QStringLiteral("movie_start_savestate_object_ref_id: %1").arg(startSavestateObjectRefId_);
                }
            }
        }
    }

    lines << QString();
    lines << QStringLiteral("battle_plan_description:");
    lines << (battlePlanDescription.isEmpty() ? QStringLiteral("(unavailable)") : battlePlanDescription);

    setInfoText(lines.join(QStringLiteral("\n")));
    materializeTasMovieButton_->setEnabled(tasMovieObjectRefId_ > 0);
    materializeStartSavestateButton_->setEnabled(startSavestateObjectRefId_ > 0);
    setStatusMessage(QStringLiteral("Loaded replication details for job %1.").arg(jobId));
}

void ExplorerRunsReplicationDialog::materializeTasMovieDtm()
{
    materializeObjectToPath(tasMovieObjectRefId_, QStringLiteral("tas_movie.dtm"));
}

void ExplorerRunsReplicationDialog::materializeStartSavestate()
{
    materializeObjectToPath(startSavestateObjectRefId_, QStringLiteral("start.sav"));
}

void ExplorerRunsReplicationDialog::setInfoText(const QString& text)
{
    infoText_->setPlainText(text);
}

void ExplorerRunsReplicationDialog::setStatusMessage(const QString& message, bool isError)
{
    statusLabel_->setText(message);
    statusLabel_->setProperty("error", isError);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
}

void ExplorerRunsReplicationDialog::materializeObjectToPath(qint64 objectRefId, const QString& suggestedFilename)
{
    if (objectRefId <= 0) {
        setStatusMessage(QStringLiteral("No artifact available to materialize."), true);
        return;
    }

    const QString outPath = QFileDialog::getSaveFileName(this, QStringLiteral("Materialize Artifact"), suggestedFilename);
    if (outPath.isEmpty()) {
        return;
    }

    auto materialize = ObjectStore::MaterializeToPathAsync(objectRefId, outPath.toStdString()).get();
    if (materialize.ok) {
        setStatusMessage(QStringLiteral("Materialized object %1 to %2.").arg(objectRefId).arg(outPath));
    } else {
        setStatusMessage(QStringLiteral("Materialize failed: %1").arg(QString::fromStdString(materialize.error.message)), true);
    }
}
