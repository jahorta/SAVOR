#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>

#include "DB/PredicateSpecRepo.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTextEdit;
class QVBoxLayout;

class PredicateEditorDialog final : public QDialog
{
public:
    enum class ValueSourceMode : int {
        AbsoluteAddress = 0,
        AddrKey = 1,
        AddrProgram = 2,
        Immediate = 3,
    };

    enum class ProgramKind : int {
        None = 0,
        TurnOrderIndex,
        ItemDropAmount,
        BattleTreasureSlotAmount,
        EnemyItemAmount,
    };

    struct ProgramDraft {
        ProgramKind kind = ProgramKind::None;
        quint16 a = 0;
        quint16 b = 0;
        QString description;
        QByteArray blob;
    };

    explicit PredicateEditorDialog(QWidget* parent = nullptr);

    void loadRow(const simcore::db::PredicateSpecRow& row);
    simcore::db::PredicateSpecRow buildRow(bool* ok, QString* errorText) const;
    QByteArray lhsProgramBlob() const;
    QString lhsProgramDescription() const;
    QByteArray rhsProgramBlob() const;
    QString rhsProgramDescription() const;

private:
    void accept() override;
    void populateAddrKeys();
    void populateBreakpointCombo(QComboBox* combo) const;
    void addRequiredBreakpointField(int selectedBp = 0);
    void removeRequiredBreakpointField(int index);
    void rebuildRequiredBreakpointRows();
    QVector<int> selectedRequiredBreakpoints() const;
    void applyProgramDraftToWidgets(const ProgramDraft& draft, bool lhs);
    ProgramDraft programDraftFromWidgets(bool lhs) const;
    void buildProgram(bool lhs);
    void refreshUi();
    QStringList validateDraft() const;
    void setDialogErrors(const QStringList& errors);

    simcore::db::PredicateSpecRow row_{};
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QWidget* requiredBpRowsWidget_ = nullptr;
    QVBoxLayout* requiredBpRowsLayout_ = nullptr;
    QPushButton* addRequiredBpButton_ = nullptr;
    QVector<QComboBox*> breakpointCombos_;
    QVector<QPushButton*> removeBreakpointButtons_;
    QVector<QHBoxLayout*> breakpointRowLayouts_;
    QComboBox* kindCombo_ = nullptr;
    QComboBox* widthCombo_ = nullptr;
    QComboBox* cmpCombo_ = nullptr;
    QComboBox* lhsModeCombo_ = nullptr;
    QLineEdit* lhsAddrEdit_ = nullptr;
    QComboBox* lhsKeyCombo_ = nullptr;
    QComboBox* lhsProgramKindCombo_ = nullptr;
    QLineEdit* lhsProgramAEdit_ = nullptr;
    QLineEdit* lhsProgramBEdit_ = nullptr;
    QPushButton* lhsBuildProgramButton_ = nullptr;
    QLabel* lhsProgramSummaryLabel_ = nullptr;
    QComboBox* rhsModeCombo_ = nullptr;
    QLineEdit* rhsValueEdit_ = nullptr;
    QComboBox* rhsKeyCombo_ = nullptr;
    QComboBox* rhsProgramKindCombo_ = nullptr;
    QLineEdit* rhsProgramAEdit_ = nullptr;
    QLineEdit* rhsProgramBEdit_ = nullptr;
    QPushButton* rhsBuildProgramButton_ = nullptr;
    QLabel* rhsProgramSummaryLabel_ = nullptr;
    QLineEdit* turnMaskEdit_ = nullptr;
    QCheckBox* activeCheck_ = nullptr;
    QCheckBox* abortCheck_ = nullptr;
    QCheckBox* captureCheck_ = nullptr;
    QCheckBox* lhsNegateCheck_ = nullptr;
    QCheckBox* rhsNegateCheck_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    QVector<int> addrKeys_;
    QVector<QString> addrNames_;
    ProgramDraft lhsProgramDraft_{};
    ProgramDraft rhsProgramDraft_{};
};
