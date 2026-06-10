#include "SeedProbeGridWidget.h"

#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPen>

#include <algorithm>
#include <cmath>

namespace {
QColor rgba(quint8 r, quint8 g, quint8 b, quint8 a = 255)
{
    return QColor(r, g, b, a);
}

QColor lerpColor(const QColor& a, const QColor& b, float t)
{
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return QColor(
        static_cast<int>(std::round(a.red() + (b.red() - a.red()) * clamped)),
        static_cast<int>(std::round(a.green() + (b.green() - a.green()) * clamped)),
        static_cast<int>(std::round(a.blue() + (b.blue() - a.blue()) * clamped)),
        static_cast<int>(std::round(a.alpha() + (b.alpha() - a.alpha()) * clamped)));
}

QColor hsvToRgb(float hDeg, float s, float v)
{
    const float h = std::fmod(std::fmod(hDeg, 360.0f) + 360.0f, 360.0f);
    return QColor::fromHsvF(h / 360.0f, std::clamp(s, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f));
}
}

QColor SeedProbeGridWidget::colorForDelta(int delta, int minNeg, int maxPos)
{
    if (delta == 0) {
        return rgba(128, 128, 128);
    }

    maxPos = std::max(maxPos, 1);
    minNeg = std::min(minNeg, -1);

    if (delta < 0) {
        const int dn = std::clamp(delta, minNeg, 0);
        const float t = static_cast<float>(dn - minNeg) / static_cast<float>(0 - minNeg);
        return lerpColor(rgba(59, 10, 87), rgba(176, 123, 227), t);
    }

    const int d = std::clamp(delta, 0, maxPos);
    const float t = std::pow(static_cast<float>(d) / static_cast<float>(maxPos), 0.6f);
    float h = 0.0f;
    if (t < 0.25f) h = 210.0f + (120.0f - 210.0f) * (t / 0.25f);
    else if (t < 0.50f) h = 120.0f + (60.0f - 120.0f) * ((t - 0.25f) / 0.25f);
    else if (t < 0.75f) h = 60.0f + (30.0f - 60.0f) * ((t - 0.50f) / 0.25f);
    else h = 30.0f + (0.0f - 30.0f) * ((t - 0.75f) / 0.25f);

    static constexpr float values[3] = { 0.90f, 0.70f, 0.50f };
    return hsvToRgb(h, 0.90f, values[d % 3]);
}

SeedProbeGridWidget::SeedProbeGridWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(180, 180);
}

void SeedProbeGridWidget::setTitle(const QString& title)
{
    title_ = title;
    update();
}

void SeedProbeGridWidget::setGridData(const GridData& data)
{
    const bool unchanged = data_.hasData == data.hasData
        && data_.minNeg == data.minNeg
        && data_.maxPos == data.maxPos
        && data_.cells.size() == data.cells.size()
        && std::equal(data_.cells.begin(), data_.cells.end(), data.cells.begin(), [](const Cell& lhs, const Cell& rhs) {
            return lhs.x == rhs.x
                && lhs.y == rhs.y
                && lhs.xSpan == rhs.xSpan
                && lhs.ySpan == rhs.ySpan
                && lhs.delta == rhs.delta;
        });
    if (unchanged) {
        return;
    }

    data_ = data;
    update();
}

QSize SeedProbeGridWidget::minimumSizeHint() const
{
    return {180, 210};
}

QSize SeedProbeGridWidget::sizeHint() const
{
    return {220, 238};
}

void SeedProbeGridWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRect full = rect().adjusted(0, 0, -1, -1);
    painter.fillRect(full, QColor(25, 29, 36));
    painter.setPen(QColor(46, 53, 66));
    painter.drawRect(full);

    const QRect titleRect = QRect(full.left() + 8, full.top() + 6, full.width() - 16, 18);
    painter.setPen(QColor(230, 234, 239));
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title_);

    const int side = std::max(32, std::min(full.width() - 16, full.height() - 34));
    const QRect plotRect(full.left() + (full.width() - side) / 2, titleRect.bottom() + 6, side, side);

    painter.fillRect(plotRect, Qt::black);
    painter.setPen(QColor(46, 53, 66));
    painter.drawRect(plotRect);

    if (!data_.hasData || data_.cells.isEmpty()) {
        painter.setPen(QColor(170, 176, 186));
        painter.drawText(plotRect, Qt::AlignCenter, QStringLiteral("No data"));
        return;
    }

    const qreal scaleX = static_cast<qreal>(plotRect.width()) / 256.0;
    const qreal scaleY = static_cast<qreal>(plotRect.height()) / 256.0;
    constexpr qreal bleed = 0.35;

    painter.setPen(Qt::NoPen);
    for (const Cell& cell : data_.cells) {
        const int x0 = std::clamp(cell.x, 0, 255);
        const int y0 = std::clamp(cell.y, 0, 255);
        const int x1 = std::clamp(cell.x + std::max(cell.xSpan, 1), 0, 256);
        const int y1 = std::clamp(cell.y + std::max(cell.ySpan, 1), 0, 256);
        QRectF r(
            plotRect.left() + x0 * scaleX,
            plotRect.top() + y0 * scaleY,
            std::max<qreal>(1.0, (x1 - x0) * scaleX),
            std::max<qreal>(1.0, (y1 - y0) * scaleY));
        r.adjust(-bleed, -bleed, bleed, bleed);
        r = r.intersected(QRectF(plotRect));
        painter.fillRect(r, colorForDelta(cell.delta, data_.minNeg, data_.maxPos));
    }

    painter.setPen(QPen(QColor(18, 21, 25), 1.0));
    painter.drawRect(plotRect);
}
