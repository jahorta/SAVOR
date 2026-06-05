#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class CoordinatorController;

class CoordinatorPane : public QWidget
{
    Q_OBJECT

public:
    enum class SettingsFocusTarget {
        CoordinatorSection,
        IsoPath,
        DolphinBaseDir
    };

    explicit CoordinatorPane(CoordinatorController* controller, QWidget* parent = nullptr);
    void setPageActive(bool active);

signals:
    void settingsNavigationRequested(SettingsFocusTarget target);
    void statusToastRequested(StatusToast toast);

public slots:
    void requestVisualReplay(qint64 jobId);
};

