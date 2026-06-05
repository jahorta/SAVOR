#pragma once

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class DtmEditorPage final : public QWidget
{
    Q_OBJECT

public:
    explicit DtmEditorPage(QWidget* parent = nullptr);

signals:
    void statusToastRequested(const StatusToast& toast);
};

