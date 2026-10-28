#include "calibration_diagnostics_dialog.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QWidget>
#include <QtGlobal>

#include <exception>
#include <stdexcept>
#include <string>

namespace
{
    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template<typename Widget>
    Widget *child(QObject &parent, const char *name)
    {
        auto *widget = parent.findChild<Widget *>(QString::fromLatin1(name));
        if (widget == nullptr)
        {
            throw std::runtime_error(std::string("诊断控件缺失：") + name);
        }
        return widget;
    }

    QImage renderWidget(QWidget *widget)
    {
        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);
        return image;
    }

    void testCoverageAndResiduals()
    {
        QTemporaryDir directory;
        require(directory.isValid(), "无法创建诊断测试目录");
        QImage original(1200, 800, QImage::Format_RGB32);
        original.fill(QColor(25, 25, 25));
        const QString path = directory.filePath("view.png");
        require(original.save(path), "无法保存诊断测试原图");

        CalibrationResult result;
        result.success = true;
        result.imageSize = original.size();
        CalibrationPose pose;
        pose.imagePath = path;
        pose.detectedPoints = {{10.0f, 10.0f}, {1190.0f, 790.0f}, {610.0f, 410.0f}, {610.0f, 410.0f}};
        for (const cv::Point2f &point: pose.detectedPoints)
        {
            pose.projectedPoints.push_back(point + cv::Point2f(3.0f, 4.0f));
        }
        result.poses.push_back(pose);
        pose.imagePath = directory.filePath("missing.png");
        result.poses.push_back(pose);
        pose.imagePath = directory.filePath("incomplete.png");
        pose.projectedPoints.pop_back();
        result.poses.push_back(pose);

        CalibrationDiagnosticsDialog dialog(result, path);
        dialog.show();
        QApplication::processEvents();
        const QString summary = child<QLabel>(dialog, "coverageSummaryLabel")->text();
        require(summary.contains(QStringLiteral("2 张有效视图"))
                && summary.contains(QStringLiteral("8 个角点"))
                && summary.contains(QStringLiteral("3 / 96")),
            "覆盖统计未按真实网格占用计算，或错误计入不成对数据");
        const QString coverageHint = child<QLabel>(dialog, "coverageHintLabel")->text();
        require(coverageHint.contains(QStringLiteral("右上角"))
                && coverageHint.contains(QStringLiteral("左下角"))
                && coverageHint.contains(QStringLiteral("不代表标定质量合格")),
            "覆盖缺口或采样指标说明缺失");

        const QString artifactDirectory = qEnvironmentVariable("CALIBRATOR_TEST_ARTIFACT_DIR");
        if (!artifactDirectory.isEmpty())
        {
            require(QDir().mkpath(artifactDirectory), "无法创建界面核验图片目录");
            require(dialog.grab().save(QDir(artifactDirectory).filePath("diagnostics-coverage.png")),
                "无法保存覆盖热力图截图");
        }

        auto *tabs = child<QTabWidget>(dialog, "diagnosticsTabs");
        tabs->setCurrentIndex(1);
        QApplication::processEvents();
        if (!artifactDirectory.isEmpty())
        {
            require(dialog.grab().save(QDir(artifactDirectory).filePath("diagnostics-residual.png")),
                "无法保存误差叠加截图");
        }
        auto *images = child<QComboBox>(dialog, "residualImageCombo");
        auto *metrics = child<QLabel>(dialog, "residualMetricsLabel");
        auto *scale = child<QDoubleSpinBox>(dialog, "residualVectorScaleSpin");
        auto *canvas = child<QWidget>(dialog, "residualCanvas");
        require(images->currentIndex() == 0 && scale->isEnabled(), "未选择指定的有效图片");
        const QString actualMetrics = metrics->text();
        require(actualMetrics.contains(QStringLiteral("实际 RMS：5.000 px"))
                && actualMetrics.contains(QStringLiteral("最大误差：5.000 px")),
            "逐点偏差应显示真实的5像素误差");
        const QImage amplified = renderWidget(canvas);
        scale->setValue(1.0);
        require(metrics->text() == actualMetrics, "误差向量缩放不应改变真实RMS");
        require(renderWidget(canvas) != amplified, "误差箭头放大控制未改变叠加图");

        images->setCurrentIndex(1);
        require(!scale->isEnabled() && metrics->text().contains(QStringLiteral("无法读取")),
            "缺失原图时不应继续显示有效叠加");
        images->setCurrentIndex(2);
        require(!scale->isEnabled() && metrics->text().contains(QStringLiteral("缺少完整")),
            "诊断点不成对时应明确拒绝误差叠加");
        images->setCurrentIndex(0);
        require(scale->isEnabled() && metrics->text() == actualMetrics,
            "切回有效图片后未恢复诊断显示");

        original = QImage(600, 400, QImage::Format_RGB32);
        original.fill(Qt::black);
        require(original.save(path), "无法改变测试图片分辨率");
        images->setCurrentIndex(1);
        images->setCurrentIndex(0);
        require(!scale->isEnabled() && metrics->text().contains(QStringLiteral("分辨率")),
            "原图尺寸变化时不应绘制旧角点");
    }

    void testEmptyData()
    {
        CalibrationDiagnosticsDialog dialog(CalibrationResult{});
        require(child<QLabel>(dialog, "coverageHintLabel")->text()
                .contains(QStringLiteral("没有可用角点")), "空结果缺少覆盖提示");
        require(!child<QDoubleSpinBox>(dialog, "residualVectorScaleSpin")->isEnabled(),
            "空结果不应允许调整误差向量");
    }
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    try
    {
        testCoverageAndResiduals();
        testEmptyData();
    }
    catch (const std::exception &exception)
    {
        qCritical() << "诊断界面测试失败：" << exception.what();
        return 1;
    }
    qInfo("诊断界面测试通过");
    return 0;
}
