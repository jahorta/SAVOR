#include "JobSetsProgressWidget.h"

#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainterPath>

#include <algorithm>

namespace {
QString buildLabel(qint64 succeeded, qint64 failed, qint64 canceled, qint64 completed, qint64 total)
{
    const qint64 remain = std::max<qint64>(0, total - succeeded - failed - canceled);
    return QStringLiteral("ok:%1 rem:%2 fail:%3 can:%4 done:%5/%6")
        .arg(succeeded)
        .arg(remain)
        .arg(failed)
        .arg(canceled)
        .arg(completed)
        .arg(total);
}
}

JobSetsProgressWidget::JobSetsProgressWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setMinimumHeight(22);
}

void JobSetsProgressWidget::setProgress(qint64 succeeded, qint64 failed, qint64 canceled, qint64 completed, qint64 total)
{
    succeeded_ = succeeded;
    failed_ = failed;
    canceled_ = canceled;
    completed_ = completed;
    total_ = total;
    update();
}

QSize JobSetsProgressWidget::sizeHint() const
{
    return {280, 24};
}

void JobSetsProgressWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF outer = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    const QColor background(31, 35, 43);
    const QColor border(46, 53, 66);
    const QColor success(30, 112, 52);
    const QColor failed(150, 45, 45);
    const QColor canceled(117, 88, 42);
    const QColor remaining(85, 92, 104);

    painter.setPen(border);
    painter.setBrush(background);
    painter.drawRoundedRect(outer, 6.0, 6.0);

    if (total_ > 0) {
        const qint64 successClamped = std::clamp<qint64>(succeeded_, 0, total_);
        const qint64 failedClamped = std::clamp<qint64>(failed_, 0, total_ - successClamped);
        const qint64 canceledClamped = std::clamp<qint64>(canceled_, 0, total_ - successClamped - failedClamped);
        const qint64 remain = std::max<qint64>(0, total_ - successClamped - failedClamped - canceledClamped);

        const qreal width = outer.width();
        qreal x = outer.left();

        auto drawSegment = [&](qint64 amount, const QColor& color, bool roundLeft, bool roundRight) {
            if (amount <= 0) {
                return;
            }

            const qreal segmentWidth = width * static_cast<qreal>(amount) / static_cast<qreal>(total_);
            QRectF segment(x, outer.top(), segmentWidth, outer.height());
            QPainterPath path;
            constexpr qreal radius = 6.0;

            if (roundLeft || roundRight) {
                path.addRoundedRect(segment, radius, radius);
                if (!roundLeft) {
                    path.addRect(segment.left(), segment.top(), radius, segment.height());
                }
                if (!roundRight) {
                    path.addRect(segment.right() - radius, segment.top(), radius, segment.height());
                }
                painter.fillPath(path, color);
            } else {
                painter.fillRect(segment, color);
            }
            x += segmentWidth;
        };

        const bool onlySuccess = failedClamped == 0 && canceledClamped == 0 && remain == 0;
        drawSegment(successClamped, success, true, onlySuccess);
        drawSegment(failedClamped, failed, successClamped == 0, canceledClamped == 0 && remain == 0);
        drawSegment(canceledClamped, canceled, successClamped == 0 && failedClamped == 0, remain == 0);
        drawSegment(remain, remaining, successClamped == 0 && failedClamped == 0 && canceledClamped == 0, true);
    }

    const QString label = buildLabel(succeeded_, failed_, canceled_, completed_, total_);
    painter.setPen(QColor(244, 247, 250));
    painter.drawText(rect(), Qt::AlignCenter, label);
}
