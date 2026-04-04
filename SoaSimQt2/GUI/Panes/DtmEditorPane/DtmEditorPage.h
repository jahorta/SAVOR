#pragma once

#include <optional>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Tas/DtmAnnotationFile.h"
#include "Tas/DtmFile.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTextEdit;

class DtmEditorPage final : public QWidget
{
    Q_OBJECT

public:
    explicit DtmEditorPage(QWidget* parent = nullptr);

signals:
    void statusToastRequested(const StatusToast& toast);

private slots:
    void onPickDtm();
    void onSaveDtmAs();
    void onInsertPoll();
    void onDeletePoll();
    void onApplyPoll();
    void onAddBookmark();
    void onSaveAnnotations();
    void onLoadAnnotations();
    void onDetectInputStream();

private:
    void rebuildValidationText();
    void rebuildPollTable();
    void rebuildBookmarkTable();

    std::optional<simcore::tas::DtmFile> dtm_;
    std::optional<simcore::tas::DtmAnnotationDoc> annotations_;
    QString loadedDtmPath_;

    QLabel* fileLabel_ = nullptr;
    QLabel* hashLabel_ = nullptr;
    QTextEdit* validationText_ = nullptr;
    QTableWidget* pollTable_ = nullptr;
    QSpinBox* buttonBitsSpin_ = nullptr;
    QSpinBox* triggerLSpin_ = nullptr;
    QSpinBox* triggerRSpin_ = nullptr;
    QSpinBox* stickXSpin_ = nullptr;
    QSpinBox* stickYSpin_ = nullptr;
    QSpinBox* cstickXSpin_ = nullptr;
    QSpinBox* cstickYSpin_ = nullptr;
    QTableWidget* bookmarkTable_ = nullptr;
    QLineEdit* bookmarkLabelEdit_ = nullptr;
    QLineEdit* bookmarkTagsEdit_ = nullptr;
    QTextEdit* bookmarkNoteEdit_ = nullptr;
};
