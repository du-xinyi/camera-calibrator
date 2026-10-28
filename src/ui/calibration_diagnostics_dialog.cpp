#include "calibration_diagnostics_dialog.h"
#include "theme.h"

#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QPushButton>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QSizePolicy>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace
{
    constexpr int kCoverageColumns = 12;
    constexpr int kCoverageRows = 8;
    constexpr int kCoverageCells = kCoverageColumns * kCoverageRows;

    bool finitePoint(const cv::Point2f &point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    bool insideImage(const cv::Point2f &point, const QSize &size)
    {
        return finitePoint(point) && point.x >= 0.0f && point.y >= 0.0f
                && point.x < size.width() && point.y < size.height();
    }

    bool pairedPoints(const CalibrationPose &pose)
    {
        return !pose.detectedPoints.empty()
                && pose.detectedPoints.size() == pose.projectedPoints.size();
    }

    QColor heatColor(double fraction)
    {
        return QColor::fromHsvF((1.0 - std::clamp(fraction, 0.0, 1.0)) * 0.66,
            0.78, 0.95);
    }

    struct CoverageData
    {
        std::array<quint64, kCoverageCells> counts{};
        std::array<quint64, 9> regions{};
        quint64 points = 0;
        quint64 skippedPoints = 0;
        int views = 0;
        int coveredCells = 0;
    };

    CoverageData coverageData(const CalibrationResult &result)
    {
        CoverageData data;
        if (!result.imageSize.isValid())
        {
            return data;
        }
        for (const CalibrationPose &pose: result.poses)
        {
            if (!pairedPoints(pose))
            {
                continue;
            }
            bool usedView = false;
            for (const cv::Point2f &point: pose.detectedPoints)
            {
                if (!insideImage(point, result.imageSize))
                {
                    ++data.skippedPoints;
                    continue;
                }
                const int column = std::min(kCoverageColumns - 1,
                    static_cast<int>(point.x / result.imageSize.width() * kCoverageColumns));
                const int row = std::min(kCoverageRows - 1,
                    static_cast<int>(point.y / result.imageSize.height() * kCoverageRows));
                ++data.counts[static_cast<size_t>(row * kCoverageColumns + column)];
                const int regionColumn = std::min(2,
                    static_cast<int>(point.x / result.imageSize.width() * 3));
                const int regionRow = std::min(2,
                    static_cast<int>(point.y / result.imageSize.height() * 3));
                ++data.regions[static_cast<size_t>(regionRow * 3 + regionColumn)];
                ++data.points;
                usedView = true;
            }
            if (usedView)
            {
                ++data.views;
            }
        }
        data.coveredCells = static_cast<int>(std::count_if(
            data.counts.begin(), data.counts.end(), [](quint64 count) { return count > 0; }));
        return data;
    }

    class CoverageCanvas final: public QWidget
    {
    public:

        CoverageCanvas(const QSize &imageSize, CoverageData data, QWidget *parent)
            : QWidget(parent), imageSize_(imageSize), data_(std::move(data))
        {
            setObjectName(QStringLiteral("coverageCanvas"));
            setMinimumSize(460, 300);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        }

    protected:

        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), Theme::kSurfaceAlt);
            if (!imageSize_.isValid() || data_.points == 0)
            {
                painter.drawText(rect(), Qt::AlignCenter,
                    tr("暂无角点覆盖数据，请重新标定。"));
                return;
            }
            const QRectF available = QRectF(rect()).adjusted(20.0, 12.0, -20.0, -64.0);
            QSizeF plotSize(imageSize_);
            plotSize.scale(available.size(), Qt::KeepAspectRatio);
            const QRectF plot(available.center() - QPointF(plotSize.width(), plotSize.height()) / 2.0,
                plotSize);
            const quint64 maximum = *std::max_element(data_.counts.begin(), data_.counts.end());
            const double cellWidth = plot.width() / kCoverageColumns;
            const double cellHeight = plot.height() / kCoverageRows;
            for (int row = 0; row < kCoverageRows; ++row)
            {
                for (int column = 0; column < kCoverageColumns; ++column)
                {
                    const quint64 count = data_.counts[static_cast<size_t>(row * kCoverageColumns + column)];
                    const QRectF cell(plot.left() + column * cellWidth,
                        plot.top() + row * cellHeight, cellWidth, cellHeight);
                    const double intensity = std::log1p(static_cast<double>(count))
                            / std::log1p(static_cast<double>(maximum));
                    painter.fillRect(cell, count == 0 ? QColor(222, 227, 233) : heatColor(intensity));
                    painter.setPen(QColor(255, 255, 255, 180));
                    painter.drawRect(cell);
                    if (cellWidth >= 32.0 && cellHeight >= 20.0)
                    {
                        painter.setPen(count == 0 ? QColor(105, 112, 122)
                            : intensity > 0.3 && intensity < 0.85 ? QColor(30, 35, 40) : Qt::white);
                        painter.drawText(cell, Qt::AlignCenter, QString::number(count));
                    }
                }
            }
            painter.setPen(QColor(80, 90, 105));
            painter.drawRect(plot);
            const QRectF legend(24.0, height() - 43.0, std::min(260.0, width() - 48.0), 12.0);
            QLinearGradient gradient(legend.topLeft(), legend.topRight());
            for (int step = 0; step <= 10; ++step)
            {
                gradient.setColorAt(step / 10.0, heatColor(step / 10.0));
            }
            painter.fillRect(legend, gradient);
            painter.drawText(QRectF(legend.left(), legend.bottom() + 3.0, legend.width(), 22.0),
                Qt::AlignLeft, tr("角点数量：少 → 多（最多 %1）").arg(maximum));
            painter.drawText(QRectF(legend.right() + 16.0, legend.top() - 4.0,
                width() - legend.right() - 28.0, 34.0), Qt::AlignVCenter | Qt::TextWordWrap,
                tr("灰色：无角点采样"));
        }

    private:

        QSize imageSize_;
        CoverageData data_;
    };

    class ResidualCanvas final: public QWidget
    {
    public:

        explicit ResidualCanvas(QWidget *parent): QWidget(parent)
        {
            setObjectName(QStringLiteral("residualCanvas"));
            setMinimumSize(460, 300);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            setCursor(Qt::OpenHandCursor);
        }

        void setData(QImage image, const CalibrationPose &pose)
        {
            image_ = std::move(image);
            detected_ = pose.detectedPoints;
            projected_ = pose.projectedPoints;
            message_.clear();
            fitView();
        }

        void showMessage(const QString &message)
        {
            image_ = {};
            detected_.clear();
            projected_.clear();
            message_ = message;
            update();
        }

        void setVectorScale(double scale)
        {
            vectorScale_ = scale;
            update();
        }

        void fitView()
        {
            fit_ = true;
            offset_ = {};
            update();
        }

        void pixelView()
        {
            fit_ = false;
            scale_ = 1.0;
            offset_ = {};
            update();
        }

    protected:

        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), Theme::kCanvasDark);
            if (image_.isNull())
            {
                painter.setPen(Qt::white);
                painter.drawText(rect().adjusted(20, 20, -20, -20),
                    Qt::AlignCenter | Qt::TextWordWrap, message_);
                return;
            }
            const double scale = currentScale();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.translate(QPointF(width() / 2.0, height() / 2.0) + offset_);
            painter.scale(scale, scale);
            painter.translate(-image_.width() / 2.0, -image_.height() / 2.0);
            painter.setClipRect(QRectF(image_.rect()));
            painter.drawImage(QPointF(0.0, 0.0), image_);
            const double radius = 3.0 / scale;
            for (size_t index = 0; index < detected_.size(); ++index)
            {
                const QPointF detected(detected_[index].x, detected_[index].y);
                const QPointF projected(projected_[index].x, projected_[index].y);
                const QPointF displacement = projected - detected;
                const QPointF end = detected + displacement * vectorScale_;
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(QColor(255, 213, 65), 1.25 / scale, Qt::DashLine));
                painter.drawLine(detected, end);
                const double length = std::hypot(displacement.x(), displacement.y());
                if (length > 0.0)
                {
                    const QPointF direction = displacement / length;
                    const QPointF normal(-direction.y(), direction.x());
                    const double head = 6.0 / scale;
                    painter.setBrush(QColor(255, 213, 65));
                    painter.drawPolygon(QPolygonF{end, end - direction * head + normal * head * 0.45,
                        end - direction * head - normal * head * 0.45});
                }
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(QColor(55, 235, 120), 1.5 / scale));
                painter.drawEllipse(detected, radius, radius);
                painter.setPen(QPen(QColor(255, 85, 220), 1.5 / scale));
                painter.drawLine(projected + QPointF(-radius, -radius), projected + QPointF(radius, radius));
                painter.drawLine(projected + QPointF(-radius, radius), projected + QPointF(radius, -radius));
            }
        }

        void wheelEvent(QWheelEvent *event) override
        {
            if (image_.isNull() || event->angleDelta().y() == 0)
            {
                event->ignore();
                return;
            }
            const double previous = currentScale();
            const double next = std::clamp(previous * std::pow(1.25, event->angleDelta().y() / 120.0),
                0.005, 64.0);
            const QPointF center(width() / 2.0, height() / 2.0);
            const QPointF local = event->position() - center - offset_;
            offset_ += local * (1.0 - next / previous);
            fit_ = false;
            scale_ = next;
            update();
            event->accept();
        }

        void mousePressEvent(QMouseEvent *event) override
        {
            if (event->button() == Qt::LeftButton && !image_.isNull())
            {
                dragging_ = true;
                previousPosition_ = event->position();
                setCursor(Qt::ClosedHandCursor);
                event->accept();
            }
        }

        void mouseMoveEvent(QMouseEvent *event) override
        {
            if (dragging_)
            {
                offset_ += event->position() - previousPosition_;
                previousPosition_ = event->position();
                update();
                event->accept();
            }
        }

        void mouseReleaseEvent(QMouseEvent *event) override
        {
            if (event->button() == Qt::LeftButton)
            {
                dragging_ = false;
                setCursor(Qt::OpenHandCursor);
                event->accept();
            }
        }

    private:

        double currentScale() const
        {
            if (!fit_ || image_.isNull())
            {
                return scale_;
            }
            return std::max(0.005, std::min(
                (width() - 24.0) / image_.width(), (height() - 24.0) / image_.height()));
        }

        QImage image_;
        std::vector<cv::Point2f> detected_;
        std::vector<cv::Point2f> projected_;
        QString message_;
        QPointF offset_;
        QPointF previousPosition_;
        double scale_ = 1.0;
        double vectorScale_ = 10.0;
        bool fit_ = true;
        bool dragging_ = false;
    };
}

CalibrationDiagnosticsDialog::CalibrationDiagnosticsDialog(const CalibrationResult &result,
    const QString &selectedImagePath, QWidget *parent)
    : QDialog(parent), result_(result)
{
    setObjectName(QStringLiteral("calibrationDiagnosticsDialog"));
    setWindowTitle(tr("标定质量诊断"));
    resize(1060, 760);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(10);
    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->setObjectName(QStringLiteral("diagnosticsTabs"));
    layout->addWidget(tabs);

    auto *coveragePage = new QWidget(tabs);
    auto *coverageLayout = new QVBoxLayout(coveragePage);
    coverageLayout->setContentsMargins(0, 12, 0, 0);
    coverageLayout->setSpacing(10);
    const CoverageData data = coverageData(result_);
    auto *summary = new QLabel(tr("%1 张有效视图 · %2 个角点 · 网格覆盖 %3 / %4（%5%）")
        .arg(data.views).arg(data.points).arg(data.coveredCells).arg(kCoverageCells)
        .arg(data.coveredCells * 100.0 / kCoverageCells, 0, 'f', 1), coveragePage);
    summary->setObjectName(QStringLiteral("coverageSummaryLabel"));
    summary->setWordWrap(true);
    summary->setProperty("role", QStringLiteral("panelTitle"));
    coverageLayout->addWidget(summary);
    coverageLayout->addWidget(new CoverageCanvas(result_.imageSize, data, coveragePage), 1);
    const QStringList regionNames{tr("左上角"), tr("上边缘"), tr("右上角"),
        tr("左边缘"), tr("中心"), tr("右边缘"), tr("左下角"), tr("下边缘"), tr("右下角")};
    QStringList missingRegions;
    for (size_t index = 0; index < data.regions.size(); ++index)
    {
        if (data.regions[index] == 0)
        {
            missingRegions << regionNames[static_cast<qsizetype>(index)];
        }
    }
    QString hint = data.points == 0 ? tr("没有可用角点，请重新标定以生成诊断数据。")
        : missingRegions.isEmpty() ? tr("九个画面区域均已有角点采样，可继续结合姿态分布与误差检查。")
        : tr("尚无角点采样的区域：%1。可补拍覆盖这些区域的图片。")
            .arg(missingRegions.join(tr("、")));
    if (data.skippedPoints > 0)
    {
        hint += tr(" 已忽略 %1 个越界或无效角点。").arg(data.skippedPoints);
    }
    hint += tr("\n覆盖率按 %1×%2 网格的角点占用统计，仅为采样分布提示，不代表标定质量合格。")
        .arg(kCoverageColumns).arg(kCoverageRows);
    auto *coverageHint = new QLabel(hint, coveragePage);
    coverageHint->setObjectName(QStringLiteral("coverageHintLabel"));
    coverageHint->setWordWrap(true);
    coverageHint->setProperty("role", QStringLiteral("hint"));
    coverageLayout->addWidget(coverageHint);
    tabs->addTab(coveragePage, tr("角点覆盖热力图"));

    auto *residualPage = new QWidget(tabs);
    auto *residualLayout = new QVBoxLayout(residualPage);
    residualLayout->setContentsMargins(0, 12, 0, 0);
    residualLayout->setSpacing(10);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(6);
    controls->addWidget(new QLabel(tr("图片："), residualPage));
    auto *images = new QComboBox(residualPage);
    images->setObjectName(QStringLiteral("residualImageCombo"));
    images->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    images->setMinimumContentsLength(18);
    int initialIndex = 0;
    for (size_t index = 0; index < result_.poses.size(); ++index)
    {
        const CalibrationPose &pose = result_.poses[index];
        images->addItem(QFileInfo(pose.imagePath).fileName(), static_cast<int>(index));
        images->setItemData(static_cast<int>(index), pose.imagePath, Qt::ToolTipRole);
        if (pose.imagePath == selectedImagePath)
        {
            initialIndex = static_cast<int>(index);
        }
    }
    controls->addWidget(images, 1);
    controls->addWidget(new QLabel(tr("误差箭头放大："), residualPage));
    auto *vectorScale = new QDoubleSpinBox(residualPage);
    vectorScale->setObjectName(QStringLiteral("residualVectorScaleSpin"));
    vectorScale->setRange(1.0, 100.0);
    vectorScale->setDecimals(1);
    vectorScale->setSuffix(tr(" 倍"));
    vectorScale->setValue(10.0);
    controls->addWidget(vectorScale);
    auto *fitButton = new QPushButton(tr("适应窗口"), residualPage);
    fitButton->setObjectName(QStringLiteral("residualFitButton"));
    controls->addWidget(fitButton);
    auto *pixelButton = new QPushButton(tr("100%"), residualPage);
    pixelButton->setObjectName(QStringLiteral("residualActualSizeButton"));
    controls->addWidget(pixelButton);
    residualLayout->addLayout(controls);
    auto *metrics = new QLabel(residualPage);
    metrics->setObjectName(QStringLiteral("residualMetricsLabel"));
    metrics->setWordWrap(true);
    residualLayout->addWidget(metrics);
    auto *canvas = new ResidualCanvas(residualPage);
    residualLayout->addWidget(canvas, 1);
    auto *legend = new QLabel(tr("绿色圆点：检测角点；洋红十字：实际重投影点；黄色虚线箭头：放大后的误差向量。"
        "\n放大倍数只影响箭头，不改变实际点位置或误差数值。滚轮缩放，按住左键拖动平移。"), residualPage);
    legend->setWordWrap(true);
    legend->setProperty("role", QStringLiteral("hint"));
    residualLayout->addWidget(legend);
    tabs->addTab(residualPage, tr("重投影误差叠加"));

    const auto displayResidual = [this, images, metrics, canvas, vectorScale]
    {
        const int index = images->currentIndex();
        if (index < 0 || static_cast<size_t>(index) >= result_.poses.size())
        {
            metrics->setText(tr("没有可用视图"));
            canvas->showMessage(tr("暂无重投影数据，请重新标定。"));
            vectorScale->setEnabled(false);
            return;
        }
        const CalibrationPose &pose = result_.poses[static_cast<size_t>(index)];
        const bool validPoints = result_.imageSize.isValid() && pairedPoints(pose)
                && std::all_of(pose.detectedPoints.begin(), pose.detectedPoints.end(),
                    [this](const cv::Point2f &point) { return insideImage(point, result_.imageSize); })
                && std::all_of(pose.projectedPoints.begin(), pose.projectedPoints.end(), finitePoint);
        if (!validPoints)
        {
            metrics->setText(tr("这张图片缺少完整的逐点诊断数据"));
            canvas->showMessage(tr("角点与重投影点缺失、数量不匹配或坐标无效，请重新标定。"));
            vectorScale->setEnabled(false);
            return;
        }
        QImageReader reader(pose.imagePath);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (image.isNull() || image.size() != result_.imageSize)
        {
            metrics->setText(image.isNull() ? tr("原图无法读取：%1").arg(reader.errorString())
                : tr("原图分辨率与标定结果不一致"));
            canvas->showMessage(tr("无法将诊断点叠加到当前原图，请检查图片路径与分辨率。\n%1")
                .arg(pose.imagePath));
            vectorScale->setEnabled(false);
            return;
        }
        double squaredError = 0.0;
        double maximumError = 0.0;
        for (size_t pointIndex = 0; pointIndex < pose.detectedPoints.size(); ++pointIndex)
        {
            const double dx = static_cast<double>(pose.projectedPoints[pointIndex].x)
                    - pose.detectedPoints[pointIndex].x;
            const double dy = static_cast<double>(pose.projectedPoints[pointIndex].y)
                    - pose.detectedPoints[pointIndex].y;
            const double error = std::hypot(dx, dy);
            squaredError += error * error;
            maximumError = std::max(maximumError, error);
        }
        const double rms = std::sqrt(squaredError / pose.detectedPoints.size());
        metrics->setText(tr("%1 个角点 · 实际 RMS：%2 px · 最大误差：%3 px")
            .arg(pose.detectedPoints.size()).arg(rms, 0, 'f', 3).arg(maximumError, 0, 'f', 3));
        vectorScale->setEnabled(true);
        canvas->setData(std::move(image), pose);
        canvas->setVectorScale(vectorScale->value());
    };
    connect(images, &QComboBox::currentIndexChanged, this,
        [displayResidual] { displayResidual(); });
    connect(vectorScale, &QDoubleSpinBox::valueChanged, this,
        [canvas](double value) { canvas->setVectorScale(value); });
    connect(fitButton, &QPushButton::clicked, this, [canvas] { canvas->fitView(); });
    connect(pixelButton, &QPushButton::clicked, this, [canvas] { canvas->pixelView(); });
    images->setCurrentIndex(initialIndex);
    displayResidual();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
