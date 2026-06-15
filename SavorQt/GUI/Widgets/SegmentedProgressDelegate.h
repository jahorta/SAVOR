#pragma once

#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyledItemDelegate>

#include <algorithm>

namespace savorqt::gui {

struct SegmentedProgressRoles {
    static constexpr int Text = Qt::UserRole + 101;
    static constexpr int Done = Qt::UserRole + 102;
    static constexpr int Remaining = Qt::UserRole + 103;
    static constexpr int Failed = Qt::UserRole + 104;
    static constexpr int Canceled = Qt::UserRole + 105;
};

class SegmentedProgressDelegate final : public QStyledItemDelegate {
public:
    explicit SegmentedProgressDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (painter == nullptr) {
            return;
        }

        QStyleOptionViewItem itemOption(option);
        initStyleOption(&itemOption, index);
        itemOption.text.clear();
        const QWidget* widget = option.widget;
        QStyle* style = widget == nullptr ? QApplication::style() : widget->style();
        style->drawControl(QStyle::CE_ItemViewItem, &itemOption, painter, widget);

        const qint64 done = std::max<qint64>(0, index.data(SegmentedProgressRoles::Done).toLongLong());
        const qint64 remaining = std::max<qint64>(0, index.data(SegmentedProgressRoles::Remaining).toLongLong());
        const qint64 failed = std::max<qint64>(0, index.data(SegmentedProgressRoles::Failed).toLongLong());
        const qint64 canceled = std::max<qint64>(0, index.data(SegmentedProgressRoles::Canceled).toLongLong());
        const qint64 total = done + remaining + failed + canceled;

        const QRect barRect = option.rect.adjusted(8, 5, -8, -5);
        if (barRect.width() <= 0 || barRect.height() <= 0) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QColor baseColor(38, 44, 54);
        const QColor doneColor(30, 124, 61);
        const QColor remainingColor(105, 111, 122);
        const QColor failedColor(181, 59, 59);
        const QColor canceledColor(185, 147, 36);
        const QColor borderColor(63, 74, 92);

        painter->setPen(Qt::NoPen);
        painter->setBrush(baseColor);
        painter->drawRoundedRect(barRect, 6, 6);

        int x = barRect.left();
        auto drawSegment = [&](qint64 value, const QColor& color) {
            if (total <= 0 || value <= 0) {
                return;
            }
            const int remainingPixels = barRect.right() - x + 1;
            int width = static_cast<int>((static_cast<double>(value) / static_cast<double>(total)) * barRect.width());
            width = std::clamp(width, 1, remainingPixels);
            const QRect segmentRect(x, barRect.top(), width, barRect.height());
            painter->setBrush(color);
            painter->drawRect(segmentRect);
            x += width;
        };

        drawSegment(done, doneColor);
        drawSegment(remaining, remainingColor);
        drawSegment(failed, failedColor);
        drawSegment(canceled, canceledColor);

        painter->setBrush(Qt::NoBrush);
        painter->setPen(borderColor);
        painter->drawRoundedRect(barRect.adjusted(0, 0, -1, -1), 6, 6);

        const QString text = index.data(SegmentedProgressRoles::Text).toString();
        painter->setPen(Qt::white);
        painter->drawText(barRect.adjusted(8, 0, -8, 0), Qt::AlignCenter, text);
        painter->restore();
    }
};

} // namespace savorqt::gui
