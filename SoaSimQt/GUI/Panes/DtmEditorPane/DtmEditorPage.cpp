#include "DtmEditorPage.h"

#include <QtCore/QStringList>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

namespace {
StatusToast makeToast(StatusToast::Level level, const QString& text)
{
    return StatusToast{ level, text, 4000 };
}
}

DtmEditorPage::DtmEditorPage(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);

    auto* topRow = new QHBoxLayout();
    auto* pickButton = new QPushButton(QStringLiteral("Open DTM..."), this);
    auto* saveButton = new QPushButton(QStringLiteral("Save DTM As..."), this);
    auto* loadAnnButton = new QPushButton(QStringLiteral("Load Annotations..."), this);
    auto* saveAnnButton = new QPushButton(QStringLiteral("Save Annotations..."), this);
    topRow->addWidget(pickButton);
    topRow->addWidget(saveButton);
    topRow->addSpacing(16);
    topRow->addWidget(loadAnnButton);
    topRow->addWidget(saveAnnButton);
    topRow->addStretch(1);
    root->addLayout(topRow);

    fileLabel_ = new QLabel(QStringLiteral("No DTM loaded."), this);
    hashLabel_ = new QLabel(QStringLiteral("SHA256: (none)"), this);
    root->addWidget(fileLabel_);
    root->addWidget(hashLabel_);

    validationText_ = new QTextEdit(this);
    validationText_->setReadOnly(true);
    validationText_->setMaximumHeight(120);
    root->addWidget(validationText_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* leftPane = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    pollTable_ = new QTableWidget(leftPane);
    pollTable_->setColumnCount(8);
    pollTable_->setHorizontalHeaderLabels(QStringList{ "Poll", "Buttons", "L", "R", "SX", "SY", "CX", "CY" });
    pollTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    pollTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    pollTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    leftLayout->addWidget(pollTable_);

    auto* pollEditorBox = new QGroupBox(QStringLiteral("Poll Editor"), leftPane);
    auto* pollForm = new QFormLayout(pollEditorBox);
    buttonBitsSpin_ = new QSpinBox(pollEditorBox); buttonBitsSpin_->setRange(0, 0xFFFF);
    triggerLSpin_ = new QSpinBox(pollEditorBox); triggerLSpin_->setRange(0, 255);
    triggerRSpin_ = new QSpinBox(pollEditorBox); triggerRSpin_->setRange(0, 255);
    stickXSpin_ = new QSpinBox(pollEditorBox); stickXSpin_->setRange(0, 255);
    stickYSpin_ = new QSpinBox(pollEditorBox); stickYSpin_->setRange(0, 255);
    cstickXSpin_ = new QSpinBox(pollEditorBox); cstickXSpin_->setRange(0, 255);
    cstickYSpin_ = new QSpinBox(pollEditorBox); cstickYSpin_->setRange(0, 255);
    pollForm->addRow(QStringLiteral("Buttons (raw bits):"), buttonBitsSpin_);
    pollForm->addRow(QStringLiteral("Trigger L:"), triggerLSpin_);
    pollForm->addRow(QStringLiteral("Trigger R:"), triggerRSpin_);
    pollForm->addRow(QStringLiteral("Stick X:"), stickXSpin_);
    pollForm->addRow(QStringLiteral("Stick Y:"), stickYSpin_);
    pollForm->addRow(QStringLiteral("CStick X:"), cstickXSpin_);
    pollForm->addRow(QStringLiteral("CStick Y:"), cstickYSpin_);

    auto* pollBtnRow = new QHBoxLayout();
    auto* applyPollButton = new QPushButton(QStringLiteral("Apply to Selected Poll"), pollEditorBox);
    auto* insertPollButton = new QPushButton(QStringLiteral("Insert Poll Before Selected"), pollEditorBox);
    auto* deletePollButton = new QPushButton(QStringLiteral("Delete Selected Poll"), pollEditorBox);
    pollBtnRow->addWidget(applyPollButton);
    pollBtnRow->addWidget(insertPollButton);
    pollBtnRow->addWidget(deletePollButton);
    pollForm->addRow(pollBtnRow);
    leftLayout->addWidget(pollEditorBox);

    auto* rightPane = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPane);
    bookmarkTable_ = new QTableWidget(rightPane);
    bookmarkTable_->setColumnCount(4);
    bookmarkTable_->setHorizontalHeaderLabels(QStringList{ "Id", "Label", "Offset", "Tags" });
    bookmarkTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rightLayout->addWidget(bookmarkTable_);

    auto* bookmarkBox = new QGroupBox(QStringLiteral("Add Bookmark"), rightPane);
    auto* bookmarkForm = new QFormLayout(bookmarkBox);
    bookmarkLabelEdit_ = new QLineEdit(bookmarkBox);
    bookmarkTagsEdit_ = new QLineEdit(bookmarkBox);
    bookmarkNoteEdit_ = new QTextEdit(bookmarkBox);
    bookmarkNoteEdit_->setMaximumHeight(80);
    bookmarkForm->addRow(QStringLiteral("Label:"), bookmarkLabelEdit_);
    bookmarkForm->addRow(QStringLiteral("Tags (comma):"), bookmarkTagsEdit_);
    bookmarkForm->addRow(QStringLiteral("Note:"), bookmarkNoteEdit_);
    auto* addBookmarkButton = new QPushButton(QStringLiteral("Add Bookmark at Selected Poll"), bookmarkBox);
    bookmarkForm->addRow(addBookmarkButton);
    rightLayout->addWidget(bookmarkBox);

    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    connect(pickButton, &QPushButton::clicked, this, &DtmEditorPage::onPickDtm);
    connect(saveButton, &QPushButton::clicked, this, &DtmEditorPage::onSaveDtmAs);
    connect(loadAnnButton, &QPushButton::clicked, this, &DtmEditorPage::onLoadAnnotations);
    connect(saveAnnButton, &QPushButton::clicked, this, &DtmEditorPage::onSaveAnnotations);
    connect(applyPollButton, &QPushButton::clicked, this, &DtmEditorPage::onApplyPoll);
    connect(insertPollButton, &QPushButton::clicked, this, &DtmEditorPage::onInsertPoll);
    connect(deletePollButton, &QPushButton::clicked, this, &DtmEditorPage::onDeletePoll);
    connect(addBookmarkButton, &QPushButton::clicked, this, &DtmEditorPage::onAddBookmark);

    connect(pollTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
        if (!dtm_.has_value()) return;
        const int row = pollTable_->currentRow();
        if (row < 0) return;
        simcore::tas::DtmGCPoll poll{};
        if (!dtm_->read_gc_poll(static_cast<size_t>(row), poll)) return;
        buttonBitsSpin_->setValue(static_cast<int>(poll.button_bits));
        triggerLSpin_->setValue(static_cast<int>(poll.trigger_l));
        triggerRSpin_->setValue(static_cast<int>(poll.trigger_r));
        stickXSpin_->setValue(static_cast<int>(poll.stick_x));
        stickYSpin_->setValue(static_cast<int>(poll.stick_y));
        cstickXSpin_->setValue(static_cast<int>(poll.cstick_x));
        cstickYSpin_->setValue(static_cast<int>(poll.cstick_y));
        });
}

void DtmEditorPage::onPickDtm()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open DTM"), QString(), QStringLiteral("DTM (*.dtm);;All Files (*)"));
    if (path.isEmpty()) return;

    simcore::tas::DtmFile dtm;
    if (!dtm.load(path.toStdString())) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Failed to load DTM file.")));
        return;
    }

    dtm_ = std::move(dtm);
    loadedDtmPath_ = path;
    fileLabel_->setText(QStringLiteral("Loaded: %1").arg(path));
    hashLabel_->setText(QStringLiteral("SHA256: %1").arg(QString::fromStdString(dtm_->compute_sha256())));
    annotations_.reset();
    rebuildValidationText();
    rebuildPollTable();
    rebuildBookmarkTable();
    emit statusToastRequested(makeToast(StatusToast::Level::Info, QStringLiteral("DTM loaded.")));
}

void DtmEditorPage::onSaveDtmAs()
{
    if (!dtm_.has_value()) return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save DTM As"), loadedDtmPath_, QStringLiteral("DTM (*.dtm);;All Files (*)"));
    if (path.isEmpty()) return;
    if (!dtm_->save(path.toStdString())) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Failed to save DTM file.")));
        return;
    }
    emit statusToastRequested(makeToast(StatusToast::Level::Success, QStringLiteral("DTM saved.")));
}

void DtmEditorPage::onInsertPoll()
{
    if (!dtm_.has_value()) return;
    const int row = std::max(0, pollTable_->currentRow());
    simcore::tas::DtmGCPoll poll;
    poll.button_bits = static_cast<uint16_t>(buttonBitsSpin_->value());
    poll.trigger_l = static_cast<uint8_t>(triggerLSpin_->value());
    poll.trigger_r = static_cast<uint8_t>(triggerRSpin_->value());
    poll.stick_x = static_cast<uint8_t>(stickXSpin_->value());
    poll.stick_y = static_cast<uint8_t>(stickYSpin_->value());
    poll.cstick_x = static_cast<uint8_t>(cstickXSpin_->value());
    poll.cstick_y = static_cast<uint8_t>(cstickYSpin_->value());
    if (!dtm_->insert_gc_polls(static_cast<size_t>(row), { poll })) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Insert poll failed (likely unsupported payload mode).")));
        return;
    }
    rebuildValidationText();
    rebuildPollTable();
}

void DtmEditorPage::onDeletePoll()
{
    if (!dtm_.has_value()) return;
    const int row = pollTable_->currentRow();
    if (row < 0) return;
    if (!dtm_->erase_gc_polls(static_cast<size_t>(row), 1)) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Delete poll failed.")));
        return;
    }
    rebuildValidationText();
    rebuildPollTable();
}

void DtmEditorPage::onApplyPoll()
{
    if (!dtm_.has_value()) return;
    const int row = pollTable_->currentRow();
    if (row < 0) return;

    simcore::tas::DtmGCPoll poll;
    poll.button_bits = static_cast<uint16_t>(buttonBitsSpin_->value());
    poll.trigger_l = static_cast<uint8_t>(triggerLSpin_->value());
    poll.trigger_r = static_cast<uint8_t>(triggerRSpin_->value());
    poll.stick_x = static_cast<uint8_t>(stickXSpin_->value());
    poll.stick_y = static_cast<uint8_t>(stickYSpin_->value());
    poll.cstick_x = static_cast<uint8_t>(cstickXSpin_->value());
    poll.cstick_y = static_cast<uint8_t>(cstickYSpin_->value());

    if (!dtm_->write_gc_poll(static_cast<size_t>(row), poll)) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Poll update failed.")));
        return;
    }
    rebuildPollTable();
}

void DtmEditorPage::onAddBookmark()
{
    if (!dtm_.has_value()) return;
    const int row = pollTable_->currentRow();
    if (row < 0) return;

    if (!annotations_.has_value()) {
        annotations_.emplace();
        annotations_->dtm_sha256 = dtm_->compute_sha256();
        annotations_->dtm_byte_length = static_cast<uint64_t>(dtm_->bytes().size());
        annotations_->sav_required = dtm_->info().starts_from_savestate;
    }

    simcore::tas::DtmBookmark bm;
    bm.id = std::to_string(annotations_->bookmarks.size() + 1);
    bm.label = bookmarkLabelEdit_->text().toStdString();
    bm.note = bookmarkNoteEdit_->toPlainText().toStdString();
    bm.input_byte_offset = static_cast<uint64_t>(row) * 8;

    const QStringList tags = bookmarkTagsEdit_->text().split(',', Qt::SkipEmptyParts);
    for (const QString& t : tags) {
        bm.tags.push_back(t.trimmed().toStdString());
    }

    annotations_->bookmarks.push_back(std::move(bm));
    rebuildBookmarkTable();
}

void DtmEditorPage::onSaveAnnotations()
{
    if (!dtm_.has_value()) return;
    if (!annotations_.has_value()) {
        emit statusToastRequested(makeToast(StatusToast::Level::Warning, QStringLiteral("No annotations to save.")));
        return;
    }

    if (annotations_->dtm_sha256 != dtm_->compute_sha256()) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Annotation binding hash mismatch with currently loaded DTM.")));
        return;
    }

    QString defaultPath = loadedDtmPath_;
    if (!defaultPath.isEmpty()) defaultPath += QStringLiteral(".annotations.ini");

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save annotations"), defaultPath, QStringLiteral("Annotation INI (*.ini);;All Files (*)"));
    if (path.isEmpty()) return;

    if (!simcore::tas::DtmAnnotationIo::save_ini(path.toStdString(), *annotations_)) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Failed to save annotations.")));
        return;
    }

    emit statusToastRequested(makeToast(StatusToast::Level::Success, QStringLiteral("Annotations saved.")));
}

void DtmEditorPage::onLoadAnnotations()
{
    if (!dtm_.has_value()) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open annotations"), QString(), QStringLiteral("Annotation INI (*.ini);;All Files (*)"));
    if (path.isEmpty()) return;

    const auto loaded = simcore::tas::DtmAnnotationIo::load_ini(path.toStdString());
    if (!loaded.has_value()) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Failed to parse annotation file.")));
        return;
    }

    if (loaded->dtm_sha256 != dtm_->compute_sha256()) {
        emit statusToastRequested(makeToast(StatusToast::Level::Error, QStringLiteral("Annotation binding hash mismatch; refusing load.")));
        return;
    }

    annotations_ = loaded;
    rebuildBookmarkTable();
    emit statusToastRequested(makeToast(StatusToast::Level::Info, QStringLiteral("Annotations loaded.")));
}

void DtmEditorPage::rebuildValidationText()
{
    if (!dtm_.has_value()) {
        validationText_->setPlainText(QStringLiteral("No DTM loaded."));
        return;
    }

    QStringList lines;
    const auto report = dtm_->validate();
    if (report.issues.empty()) {
        lines << QStringLiteral("Validation: no issues detected.");
    }
    for (const auto& issue : report.issues) {
        QString prefix = QStringLiteral("[INFO]");
        if (issue.severity == simcore::tas::DtmValidationIssue::Severity::Warning) prefix = QStringLiteral("[WARN]");
        if (issue.severity == simcore::tas::DtmValidationIssue::Severity::Error) prefix = QStringLiteral("[ERR]");
        lines << QStringLiteral("%1 %2: %3").arg(prefix, QString::fromStdString(issue.code), QString::fromStdString(issue.message));
    }
    validationText_->setPlainText(lines.join('\n'));
}

void DtmEditorPage::rebuildPollTable()
{
    pollTable_->setRowCount(0);
    if (!dtm_.has_value()) return;

    std::string reason;
    if (!dtm_->supports_gc_poll_editing(&reason)) {
        pollTable_->setRowCount(1);
        pollTable_->setItem(0, 0, new QTableWidgetItem(QStringLiteral("N/A")));
        pollTable_->setItem(0, 1, new QTableWidgetItem(QString::fromStdString(reason)));
        return;
    }

    const size_t n = dtm_->gc_poll_count();
    pollTable_->setRowCount(static_cast<int>(n));
    for (size_t i = 0; i < n; ++i) {
        simcore::tas::DtmGCPoll poll{};
        if (!dtm_->read_gc_poll(i, poll)) continue;

        auto setItem = [this, i](int col, const QString& text) {
            pollTable_->setItem(static_cast<int>(i), col, new QTableWidgetItem(text));
        };

        setItem(0, QString::number(static_cast<qulonglong>(i)));
        setItem(1, QString::number(poll.button_bits));
        setItem(2, QString::number(poll.trigger_l));
        setItem(3, QString::number(poll.trigger_r));
        setItem(4, QString::number(poll.stick_x));
        setItem(5, QString::number(poll.stick_y));
        setItem(6, QString::number(poll.cstick_x));
        setItem(7, QString::number(poll.cstick_y));
    }
}

void DtmEditorPage::rebuildBookmarkTable()
{
    bookmarkTable_->setRowCount(0);
    if (!annotations_.has_value()) return;

    bookmarkTable_->setRowCount(static_cast<int>(annotations_->bookmarks.size()));
    for (size_t i = 0; i < annotations_->bookmarks.size(); ++i) {
        const auto& bm = annotations_->bookmarks[i];
        bookmarkTable_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(QString::fromStdString(bm.id)));
        bookmarkTable_->setItem(static_cast<int>(i), 1, new QTableWidgetItem(QString::fromStdString(bm.label)));
        bookmarkTable_->setItem(static_cast<int>(i), 2, new QTableWidgetItem(QString::number(static_cast<qulonglong>(bm.input_byte_offset))));

        QStringList tags;
        for (const auto& t : bm.tags) tags << QString::fromStdString(t);
        bookmarkTable_->setItem(static_cast<int>(i), 3, new QTableWidgetItem(tags.join(',')));
    }
}
