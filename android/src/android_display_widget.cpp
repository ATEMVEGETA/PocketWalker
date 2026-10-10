#include "android_display_widget.h"

#include <algorithm>

#include <QImage>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QPushButton>
#include <QPixmap>
#include <QPolygonF>
#include <QResizeEvent>

namespace
{
constexpr int SCREEN_WIDTH = 96;
constexpr int SCREEN_HEIGHT = 64;
constexpr QRgb PALETTE[] = {
    qRgb(198, 202, 191),
    qRgb(151, 157, 148),
    qRgb(87, 91, 87),
    qRgb(45, 47, 45),
};
}

AndroidDisplayWidget::AndroidDisplayWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(300, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(true);
    QPalette background_palette = palette();
    background_palette.setColor(QPalette::Window, QColor(23, 25, 27));
    setPalette(background_palette);
}

void AndroidDisplayWidget::setEmulator(PocketWalker* value)
{
    emulator = value;
    update();
}

void AndroidDisplayWidget::setControlButtons(
    QPushButton* left, QPushButton* center, QPushButton* right)
{
    left_button = left;
    center_button = center;
    right_button = right;
    left_button->setParent(this);
    center_button->setParent(this);
    right_button->setParent(this);
    left_button->setAccessibleName("Left button");
    center_button->setAccessibleName("Center button");
    right_button->setAccessibleName("Right button");
    left_button->setToolTip("Left");
    center_button->setToolTip("Center");
    right_button->setToolTip("Right");
    resizeEvent(nullptr);
}

void AndroidDisplayWidget::paintEvent(QPaintEvent*)
{
    QImage frame(SCREEN_WIDTH, SCREEN_HEIGHT, QImage::Format_RGB32);
    frame.fill(PALETTE[0]);

    if (emulator)
    {
        NT7508DrawInfo draw = emulator->GetDrawInfoSnapshot();
        if (!draw.power_save)
        {
            for (int y = 0; y < SCREEN_HEIGHT; ++y)
            {
                const int row = (y + draw.initial_display_line) % NT7508_GRAPHICS_HEIGHT;
                const int page = row / 8;
                const int page_offset = page * NT7508_TOTAL_COLUMNS * NT7508_COLUMN_SIZE;
                const int bit = row % 8;
                auto* scanline = reinterpret_cast<QRgb*>(frame.scanLine(y));
                for (int x = 0; x < SCREEN_WIDTH; ++x)
                {
                    const int base = NT7508_COLUMN_SIZE * x + page_offset;
                    const uint8_t index = (((draw.vram.Read8(base) >> bit) & 1) << 1) |
                                          ((draw.vram.Read8(base + 1) >> bit) & 1);
                    scanline[x] = PALETTE[index];
                }
            }
        }
    }

    QPainter painter(this);
    painter.fillRect(rect(), QColor(23, 25, 27));
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF device = deviceRect();
    QPainterPath body;
    body.addEllipse(device);

    painter.save();
    painter.setClipPath(body);
    painter.fillRect(device, QColor(244, 245, 247));
    painter.fillRect(QRectF(device.left(), device.top(), device.width(), device.height() * 0.52),
                     QColor(225, 38, 62));
    painter.fillRect(QRectF(device.left(), device.top() + device.height() * 0.46,
                            device.width(), device.height() * 0.095), QColor(15, 16, 18));
    painter.restore();

    QPen outline(QColor(92, 96, 101));
    outline.setWidthF(std::max(2.0, device.width() * 0.008));
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(device.adjusted(2, 2, -2, -2));

    QPen highlight(QColor(255, 255, 255, 215));
    highlight.setCapStyle(Qt::RoundCap);
    highlight.setWidthF(std::max(3.0, device.width() * 0.018));
    painter.setPen(highlight);
    painter.drawArc(device.adjusted(device.width() * 0.13, device.height() * 0.08,
                                    -device.width() * 0.13, -device.height() * 0.08),
                    72 * 16, 58 * 16);

    const QRectF screen = screenRect(device);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 13, 14));
    painter.drawRoundedRect(screen.adjusted(-8, -8, 8, 8), 12, 12);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(screen, frame);
}

void AndroidDisplayWidget::resizeEvent(QResizeEvent* event)
{
    if (event)
        QWidget::resizeEvent(event);
    if (!left_button || !center_button || !right_button)
        return;

    const QRectF device = deviceRect();
    const int side_diameter = std::max(54, static_cast<int>(device.width() * 0.16));
    const int center_diameter = std::max(62, static_cast<int>(device.width() * 0.19));
    const qreal side_y = device.top() + device.height() * 0.77;
    const qreal center_y = device.top() + device.height() * 0.80;
    placeButton(left_button,
                QPointF(device.left() + device.width() * 0.27, side_y),
                side_diameter, std::max(20, side_diameter / 3));
    placeButton(center_button,
                QPointF(device.left() + device.width() * 0.50, center_y),
                center_diameter, std::max(20, center_diameter / 3));
    placeButton(right_button,
                QPointF(device.left() + device.width() * 0.73, side_y),
                side_diameter, std::max(20, side_diameter / 3));
    setArrowIcon(left_button, true, side_diameter);
    center_button->setIcon(QIcon());
    setArrowIcon(right_button, false, side_diameter);
    update();
}

QRectF AndroidDisplayWidget::deviceRect() const
{
    const qreal side = std::max<qreal>(0, std::min(width() - 12, height() - 12));
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

QRectF AndroidDisplayWidget::screenRect(const QRectF& device) const
{
    const qreal screen_width = device.width() * 0.66;
    const qreal screen_height = screen_width * SCREEN_HEIGHT / SCREEN_WIDTH;
    return {device.center().x() - screen_width / 2.0,
            device.top() + device.height() * 0.205,
            screen_width,
            screen_height};
}

void AndroidDisplayWidget::placeButton(
    QPushButton* button, const QPointF& center, const int diameter, const int font_size)
{
    button->setGeometry(static_cast<int>(center.x() - diameter / 2.0),
                        static_cast<int>(center.y() - diameter / 2.0),
                        diameter, diameter);
    button->setStyleSheet(QString(
        "QPushButton { background: #f8f8f9; color: #303236; border: 2px solid #777b80; "
        "border-radius: %1px; font-size: %2px; font-weight: 600; } "
        "QPushButton:pressed { background: #cfd2d5; border-color: #303236; } "
        "QPushButton:disabled { background: #b8bbbe; color: #777b80; }")
        .arg(diameter / 2).arg(font_size));
    button->raise();
}

void AndroidDisplayWidget::setArrowIcon(
    QPushButton* button, const bool points_left, const int diameter)
{
    const int icon_size = std::max(28, static_cast<int>(diameter * 0.62));
    QPixmap icon(icon_size, icon_size);
    icon.fill(Qt::transparent);

    const qreal near_edge = icon_size * 0.22;
    const qreal far_edge = icon_size * 0.78;
    const qreal top = icon_size * 0.12;
    const qreal bottom = icon_size * 0.88;
    const qreal middle = icon_size * 0.50;
    const qreal horizontal_shift = (points_left ? -1.0 : 1.0) * icon_size * 0.13;
    QPolygonF triangle;
    if (points_left)
        triangle << QPointF(near_edge + horizontal_shift, middle)
                 << QPointF(far_edge + horizontal_shift, top)
                 << QPointF(far_edge + horizontal_shift, bottom);
    else
        triangle << QPointF(far_edge + horizontal_shift, middle)
                 << QPointF(near_edge + horizontal_shift, top)
                 << QPointF(near_edge + horizontal_shift, bottom);

    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(218, 219, 221));
    painter.drawPolygon(triangle);
    painter.end();

    button->setIcon(QIcon(icon));
    button->setIconSize(QSize(icon_size, icon_size));
}
