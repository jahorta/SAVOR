#pragma once

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class JobBuilderPage final : public QWidget
{
    Q_OBJECT

public:
    explicit JobBuilderPage(QWidget* parent = nullptr);

signals:
    void statusToastRequested(StatusToast toast);
};

