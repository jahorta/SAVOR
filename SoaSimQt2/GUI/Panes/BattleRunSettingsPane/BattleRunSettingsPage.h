#pragma once

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class BattleRunSettingsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit BattleRunSettingsPage(QWidget* parent = nullptr);
    ~BattleRunSettingsPage() override = default;

signals:
    void statusToastRequested(StatusToast toast);
};

