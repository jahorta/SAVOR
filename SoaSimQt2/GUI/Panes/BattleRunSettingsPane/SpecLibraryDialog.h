#pragma once

#include <cstdint>
#include <vector>

#include <QtWidgets/QDialog>

#include "GUI/Common/StatusToast.h"
#include "Authoring/IAuthoringDb.h"

class QLabel;
class QListWidget;
class QPushButton;
class QVBoxLayout;
class QSplitter;
class QFrame;

class SeedProbeSpecEditorWindow;
class TasSpecEditorWindow;
class BattleRunSpecEditorWindow;
class PredicateSpecEditorWindow;
class BattlePlanEditorWindow;

class SpecLibraryDialog final : public QDialog
{
    Q_OBJECT

public:
    enum class SpecKind {
        SeedProbe,
        Tas,
        BattleRun,
        Predicate,
        BattlePlan,
    };

    explicit SpecLibraryDialog(SpecKind kind, QWidget* parent = nullptr);

signals:
    void statusToastRequested(StatusToast toast);

private:
    void createWidgets();
    void refreshLibrary();
    void refreshSeedProbeSpecs();
    void refreshTasSpecs();
    void refreshBattleRunSpecs();
    void refreshPredicates();
    void refreshBattlePlans();
    void clearRightPane();
    void installEditorWidget(QWidget* editor);
    void openNewSeedProbeSpecEditor();
    void openNewTasSpecEditor();
    void openNewBattleRunSpecEditor();
    void openNewPredicateEditor();
    void openNewBattlePlanEditor();
    void handleLibrarySelectionChanged();
    void onLibraryActivated();
    void showForRow(int row, bool duplicate);
    void showEditorForSeedProbe(std::int64_t row, bool duplicate);
    void showEditorForTas(std::int64_t row, bool duplicate);
    void showEditorForBattleRun(std::int64_t row, bool duplicate);
    void showEditorForPredicate(std::int64_t row, bool duplicate);
    void showEditorForBattlePlan(std::int64_t row, bool duplicate);
    void postStatusMessage(const QString& text, StatusToast::Severity severity);
    void updateEditButtonState();
    int selectedLibraryRow() const;

    static QString seedProbeText(const simcore::db::SeedProbeSpecSnapshot& snapshot);
    static QString tasText(const simcore::db::TasSpecSnapshot& snapshot);
    static QString battleRunText(const simcore::db::BattleRunSpecSnapshot& snapshot);
    static QString predicateText(const simcore::db::PredicateSpecSnapshot& snapshot);
    static QString battlePlanText(const simcore::db::BattlePlanSnapshot& snapshot);
    QString dialogTitle() const;
    QString libraryCountText(std::size_t count) const;

    SpecKind kind_ = SpecKind::SeedProbe;
    QLabel* libraryStatusLabel_ = nullptr;
    QListWidget* libraryList_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QWidget* rightPane_ = nullptr;
    QVBoxLayout* rightPaneLayout_ = nullptr;
    QLabel* placeholderLabel_ = nullptr;
    QWidget* activeEditor_ = nullptr;
    QSplitter* splitter_ = nullptr;

    std::vector<simcore::db::SeedProbeSpecSnapshot> seedProbeSpecs_;
    std::vector<simcore::db::TasSpecSnapshot> tasSpecs_;
    std::vector<simcore::db::BattleRunSpecSnapshot> battleRunSpecs_;
    std::vector<simcore::db::PredicateSpecSnapshot> predicateSpecs_;
    std::vector<simcore::db::BattlePlanSnapshot> battlePlanSpecs_;
};
