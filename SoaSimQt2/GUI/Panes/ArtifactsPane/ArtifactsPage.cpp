#include "ArtifactsPage.h"

#include "ArtifactsController.h"
#include "ArtifactsBrowserTableModel.h"
#include "ArtifactsBrowserTableView.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

namespace {
void selectFlatRow(QAbstractItemView* view, int row)
{
    if (!view || !view->model()) {
        return;
    }

    const QModelIndex index = view->model()->index(row, 0);
    if (!index.isValid() || !view->selectionModel()) {
        return;
    }

    view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->scrollTo(index);
}
}

ArtifactsPage::ArtifactsPage(QWidget* parent)
    : QWidget(parent)
    , controller_(new ArtifactsController(this))
{
    createWidgets();
    wireSignals();
    controller_->loadInitial();
}

void ArtifactsPage::handleImportRequested()
{
    const QString sourcePath = QFileDialog::getOpenFileName(this, QStringLiteral("Import Artifact"));
    if (sourcePath.isEmpty()) {
        return;
    }

    openImportDialog({ sourcePath, QFileInfo(sourcePath).fileName() });
}

void ArtifactsPage::handleDroppedPaths(const QStringList& paths)
{
    if (paths.size() != 1) {
        QMessageBox::information(this, QStringLiteral("Import Artifact"), QStringLiteral("Drop exactly one local file."));
        return;
    }

    const QFileInfo info(paths.front());
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(this, QStringLiteral("Import Artifact"), QStringLiteral("The dropped item is not a readable local file."));
        return;
    }

    openImportDialog({ info.absoluteFilePath(), info.fileName() });
}

void ArtifactsPage::handleExportRequested()
{
    const auto& state = controller_->viewState();
    QString defaultName = QStringLiteral("artifact.bin");
    for (const auto& artifact : state.page.items) {
        if (artifact.artifact_id == state.selectedArtifactId) {
            defaultName = QString::fromStdString(artifact.filename);
            break;
        }
    }

    const QString destinationPath = QFileDialog::getSaveFileName(this, QStringLiteral("Materialize Artifact"), defaultName);
    if (destinationPath.isEmpty()) {
        return;
    }

    controller_->materializeSelectedArtifact(destinationPath);
}

void ArtifactsPage::openImportDialog(const ImportRequest& request)
{
    if (request.sourcePath.isEmpty()) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Import Artifact"));

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    QLabel* sourceLabel = new QLabel(QStringLiteral("Source"), &dialog);
    QLineEdit* sourceEdit = new QLineEdit(request.sourcePath, &dialog);
    sourceEdit->setReadOnly(true);
    QLabel* filenameLabel = new QLabel(QStringLiteral("Filename"), &dialog);
    QLineEdit* filenameEdit = new QLineEdit(request.defaultName, &dialog);
    QLabel* kindLabel = new QLabel(QStringLiteral("Kind"), &dialog);
    QComboBox* kindCombo = new QComboBox(&dialog);
    kindCombo->addItem(QStringLiteral("Auto"), QString());
    kindCombo->addItem(QStringLiteral("DTM"), QStringLiteral("DTM"));
    kindCombo->addItem(QStringLiteral("DTMINI"), QStringLiteral("DTMINI"));
    kindCombo->addItem(QStringLiteral("SAV"), QStringLiteral("SAV"));
    kindCombo->addItem(QStringLiteral("LOG"), QStringLiteral("LOG"));
    kindCombo->addItem(QStringLiteral("OTHER"), QStringLiteral("OTHER"));

    QLabel* hint = new QLabel(
        QStringLiteral("Review the source filename and object kind before importing."),
        &dialog);
    hint->setWordWrap(true);

    layout->addWidget(sourceLabel);
    layout->addWidget(sourceEdit);
    layout->addWidget(filenameLabel);
    layout->addWidget(filenameEdit);
    layout->addWidget(kindLabel);
    layout->addWidget(kindCombo);
    layout->addWidget(hint);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, filenameEdit]() {
        if (filenameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Import Artifact"), QStringLiteral("Filename cannot be empty."));
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    controller_->importArtifact(
        request.sourcePath,
        filenameEdit->text().trimmed(),
        kindCombo->currentData().toString());
}

void ArtifactsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    rootsBannerLabel_ = new QLabel(this);
    rootsBannerLabel_->setObjectName("jobSetsInlineMessage");
    rootsBannerLabel_->setProperty("severity", QStringLiteral("error"));
    rootsBannerLabel_->setWordWrap(true);
    rootsBannerLabel_->hide();
    rootLayout->addWidget(rootsBannerLabel_);

    QFrame* filterPanel = new QFrame(this);
    filterPanel->setObjectName("jobsToolbarPanel");
    QGridLayout* filterLayout = new QGridLayout(filterPanel);
    filterLayout->setContentsMargins(12, 10, 12, 10);
    filterLayout->setHorizontalSpacing(10);
    filterLayout->setVerticalSpacing(10);

    searchEdit_ = new QLineEdit(filterPanel);
    extensionEdit_ = new QLineEdit(filterPanel);
    pageSizeSpin_ = new QSpinBox(filterPanel);
    applyButton_ = new QPushButton(QStringLiteral("Apply"), filterPanel);
    resetButton_ = new QPushButton(QStringLiteral("Reset"), filterPanel);
    importButton_ = new QPushButton(QStringLiteral("Import artifact…"), filterPanel);

    searchEdit_->setObjectName("jobsFilterEdit");
    extensionEdit_->setObjectName("jobsFilterEdit");
    pageSizeSpin_->setObjectName("jobsRefreshSpin");
    applyButton_->setObjectName("jobsPrimaryButton");
    resetButton_->setObjectName("jobsSecondaryButton");
    importButton_->setObjectName("jobsSecondaryButton");

    searchEdit_->setPlaceholderText(QStringLiteral("filename contains…"));
    extensionEdit_->setPlaceholderText(QStringLiteral(".sav / .dtm / .bctx"));
    pageSizeSpin_->setRange(10, 500);
    pageSizeSpin_->setSingleStep(10);

    filterLayout->addWidget(new QLabel(QStringLiteral("Search"), filterPanel), 0, 0);
    filterLayout->addWidget(searchEdit_, 1, 0, 1, 2);
    filterLayout->addWidget(new QLabel(QStringLiteral("Extension"), filterPanel), 0, 2);
    filterLayout->addWidget(extensionEdit_, 1, 2);
    filterLayout->addWidget(new QLabel(QStringLiteral("Page size"), filterPanel), 0, 3);
    filterLayout->addWidget(pageSizeSpin_, 1, 3);
    filterLayout->addWidget(applyButton_, 1, 4);
    filterLayout->addWidget(resetButton_, 1, 5);
    filterLayout->addWidget(importButton_, 1, 6);
    filterLayout->setColumnStretch(1, 1);
    rootLayout->addWidget(filterPanel);

    QFrame* contentPanel = new QFrame(this);
    contentPanel->setObjectName("jobsContentPanel");
    QVBoxLayout* contentLayout = new QVBoxLayout(contentPanel);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    QFrame* pageControls = new QFrame(contentPanel);
    pageControls->setObjectName("jobsPagingPanel");
    QHBoxLayout* pageLayout = new QHBoxLayout(pageControls);
    pageLayout->setContentsMargins(12, 10, 12, 10);
    prevButton_ = new QPushButton(QStringLiteral("Newer"), pageControls);
    nextButton_ = new QPushButton(QStringLiteral("Older"), pageControls);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), pageControls);
    pageSummaryLabel_ = new QLabel(pageControls);
    lastRefreshLabel_ = new QLabel(pageControls);
    prevButton_->setObjectName("jobsSecondaryButton");
    nextButton_->setObjectName("jobsSecondaryButton");
    refreshButton_->setObjectName("jobsSecondaryButton");
    pageSummaryLabel_->setObjectName("jobsMetaText");
    lastRefreshLabel_->setObjectName("jobsMetaText");
    pageLayout->addWidget(prevButton_);
    pageLayout->addWidget(nextButton_);
    pageLayout->addWidget(refreshButton_);
    pageLayout->addSpacing(8);
    pageLayout->addWidget(pageSummaryLabel_);
    pageLayout->addStretch();
    pageLayout->addWidget(lastRefreshLabel_);
    contentLayout->addWidget(pageControls);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, contentPanel);
    splitter->setChildrenCollapsible(false);

    QFrame* tablePanel = new QFrame(splitter);
    tablePanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* tableLayout = new QVBoxLayout(tablePanel);
    tableLayout->setContentsMargins(12, 12, 12, 12);
    tableLayout->addWidget(new QLabel(QStringLiteral("Artifact Table"), tablePanel));
    artifactsTable_ = new ArtifactsBrowserTableView(tablePanel);
    tableModel_ = new ArtifactsBrowserTableModel(artifactsTable_);
    artifactsTable_->attachModel(tableModel_);
    tableLayout->addWidget(artifactsTable_, 1);

    QFrame* inspectorPanel = new QFrame(splitter);
    inspectorPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* inspectorLayout = new QVBoxLayout(inspectorPanel);
    inspectorLayout->setContentsMargins(12, 12, 12, 12);

    inspectorLayout->addWidget(new QLabel(QStringLiteral("Artifact Inspector"), inspectorPanel));
    inspectorSummary_ = new QLabel(QStringLiteral("Select an artifact to inspect metadata and export it."), inspectorPanel);
    inspectorSummary_->setObjectName("jobsInspectorSummary");
    inspectorSummary_->setWordWrap(true);
    inspectorLayout->addWidget(inspectorSummary_);

    QGridLayout* inspectorGrid = new QGridLayout();
    inspectorGrid->setHorizontalSpacing(10);
    inspectorGrid->setVerticalSpacing(10);
    inspectorIdValue_ = new QLabel(QStringLiteral("--"), inspectorPanel);
    inspectorFilenameValue_ = new QLabel(QStringLiteral("--"), inspectorPanel);
    inspectorFilenameValue_->setWordWrap(true);
    inspectorSizeValue_ = new QLabel(QStringLiteral("--"), inspectorPanel);
    inspectorKindValue_ = new QLabel(QStringLiteral("--"), inspectorPanel);
    inspectorCreatedValue_ = new QLabel(QStringLiteral("--"), inspectorPanel);
    inspectorGrid->addWidget(new QLabel(QStringLiteral("ID"), inspectorPanel), 0, 0);
    inspectorGrid->addWidget(inspectorIdValue_, 0, 1);
    inspectorGrid->addWidget(new QLabel(QStringLiteral("Filename"), inspectorPanel), 1, 0);
    inspectorGrid->addWidget(inspectorFilenameValue_, 1, 1);
    inspectorGrid->addWidget(new QLabel(QStringLiteral("Size"), inspectorPanel), 2, 0);
    inspectorGrid->addWidget(inspectorSizeValue_, 2, 1);
    inspectorGrid->addWidget(new QLabel(QStringLiteral("Kind"), inspectorPanel), 3, 0);
    inspectorGrid->addWidget(inspectorKindValue_, 3, 1);
    inspectorGrid->addWidget(new QLabel(QStringLiteral("Created"), inspectorPanel), 4, 0);
    inspectorGrid->addWidget(inspectorCreatedValue_, 4, 1);
    inspectorLayout->addLayout(inspectorGrid);

    inspectorLayout->addWidget(new QLabel(QStringLiteral("SHA-256"), inspectorPanel));
    inspectorShaText_ = new QTextEdit(inspectorPanel);
    inspectorShaText_->setObjectName("jobsInspectorText");
    inspectorShaText_->setReadOnly(true);
    inspectorShaText_->setFixedHeight(96);
    inspectorLayout->addWidget(inspectorShaText_);

    QHBoxLayout* inspectorActions = new QHBoxLayout();
    exportButton_ = new QPushButton(QStringLiteral("Materialize to file…"), inspectorPanel);
    exportButton_->setObjectName("jobsPrimaryButton");
    inspectorActions->addWidget(exportButton_);
    inspectorActions->addStretch();
    inspectorLayout->addLayout(inspectorActions);
    inspectorLayout->addStretch();

    splitter->addWidget(tablePanel);
    splitter->addWidget(inspectorPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    contentLayout->addWidget(splitter, 1);

    inlineMessageLabel_ = new QLabel(contentPanel);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    contentLayout->addWidget(inlineMessageLabel_);

    rootLayout->addWidget(contentPanel, 1);
}

void ArtifactsPage::wireSignals()
{
    connect(controller_, &ArtifactsController::stateChanged, this, [this]() {
        syncControlsFromController();
        refreshModel();
        updateInspector();
        updateStatusWidgets();
    });

    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        controller_->applyFilters(searchEdit_->text(), extensionEdit_->text(), pageSizeSpin_->value());
    });
    connect(resetButton_, &QPushButton::clicked, controller_, &ArtifactsController::resetFilters);
    connect(importButton_, &QPushButton::clicked, this, &ArtifactsPage::handleImportRequested);
    connect(refreshButton_, &QPushButton::clicked, controller_, &ArtifactsController::requestRefresh);
    connect(prevButton_, &QPushButton::clicked, controller_, &ArtifactsController::requestPreviousPage);
    connect(nextButton_, &QPushButton::clicked, controller_, &ArtifactsController::requestNextPage);
    connect(exportButton_, &QPushButton::clicked, this, &ArtifactsPage::handleExportRequested);
    connect(artifactsTable_, &ArtifactsBrowserTableView::fileDropRequested, this, &ArtifactsPage::handleDroppedPaths);

    connect(artifactsTable_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) {
            controller_->selectArtifact(0);
            return;
        }
        if (const auto* row = tableModel_->rowAt(current.row())) {
            controller_->selectArtifact(row->artifact.artifact_id);
        }
    });

    connect(artifactsTable_, &ArtifactsBrowserTableView::doubleClicked, this, [this](const QModelIndex& current) {
        if (!current.isValid()) {
            return;
        }
        selectFlatRow(artifactsTable_, current.row());
        handleExportRequested();
    });
}

void ArtifactsPage::syncControlsFromController()
{
    const auto& state = controller_->viewState();
    {
        QSignalBlocker blocker(searchEdit_);
        searchEdit_->setText(state.search);
    }
    {
        QSignalBlocker blocker(extensionEdit_);
        extensionEdit_->setText(state.extension);
    }
    {
        QSignalBlocker blocker(pageSizeSpin_);
        pageSizeSpin_->setValue(state.pageLimit);
    }

    const bool interactive = state.rootsReady && !state.loading && !state.importBusy && !state.exportBusy;
    searchEdit_->setEnabled(state.rootsReady);
    extensionEdit_->setEnabled(state.rootsReady);
    pageSizeSpin_->setEnabled(state.rootsReady);
    applyButton_->setEnabled(interactive);
    resetButton_->setEnabled(interactive);
    importButton_->setEnabled(state.rootsReady && !state.importBusy && !state.exportBusy);
    prevButton_->setEnabled(state.rootsReady && state.page.prev.has_value() && !state.loading);
    nextButton_->setEnabled(state.rootsReady && state.page.next.has_value() && !state.loading);
    refreshButton_->setEnabled(state.rootsReady && !state.loading);
}

void ArtifactsPage::refreshModel()
{
    
    const auto& state = controller_->viewState();

    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(artifactsTable_);

    std::vector<ArtifactsBrowserTableModel::Row> rows;
    rows.reserve(state.page.items.size());
    for (const auto& artifact : state.page.items) {
        rows.push_back(ArtifactsBrowserTableModel::Row{ artifact });
    }
    tableModel_->setRows(rows);

    if (QItemSelectionModel* selectionModel = artifactsTable_->selectionModel()) {
        QSignalBlocker blocker(selectionModel);
        bool matchedSelection = false;
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            if (rows[row].artifact.artifact_id == state.selectedArtifactId) {
                selectFlatRow(artifactsTable_, row);
                matchedSelection = true;
                break;
            }
        }

        if (!matchedSelection) {
            selectionModel->clearSelection();
            selectionModel->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
        }
    }

    restoreItemViewScrollSnapshot(artifactsTable_, scrollSnapshot);

}

void ArtifactsPage::updateInspector()
{
    const auto& state = controller_->viewState();
    const ScrollAreaScrollSnapshot inspectorShaScrollSnapshot = captureScrollAreaScrollSnapshot(inspectorShaText_);
    const simcore::db::UiArtifactSummary* selected = nullptr;
    for (const auto& artifact : state.page.items) {
        if (artifact.artifact_id == state.selectedArtifactId) {
            selected = &artifact;
            break;
        }
    }

    if (!selected) {
        inspectorSummary_->setText(QStringLiteral("Select an artifact to inspect metadata and export it."));
        inspectorIdValue_->setText(QStringLiteral("--"));
        inspectorFilenameValue_->setText(QStringLiteral("--"));
        inspectorSizeValue_->setText(QStringLiteral("--"));
        inspectorKindValue_->setText(QStringLiteral("--"));
        inspectorCreatedValue_->setText(QStringLiteral("--"));
        inspectorShaText_->clear();
        restoreScrollAreaScrollSnapshot(inspectorShaText_, inspectorShaScrollSnapshot);
        exportButton_->setEnabled(false);
        return;
    }

    inspectorSummary_->setText(QStringLiteral("Artifact %1 is ready for inspection or materialization.").arg(selected->artifact_id));
    inspectorIdValue_->setText(QString::number(selected->artifact_id));
    inspectorFilenameValue_->setText(QString::fromStdString(selected->filename));
    inspectorSizeValue_->setText(ArtifactsBrowserTableModel::formatSize(static_cast<qint64>(selected->size_bytes)));
    inspectorKindValue_->setText(QString::fromStdString(selected->artifact_kind));
    inspectorCreatedValue_->setText(ArtifactsBrowserTableModel::formatCreatedAt(selected->created_at_utc));
    inspectorShaText_->setPlainText(QString::fromStdString(selected->sha256));
    restoreScrollAreaScrollSnapshot(inspectorShaText_, inspectorShaScrollSnapshot);
    exportButton_->setEnabled(state.rootsReady && !state.exportBusy && !state.importBusy);
}

void ArtifactsPage::updateStatusWidgets()
{
    const auto& state = controller_->viewState();
    StatusToast::Severity toastSeverity = StatusToast::Severity::Info;
    QString toastMessage;
    pageSummaryLabel_->setText(QStringLiteral("Rows: %1 • page size: %2").arg(state.page.items.size()).arg(state.pageLimit));
    lastRefreshLabel_->setText(state.lastRefresh.isValid() ? QStringLiteral("Last refresh: %1").arg(state.lastRefresh.toString(QStringLiteral("hh:mm:ss AP"))) : QStringLiteral("Last refresh: --"));

    if (!state.rootsReady) {
        rootsBannerLabel_->setText(QStringLiteral("ObjectStore roots are unset. Artifacts view/actions are disabled until storage is configured in Settings."));
        rootsBannerLabel_->show();
    } else {
        rootsBannerLabel_->hide();
    }

    if (!state.errorMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error"));
        inlineMessageLabel_->setText(state.errorMessage);
        inlineMessageLabel_->show();
        toastSeverity = StatusToast::Severity::Error;
        toastMessage = state.errorMessage;
    } else if (state.loading) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Loading artifacts…"));
        inlineMessageLabel_->show();
    } else if (state.importBusy) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Importing artifact…"));
        inlineMessageLabel_->show();
    } else if (state.exportBusy) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Materializing artifact…"));
        inlineMessageLabel_->show();
    } else if (!state.infoMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(state.infoMessage);
        inlineMessageLabel_->show();
        toastMessage = state.infoMessage;
    } else if (state.rootsReady && state.page.items.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("No artifacts matched the current filters."));
        inlineMessageLabel_->show();
    } else {
        inlineMessageLabel_->hide();
    }

    inlineMessageLabel_->style()->unpolish(inlineMessageLabel_);
    inlineMessageLabel_->style()->polish(inlineMessageLabel_);
    rootsBannerLabel_->style()->unpolish(rootsBannerLabel_);
    rootsBannerLabel_->style()->polish(rootsBannerLabel_);

    if (!toastMessage.isEmpty()) {
        const QString signature = QStringLiteral("%1|%2").arg(static_cast<int>(toastSeverity)).arg(toastMessage);
        if (signature != lastToastSignature_) {
            lastToastSignature_ = signature;
            emit statusToastRequested(StatusToast{ toastSeverity, toastMessage, QString(), 1, QDateTime{}, 4000 });
        }
    }
}
