#pragma once

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class ExplorerRunsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ExplorerRunsPage(QWidget* parent = nullptr);
    ~ExplorerRunsPage() override = default;
    void setPageActive(bool active);

signals:
    void visualReplayRequested(qint64 jobId);
    void statusToastRequested(StatusToast toast);
};

