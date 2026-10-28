#include "image_comparison_dialog.h"
#include "theme.h"

#include <QComboBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPointF>
#include <QPushButton>
#include <QRectF>
#include <QResizeEvent>
#include <QSizeF>
#include <QString>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace
{
    constexpr double kMinScale = 0.01;
    constexpr double kMaxScale = 32.0;
    constexpr double kHeaderHeight = 28.0;
    constexpr double kZoomStep = 1.25;

    class ImageComparisonCanvas final: public QWidget
    {
    public:

        ImageComparisonCanvas(const QImage &original,
            const QImage &undistorted, QWidget *parent)
            : QWidget(parent), original_(original), undistorted_(undistorted)
        {
            setObjectName(QStringLiteral("imageComparisonCanvas"));
            setMinimumSize(320, 200);
            setMouseTracking(true);
            setCursor(Qt::OpenHandCursor);
            if (original_.isNull() || undistorted_.isNull())
            {
                error_ = tr("无法对比：原图或去畸变图片为空。");
            }
            else if (original_.size() != undistorted_.size())
            {
                error_ = tr("无法对比：两幅图片的尺寸不一致（%1×%2 与 %3×%4）。")
                        .arg(original_.width()).arg(original_.height())
                        .arg(undistorted_.width()).arg(undistorted_.height());
            }
            if (!isValid())
            {
                setCursor(Qt::ArrowCursor);
            }
        }

        bool isValid() const
        {
            return error_.isEmpty();
        }

        void setScaleChangedCallback(std::function<void(double)> callback)
        {
            scaleChanged_ = std::move(callback);
            notifyScale();
        }

        void setWipeMode(bool enabled)
        {
            wipeMode_ = enabled;
            drag_ = Drag::None;
            notifyScale();
            update();
        }

        void fitToWindow()
        {
            fitMode_ = true;
            pan_ = {};
            notifyScale();
            update();
        }

        void actualSize()
        {
            fitMode_ = false;
            scale_ = 1.0;
            pan_ = {};
            notifyScale();
            update();
        }

        void zoomBy(double factor)
        {
            zoomAt(factor, paneArea({0.0, 0.0}).center());
        }

    protected:

        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), Theme::kCanvasDark);
            if (!isValid())
            {
                painter.setPen(Qt::white);
                painter.drawText(rect().adjusted(24, 24, -24, -24),
                    Qt::AlignCenter | Qt::TextWordWrap, error_);
                return;
            }
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            if (wipeMode_)
            {
                const QRectF area = paneArea({});
                const double divider = area.left() + split_ * area.width();
                drawImage(painter, original_, area,
                    QRectF(area.left(), area.top(), divider - area.left(), area.height()));
                drawImage(painter, undistorted_, area,
                    QRectF(divider, area.top(), area.right() - divider, area.height()));
                painter.setPen(QPen(QColor(96, 165, 250), 2.0));
                painter.drawLine(QPointF(divider, area.top()),
                    QPointF(divider, area.bottom()));
                painter.setBrush(QColor(96, 165, 250));
                painter.drawRoundedRect(
                    QRectF(divider - 6.0, area.center().y() - 18.0, 12.0, 36.0), 4.0, 4.0);
                drawHeader(painter, QRectF(0, 0, width() / 2.0, kHeaderHeight), tr("原图"));
                drawHeader(painter, QRectF(width() / 2.0, 0, width() / 2.0, kHeaderHeight),
                    tr("去畸变图"));
            }
            else
            {
                const QRectF left = paneArea({0.0, 0.0});
                const QRectF right = paneArea({static_cast<double>(width()), 0.0});
                drawImage(painter, original_, left, left);
                drawImage(painter, undistorted_, right, right);
                drawHeader(painter, QRectF(left.left(), 0, left.width(), kHeaderHeight), tr("原图"));
                drawHeader(painter, QRectF(right.left(), 0, right.width(), kHeaderHeight),
                    tr("去畸变图"));
                painter.setPen(QColor(110, 115, 120));
                painter.drawLine(QPointF(left.right(), 0), QPointF(left.right(), height()));
            }
        }

        void resizeEvent(QResizeEvent *event) override
        {
            QWidget::resizeEvent(event);
            notifyScale();
        }

        void wheelEvent(QWheelEvent *event) override
        {
            if (!isValid() || event->position().y() < kHeaderHeight)
            {
                event->ignore();
                return;
            }
            const double delta = event->angleDelta().y() != 0
                    ? event->angleDelta().y() / 120.0
                    : event->pixelDelta().y() / 120.0;
            zoomAt(std::pow(kZoomStep, delta), event->position());
            event->accept();
        }

        void mousePressEvent(QMouseEvent *event) override
        {
            if (!isValid() || event->button() != Qt::LeftButton
                || event->position().y() < kHeaderHeight)
            {
                event->ignore();
                return;
            }
            drag_ = nearDivider(event->position()) ? Drag::Divider : Drag::Pan;
            lastPosition_ = event->position();
            setCursor(drag_ == Drag::Divider ? Qt::SplitHCursor : Qt::ClosedHandCursor);
            event->accept();
        }

        void mouseMoveEvent(QMouseEvent *event) override
        {
            if (!isValid())
            {
                return;
            }
            if (drag_ == Drag::Divider)
            {
                split_ = std::clamp(event->position().x() / std::max(1, width()), 0.0, 1.0);
                update();
            }
            else if (drag_ == Drag::Pan)
            {
                scale_ = currentScale();
                fitMode_ = false;
                pan_ += event->position() - lastPosition_;
                lastPosition_ = event->position();
                update();
            }
            else
            {
                setCursor(nearDivider(event->position()) ? Qt::SplitHCursor : Qt::OpenHandCursor);
            }
        }

        void mouseReleaseEvent(QMouseEvent *event) override
        {
            if (event->button() == Qt::LeftButton)
            {
                drag_ = Drag::None;
                setCursor(!isValid() ? Qt::ArrowCursor
                    : nearDivider(event->position()) ? Qt::SplitHCursor : Qt::OpenHandCursor);
                event->accept();
            }
        }

    private:

        enum class Drag { None, Pan, Divider };

        QRectF paneArea(const QPointF &position) const
        {
            const double paneWidth = wipeMode_ ? width() : width() / 2.0;
            const double left = !wipeMode_ && position.x() >= paneWidth ? paneWidth : 0.0;
            return {left, kHeaderHeight, paneWidth, std::max(1.0, height() - kHeaderHeight)};
        }

        double currentScale() const
        {
            if (!fitMode_ || !isValid())
            {
                return scale_;
            }
            const QRectF area = paneArea({});
            const double fit = std::min(
                std::max(1.0, area.width() - 24.0) / original_.width(),
                std::max(1.0, area.height() - 24.0) / original_.height());
            return std::clamp(fit, kMinScale, kMaxScale);
        }

        void zoomAt(double factor, const QPointF &position)
        {
            if (!isValid())
            {
                return;
            }
            const double oldScale = currentScale();
            const double newScale = std::clamp(oldScale * factor, kMinScale, kMaxScale);
            const QPointF relative = position - paneArea(position).center();
            // 同一图像坐标在缩放前后保持在鼠标下，另一个窗格复用相同变换。
            pan_ = relative - (relative - pan_) * (newScale / oldScale);
            scale_ = newScale;
            fitMode_ = false;
            notifyScale();
            update();
        }

        bool nearDivider(const QPointF &position) const
        {
            return wipeMode_ && position.y() >= kHeaderHeight
                    && std::abs(position.x() - split_ * width()) <= 8.0;
        }

        void notifyScale()
        {
            if (scaleChanged_ && isValid())
            {
                scaleChanged_(currentScale());
            }
        }

        void drawImage(QPainter &painter, const QImage &image,
            const QRectF &area, const QRectF &clip) const
        {
            const double scale = currentScale();
            const QSizeF scaledSize(image.width() * scale, image.height() * scale);
            const QPointF topLeft = area.center() + pan_
                    - QPointF(scaledSize.width() / 2.0, scaledSize.height() / 2.0);
            painter.save();
            painter.setClipRect(clip);
            painter.drawImage(QRectF(topLeft, scaledSize), image);
            painter.restore();
        }

        void drawHeader(QPainter &painter, const QRectF &area, const QString &label) const
        {
            painter.fillRect(area, QColor(55, 59, 64));
            painter.setPen(Qt::white);
            painter.drawText(area, Qt::AlignCenter, label);
        }

        QImage original_;
        QImage undistorted_;
        QString error_;
        bool wipeMode_ = false;
        bool fitMode_ = true;
        double scale_ = 1.0;
        double split_ = 0.5;
        QPointF pan_;
        QPointF lastPosition_;
        Drag drag_ = Drag::None;
        std::function<void(double)> scaleChanged_;
    };
}

ImageComparisonDialog::ImageComparisonDialog(const QImage &original,
    const QImage &undistorted, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("imageComparisonDialog"));
    setWindowTitle(tr("去畸变前后对比"));
    resize(1100, 720);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(10);
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);
    auto *mode = new QComboBox(this);
    mode->setObjectName(QStringLiteral("imageComparisonModeCombo"));
    mode->addItem(tr("并排对比"), 0);
    mode->addItem(tr("拖动分割线"), 1);
    toolbar->addWidget(new QLabel(tr("显示方式："), this));
    toolbar->addWidget(mode);
    toolbar->addSpacing(12);
    auto *zoomOut = new QPushButton(tr("缩小"), this);
    zoomOut->setObjectName(QStringLiteral("imageComparisonZoomOutButton"));
    auto *zoomIn = new QPushButton(tr("放大"), this);
    zoomIn->setObjectName(QStringLiteral("imageComparisonZoomInButton"));
    auto *fit = new QPushButton(tr("适应窗口"), this);
    fit->setObjectName(QStringLiteral("imageComparisonFitButton"));
    auto *actual = new QPushButton(QStringLiteral("100%"), this);
    actual->setObjectName(QStringLiteral("imageComparisonActualSizeButton"));
    auto *zoomLabel = new QLabel(this);
    zoomLabel->setObjectName(QStringLiteral("imageComparisonZoomLabel"));
    zoomLabel->setMinimumWidth(60);
    zoomLabel->setAlignment(Qt::AlignCenter);
    zoomLabel->setProperty("role", QStringLiteral("muted"));
    toolbar->addWidget(zoomOut);
    toolbar->addWidget(zoomIn);
    toolbar->addWidget(zoomLabel);
    toolbar->addWidget(fit);
    toolbar->addWidget(actual);
    toolbar->addStretch();
    layout->addLayout(toolbar);

    auto *canvas = new ImageComparisonCanvas(original, undistorted, this);
    layout->addWidget(canvas, 1);
    canvas->setScaleChangedCallback([zoomLabel](double scale)
        {
            zoomLabel->setText(QStringLiteral("%1%").arg(scale * 100.0, 0, 'f', 0));
        });
    const bool valid = canvas->isValid();
    mode->setEnabled(valid);
    zoomOut->setEnabled(valid);
    zoomIn->setEnabled(valid);
    fit->setEnabled(valid);
    actual->setEnabled(valid);
    if (!valid)
    {
        zoomLabel->setText(QStringLiteral("--"));
    }
    connect(mode, &QComboBox::currentIndexChanged, canvas,
        [canvas, mode] { canvas->setWipeMode(mode->currentData().toInt() == 1); });
    connect(zoomOut, &QPushButton::clicked, canvas,
        [canvas] { canvas->zoomBy(1.0 / kZoomStep); });
    connect(zoomIn, &QPushButton::clicked, canvas,
        [canvas] { canvas->zoomBy(kZoomStep); });
    connect(fit, &QPushButton::clicked, canvas,
        [canvas] { canvas->fitToWindow(); });
    connect(actual, &QPushButton::clicked, canvas,
        [canvas] { canvas->actualSize(); });

    auto *hint = new QLabel(
        tr("滚轮：以鼠标位置缩放　拖动图片：同步平移　分割模式：拖动中线比较左右图像"), this);
    hint->setWordWrap(true);
    hint->setProperty("role", QStringLiteral("muted"));
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
