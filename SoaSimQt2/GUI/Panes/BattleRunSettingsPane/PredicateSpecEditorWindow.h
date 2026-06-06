#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <QtCore/QByteArray>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Authoring/IAuthoringDb.h"

class QLabel;
class QCheckBox;
class QComboBox;
class QCloseEvent;
class QHBoxLayout;
class QButtonGroup;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QGridLayout;
class QFormLayout;
class QVBoxLayout;
class QWidget;

class PredicateSpecEditorWindow final : public QWidget
{
public:
    explicit PredicateSpecEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const simcore::db::PredicateSpecSnapshot& snapshot, bool duplicate);

private:
public:
    enum class ValueSourceMode : int {
        AbsoluteAddress = 0,
        AddrKey = 1,
        AddrProgram = 2,
        Immediate = 3,
    };

    enum class ProgramKind : int {
        None = 0,
        TurnOrderIndex = 1,
        ItemDropAmount = 2,
        BattleTreasureSlotAmount = 3,
        EnemyItemAmount = 4,
    };

    struct ProgramDraft {
        ProgramKind kind = ProgramKind::None;
        std::uint16_t a = 0;
        std::uint16_t b = 0;
        QString description;
        QByteArray blob;
    };

private:
    void closeEvent(QCloseEvent* event) override;
    void createWidgets();
    void createCoreSection();
    void createExecutionSection();
    void createFlagsSection();
    void createMatchSection();
    void savePredicate();
    void populateAddrKeys();
    void populateBreakpointCombo(QComboBox* combo) const;
    void addRequiredBreakpointField(int selectedBp = 0);
    void removeRequiredBreakpointField(int index);
    void rebuildRequiredBreakpointRows();
    std::vector<int> selectedRequiredBreakpoints() const;
    void setComparison(simcore::db::PredicateComparisonOp op);
    simcore::db::PredicateComparisonOp comparison() const;
    void applyProgramDraftToWidgets(const ProgramDraft& draft, bool lhs);
    ProgramDraft programDraftFromWidgets(bool lhs) const;
    void buildProgram(bool lhs);
    void refreshUi();
    std::vector<QString> validateDraft() const;
    void markDirty();
    bool confirmDiscardIfDirty();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    bool dirty_ = false;
    QLineEdit* nameEdit_ = nullptr;
    QWidget* requiredBpRowsWidget_ = nullptr;
    QVBoxLayout* requiredBpRowsLayout_ = nullptr;
    QPushButton* addRequiredBpButton_ = nullptr;
    std::vector<QComboBox*> breakpointCombos_;
    std::vector<QPushButton*> removeBreakpointButtons_;
    std::vector<QHBoxLayout*> breakpointRowLayouts_;
    QLineEdit* turnMaskEdit_ = nullptr;
    QComboBox* lhsKindCombo_ = nullptr;
    QLineEdit* lhsValueEdit_ = nullptr;
    QComboBox* rhsKindCombo_ = nullptr;
    QComboBox* lhsModeCombo_ = nullptr;
    QLineEdit* rhsValueEdit_ = nullptr;
    QComboBox* rhsModeCombo_ = nullptr;
    QComboBox* lhsKeyCombo_ = nullptr;
    QComboBox* rhsKeyCombo_ = nullptr;
    QComboBox* lhsProgramKindCombo_ = nullptr;
    QLineEdit* lhsProgramAEdit_ = nullptr;
    QLineEdit* lhsProgramBEdit_ = nullptr;
    QPushButton* lhsBuildProgramButton_ = nullptr;
    QLabel* lhsProgramSummaryLabel_ = nullptr;
    QComboBox* rhsProgramKindCombo_ = nullptr;
    QLineEdit* rhsProgramAEdit_ = nullptr;
    QLineEdit* rhsProgramBEdit_ = nullptr;
    QPushButton* rhsBuildProgramButton_ = nullptr;
    QLabel* rhsProgramSummaryLabel_ = nullptr;
    QButtonGroup* cmpButtonGroup_ = nullptr;
    QFormLayout* lhsMatchLayout_ = nullptr;
    QFormLayout* rhsMatchLayout_ = nullptr;
    QComboBox* widthCombo_ = nullptr;
    QCheckBox* abortOnFailCheck_ = nullptr;
    QCheckBox* activeCheck_ = nullptr;
    QCheckBox* captureCheck_ = nullptr;
    QCheckBox* lhsNegateCheck_ = nullptr;
    QCheckBox* rhsNegateCheck_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QVBoxLayout* requiredBpRowsMainLayout_ = nullptr;
    QVector<int> addrKeys_;
    QVector<QString> addrNames_;
    ProgramDraft lhsProgramDraft_{};
    ProgramDraft rhsProgramDraft_{};
    QVBoxLayout* contentLayout_ = nullptr;
};
