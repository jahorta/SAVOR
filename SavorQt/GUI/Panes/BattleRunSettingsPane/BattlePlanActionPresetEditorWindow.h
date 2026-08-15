#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "DB/SavorDbAuthoringService.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class BattlePlanActionPresetEditorWindow final : public QWidget
{
public:
    explicit BattlePlanActionPresetEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::BattlePlanActionPresetSnapshot& snapshot, bool duplicate);
    void loadNew();

private:
    void createWidgets();
    void syncControlsForPresetMode();
    void refreshTitleForMode();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);
    bool buildDraft(savorqt::db::BattlePlanActionPresetDraft& draft, QString* errorText) const;
    bool canSaveCurrentInPlace(const savorqt::db::BattlePlanActionPresetDraft& draft) const;
    void saveAsNew();
    void saveCurrent();
    void applyLoadedSnapshot(const savor::db::BattlePlanActionPresetSnapshot& snapshot);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;

    std::optional<std::int64_t> loadedPresetId_;
    savor::db::BattlePlanActionPresetSnapshot loadedSnapshot_{};
    bool loadedFromSnapshot_ = false;

    QLineEdit* nameEdit_ = nullptr;
    QComboBox* macroCombo_ = nullptr;
    QComboBox* targetKindCombo_ = nullptr;
    QSpinBox* targetSingleSlotSpin_ = nullptr;
    QLineEdit* targetMaskEdit_ = nullptr;
    QSpinBox* targetSameAsActorSpin_ = nullptr;
    QLineEdit* itemIdEdit_ = nullptr;
    QLabel* noteLabel_ = nullptr;

    QPushButton* saveAsNewButton_ = nullptr;
    QPushButton* updateCurrentButton_ = nullptr;
};
