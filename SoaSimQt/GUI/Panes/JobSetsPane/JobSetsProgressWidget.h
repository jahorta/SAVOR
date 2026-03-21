#pragma once

#include <QtWidgets/QWidget>

class JobSetsProgressWidget final : public QWidget
{
public:
    explicit JobSetsProgressWidget(QWidget* parent = nullptr);

    void setProgress(qint64 succeeded, qint64 failed, qint64 canceled, qint64 completed, qint64 total);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qint64 succeeded_ = 0;
    qint64 failed_ = 0;
    qint64 canceled_ = 0;
    qint64 completed_ = 0;
    qint64 total_ = 0;
};
