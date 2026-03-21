#include "JobSetsProgressDelegate.h"

#include "JobSetsTreeModel.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>

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

JobSetsProgressDelegate::JobSetsProgressDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void JobSetsProgressDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    if (!painter || !index.isValid()) {
        return;
    }

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    opt.text.clear();

    QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qint64 succeeded = index.data(JobSetsTreeModel::SucceededJobsRole).toLongLong();
    const qint64 failed = index.data(JobSetsTreeModel::FailedJobsRole).toLongLong();
    const qint64 canceled = index.data(JobSetsTreeModel::CanceledJobsRole).toLongLong();
    const qint64 completed = index.data(JobSetsTreeModel::CompletedJobsRole).toLongLong();
    const qint64 total = index.data(JobSetsTreeModel::TotalJobsRole).toLongLong();

    const QRectF outer = option.rect.adjusted(4.5, 2.5, -4.5, -2.5);
    const QColor background(31, 35, 43);
    const QColor border(46, 53, 66);
    const QColor success(30, 112, 52);
    const QColor failedColor(150, 45, 45);
    const QColor canceledColor(117, 88, 42);
    const QColor remaining(85, 92, 104);

    painter->setPen(border);
    painter->setBrush(background);
    painter->drawRoundedRect(outer, 6.0, 6.0);

    if (total > 0) {
        const qint64 successClamped = std::clamp<qint64>(succeeded, 0, total);
        const qint64 failedClamped = std::clamp<qint64>(failed, 0, total - successClamped);
        const qint64 canceledClamped = std::clamp<qint64>(canceled, 0, total - successClamped - failedClamped);
        const qint64 remain = std::max<qint64>(0, total - successClamped - failedClamped - canceledClamped);

        const qreal width = outer.width();
        qreal x = outer.left();

        auto drawSegment = [&](qint64 amount, const QColor& color, bool roundLeft, bool roundRight) {
            if (amount <= 0) {
                return;
            }

            const qreal segmentWidth = width * static_cast<qreal>(amount) / static_cast<qreal>(total);
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
                painter->fillPath(path, color);
            } else {
                painter->fillRect(segment, color);
            }
            x += segmentWidth;
        };

        const bool onlySuccess = failedClamped == 0 && canceledClamped == 0 && remain == 0;
        drawSegment(successClamped, success, true, onlySuccess);
        drawSegment(failedClamped, failedColor, successClamped == 0, canceledClamped == 0 && remain == 0);
        drawSegment(canceledClamped, canceledColor, successClamped == 0 && failedClamped == 0, remain == 0);
        drawSegment(remain, remaining, successClamped == 0 && failedClamped == 0 && canceledClamped == 0, true);
    }

    painter->setPen(QColor(244, 247, 250));
    painter->drawText(option.rect.adjusted(8, 0, -8, 0), Qt::AlignCenter, buildLabel(succeeded, failed, canceled, completed, total));
    painter->restore();
}

QSize JobSetsProgressDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    return {280, 24};
}
