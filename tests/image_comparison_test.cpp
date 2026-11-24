#include "calibration.h"
#include "image_comparison_dialog.h"
#include "image_quality.h"

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QWidget>
#include <QtGlobal>

#include <opencv2/geometry/3d.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <stdexcept>

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
        require(widget != nullptr, "缺少图片对比操作控件");
        return widget;
    }

    QImage render(QWidget &widget)
    {
        QImage image(widget.size(), QImage::Format_RGB32);
        image.fill(Qt::black);
        widget.render(&image);
        return image;
    }

    void requireColor(const QImage &image, const QPointF &position,
        const QColor &expected, const char *message)
    {
        const QPoint point = position.toPoint();
        require(image.rect().contains(point), "渲染检查点位于图片外");
        require(image.pixelColor(point) == expected, message);
    }

    void drag(QWidget &widget, const QPointF &from, const QPointF &to)
    {
        QMouseEvent press(QEvent::MouseButtonPress, from,
            QPointF(widget.mapToGlobal(from.toPoint())),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &press);
        QMouseEvent move(QEvent::MouseMove, to,
            QPointF(widget.mapToGlobal(to.toPoint())),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to,
            QPointF(widget.mapToGlobal(to.toPoint())),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &release);
    }

    double zoomPercent(QLabel &label)
    {
        QString text = label.text();
        text.remove(QLatin1Char('%'));
        return text.toDouble();
    }

    void saveArtifact(QWidget &dialog, const QString &name)
    {
        const QString path = qEnvironmentVariable("CALIBRATOR_TEST_ARTIFACT_DIR");
        if (path.isEmpty())
        {
            return;
        }
        QDir directory(path);
        require(directory.mkpath(QStringLiteral(".")), "无法创建界面截图目录");
        QApplication::processEvents();
        require(dialog.grab().save(directory.filePath(name)), "无法保存界面核验截图");
    }

    void testComparisonViews()
    {
        QImage original(200, 100, QImage::Format_RGB32);
        original.fill(Qt::red);
        {
            QPainter painter(&original);
            painter.fillRect(145, 0, 10, 100, Qt::green);
        }
        QImage undistorted(200, 100, QImage::Format_RGB32);
        undistorted.fill(Qt::blue);
        {
            QPainter painter(&undistorted);
            painter.fillRect(145, 0, 10, 100, Qt::yellow);
        }
        ImageComparisonDialog dialog(original, undistorted);
        dialog.resize(1000, 650);
        dialog.show();
        QApplication::processEvents();
        auto *canvas = child<QWidget>(dialog, "imageComparisonCanvas");
        auto *mode = child<QComboBox>(dialog, "imageComparisonModeCombo");
        auto *zoomIn = child<QPushButton>(dialog, "imageComparisonZoomInButton");
        auto *zoomOut = child<QPushButton>(dialog, "imageComparisonZoomOutButton");
        auto *fit = child<QPushButton>(dialog, "imageComparisonFitButton");
        auto *actual = child<QPushButton>(dialog, "imageComparisonActualSizeButton");
        auto *zoom = child<QLabel>(dialog, "imageComparisonZoomLabel");
        require(mode->currentData().toInt() == 0, "默认显示方式不是并排对比");
        saveArtifact(dialog, QStringLiteral("image-comparison-side.png"));
        actual->click();
        require(zoomPercent(*zoom) == 100.0, "100% 操作未恢复原始像素比例");

        // 对比两幅含不同颜色标记的图，检查真实渲染结果，而非只检查内部状态。
        const QPointF leftCenter(canvas->width() / 4.0, (canvas->height() + 28.0) / 2.0);
        const QPointF rightCenter(canvas->width() * 3.0 / 4.0, leftCenter.y());
        QImage image = render(*canvas);
        requireColor(image, leftCenter, Qt::red, "左窗格未显示原图");
        requireColor(image, rightCenter, Qt::blue, "右窗格未显示去畸变图");
        requireColor(image, leftCenter + QPointF(50, 0), Qt::green,
            "原图未按100%比例居中显示");
        requireColor(image, rightCenter + QPointF(50, 0), Qt::yellow,
            "去畸变图的坐标与原图不同步");

        zoomIn->click();
        require(zoomPercent(*zoom) == 125.0, "放大按钮未更新缩放比例");
        zoomOut->click();
        require(zoomPercent(*zoom) == 100.0, "缩小按钮未恢复缩放比例");

        drag(*canvas, leftCenter, leftCenter + QPointF(30, 0));
        image = render(*canvas);
        requireColor(image, leftCenter + QPointF(80, 0), Qt::green,
            "拖动未平移原图");
        requireColor(image, rightCenter + QPointF(80, 0), Qt::yellow,
            "拖动未同步平移去畸变图");
        actual->click();
        image = render(*canvas);
        requireColor(image, leftCenter + QPointF(50, 0), Qt::green,
            "100% 操作未复位平移位置");

        const QPointF anchor = leftCenter + QPointF(50, 0);
        QWheelEvent wheel(anchor, QPointF(canvas->mapToGlobal(anchor.toPoint())),
            QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
            Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &wheel);
        require(zoomPercent(*zoom) == 125.0, "滚轮未缩放图像");
        image = render(*canvas);
        requireColor(image, anchor, Qt::green, "滚轮缩放未保持鼠标处图像坐标");
        requireColor(image, leftCenter - QPointF(120, 0), Qt::red,
            "原图未按鼠标锚点扩大显示范围");
        requireColor(image, rightCenter - QPointF(120, 0), Qt::blue,
            "滚轮未同步缩放去畸变图");

        fit->click();
        const double fitBeforeResize = zoomPercent(*zoom);
        require(fitBeforeResize > 100.0, "适应窗口未按可用区域缩放图像");
        dialog.resize(1200, 750);
        QApplication::processEvents();
        require(zoomPercent(*zoom) > fitBeforeResize,
            "适应窗口模式未跟随窗口大小变化");

        mode->setCurrentIndex(mode->findData(1));
        saveArtifact(dialog, QStringLiteral("image-comparison-wipe.png"));
        actual->click();
        const QPointF wipeCenter(canvas->width() / 2.0, (canvas->height() + 28.0) / 2.0);
        image = render(*canvas);
        requireColor(image, wipeCenter - QPointF(40, 0), Qt::red,
            "分割模式左侧未显示原图");
        requireColor(image, wipeCenter + QPointF(40, 0), Qt::blue,
            "分割模式右侧未显示去畸变图");
        drag(*canvas, wipeCenter, wipeCenter + QPointF(70, 0));
        image = render(*canvas);
        requireColor(image, wipeCenter + QPointF(40, 0), Qt::red,
            "拖动分割线未改变原图显示区域");
        requireColor(image, wipeCenter + QPointF(90, 0), Qt::blue,
            "拖动分割线破坏了去畸变图显示区域");
    }

    void testInvalidImages()
    {
        const auto verifyDisabled = [](ImageComparisonDialog &dialog)
        {
            dialog.show();
            QApplication::processEvents();
            require(!child<QComboBox>(dialog, "imageComparisonModeCombo")->isEnabled(),
                "无效图片仍允许切换对比模式");
            require(!child<QPushButton>(dialog,
                        "imageComparisonZoomInButton")->isEnabled(),
                "无效图片仍允许缩放");
            require(child<QLabel>(dialog, "imageComparisonZoomLabel")->text()
                    == QStringLiteral("--"),
                "无效图片显示了误导性的缩放比例");
            require(!render(*child<QWidget>(dialog, "imageComparisonCanvas")).isNull(),
                "无效输入的错误提示无法渲染");
        };
        ImageComparisonDialog emptyDialog(QImage{}, QImage{});
        verifyDisabled(emptyDialog);
        QImage original(200, 100, QImage::Format_RGB32);
        original.fill(Qt::red);
        QImage differentSize(100, 100, QImage::Format_RGB32);
        differentSize.fill(Qt::blue);
        ImageComparisonDialog mismatchDialog(original, differentSize);
        verifyDisabled(mismatchDialog);
    }

    void testFisheyeSkew(const QTemporaryDir &directory)
    {
        const QString source = directory.filePath(QStringLiteral("fish.png"));
        const QString destination = directory.filePath(QStringLiteral("fish-corrected.png"));
        cv::Mat input(120, 160, CV_16UC4);
        for (int y = 0; y < input.rows; ++y)
        {
            for (int x = 0; x < input.cols; ++x)
            {
                input.at<cv::Vec4w>(y, x) = {0, static_cast<ushort>(y * 256),
                    static_cast<ushort>(x * 256), 48000};
            }
        }
        require(cv::imwrite(source.toStdString(), input), "无法创建鱼眼像素坐标图");

        CalibrationResult result;
        result.success = true;
        result.options.cameraModel = CameraModel::Fisheye;
        result.imageSize = {input.cols, input.rows};
        const cv::Matx33d intrinsic(75, 12, 80, 0, 75, 60, 0, 0, 1);
        const cv::Vec4d distortion(-0.03, 0.01, 0, 0);
        result.cameraMatrix = cv::Mat(intrinsic);
        result.distCoeffs = cv::Mat(distortion);
        const QPoint sample(123, 108);
        const cv::Vec3d ray = intrinsic.inv() * cv::Vec3d(sample.x(), sample.y(), 1);
        const std::vector<cv::Point3d> rays{{ray[0], ray[1], ray[2]}};
        std::vector<cv::Point2d> projected;
        cv::fisheye::projectPoints(rays, projected, cv::Vec3d(), cv::Vec3d(),
            intrinsic, distortion, intrinsic(0, 1) / intrinsic(0, 0));
        const cv::Point2d expected = projected.front();

        Calibrator calibrator;
        const auto comparison = calibrator.undistortionPreview(source, result);
        const auto preview = calibrator.previewImage(source, result.options, result, true);
        for (const QImage &image: {comparison.undistorted, preview})
        {
            require(!image.isNull(), "非零轴倾斜的鱼眼预览失败");
            const QColor color = image.pixelColor(sample);
            require(std::abs(color.red() - expected.x) < 1.0
                    && std::abs(color.green() - expected.y) < 1.0,
                "鱼眼预览的采样位置没有包含源相机的轴倾斜");
        }
        QString error;
        require(calibrator.undistortImageFile(source, destination, result, &error),
            "鱼眼批量去畸变失败");
        const cv::Mat output = cv::imread(destination.toStdString(), cv::IMREAD_UNCHANGED);
        require(output.type() == CV_16UC4 && output.size() == input.size(),
            "去畸变输出丢失原图位深、通道或分辨率");
        const auto pixel = output.at<cv::Vec4w>(sample.y(), sample.x());
        require(std::abs(pixel[2] / 256.0 - expected.x) < 0.1
                && std::abs(pixel[1] / 256.0 - expected.y) < 0.1 && pixel[3] == 48000,
            "鱼眼批量去畸变的采样位置或透明度错误");
    }

    void testExifOrientation(const QTemporaryDir &directory)
    {
        QImage original(80, 50, QImage::Format_RGB32);
        original.fill(Qt::red);
        {
            QPainter painter(&original);
            painter.fillRect(0, 0, 40, 25, Qt::green);
            painter.fillRect(40, 25, 40, 25, Qt::blue);
        }
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        require(buffer.open(QIODevice::WriteOnly) && original.save(&buffer, "JPEG"),
            "无法创建 EXIF 方向测试 JPEG");

        CalibrationResult result;
        result.success = true;
        result.imageSize = {50, 80};
        result.cameraMatrix = cv::Mat(cv::Matx33d(60, 0, 25, 0, 60, 40, 0, 0, 1));
        result.distCoeffs = cv::Mat::zeros(1, 5, CV_64F);
        Calibrator calibrator;
        for (const int orientation: {5, 6})
        {
            const QString path = directory.filePath(QStringLiteral("orientation-%1.jpg").arg(orientation));
            const QString destination = directory.filePath(QStringLiteral("orientation-%1.png").arg(orientation));
            QByteArray contents = jpeg;
            // APP1 EXIF 块只携带 Orientation，5 和 6 分别覆盖镜像转置与顺时针旋转。
            QByteArray exif = QByteArray::fromHex("ffe1002245786966000049492a000800000001001201030001000000");
            exif.append(static_cast<char>(orientation));
            exif.append(7, '\0');
            contents.insert(2, exif);
            QFile file(path);
            require(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(),
                "无法写入带 EXIF 方向的 JPEG");
            file.close();

            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QImage expected = reader.read().convertToFormat(QImage::Format_RGB32);
            require(expected.size() == result.imageSize, "测试 EXIF 方向块未生效");
            const auto comparison = calibrator.undistortionPreview(path, result);
            require(comparison.error.isEmpty() && comparison.original.size() == result.imageSize,
                "图片对比未使用 EXIF 旋转后的分辨率");
            const auto preview = calibrator.previewImage(path, result.options, result, true);
            require(preview.size() == result.imageSize, "普通预览未应用 EXIF 方向");
            const auto calibration = calibrator.calibrate({path}, result.options);
            require(calibration.imageSize == result.imageSize, "标定尺寸未应用 EXIF 方向");
            QString error;
            require(calibrator.undistortImageFile(path, destination, result, &error),
                "批量去畸变未使用 EXIF 旋转后的分辨率");
            require(QImage(destination).convertToFormat(QImage::Format_RGB32) == expected,
                "批量去畸变像素方向与 Qt 诊断图不一致");
        }

        if (QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("tiff")))
        {
            const QString path = directory.filePath(QStringLiteral("oriented.tiff"));
            const QString destination = directory.filePath(QStringLiteral("oriented.png"));
            QImageWriter writer(path);
            writer.setTransformation(QImageIOHandler::TransformationRotate90);
            require(writer.write(original), "无法创建带方向标记的 TIFF");
            QImageReader reader(path);
            reader.setAutoTransform(true);
            require(reader.transformation() == QImageIOHandler::TransformationRotate90,
                "测试 TIFF 方向标记未生效");
            const QImage expected = reader.read().convertToFormat(QImage::Format_RGB32);
            require(expected.size() == original.size().transposed() && analyzeImageQuality(path).readable,
                "带转置方向的非方形 TIFF 应能按旋转后的尺寸读取并通过质量预检");
            result.imageSize = expected.size();
            const auto comparison = calibrator.undistortionPreview(path, result);
            require(comparison.error.isEmpty()
                    && comparison.original.convertToFormat(QImage::Format_RGB32) == expected,
                "去畸变对比重复应用了 TIFF 方向");
            QString error;
            require(calibrator.undistortImageFile(path, destination, result, &error),
                "带方向标记的 TIFF 批量去畸变失败");
            require(QImage(destination).convertToFormat(QImage::Format_RGB32) == expected,
                "批量去畸变重复应用了 TIFF 方向");
        }
    }
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    try
    {
        testComparisonViews();
        testInvalidImages();
        QTemporaryDir directory;
        require(directory.isValid(), "无法创建图片对比测试目录");
        testFisheyeSkew(directory);
        testExifOrientation(directory);
        qInfo("图片对比行为测试通过");
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        qCritical() << "图片对比行为测试失败：" << error.what();
        return EXIT_FAILURE;
    }
}
