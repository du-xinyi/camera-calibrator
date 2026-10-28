#include "image_comparison_dialog.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QWheelEvent>
#include <QWidget>
#include <QtGlobal>

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
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    try
    {
        testComparisonViews();
        testInvalidImages();
        qInfo("图片对比行为测试通过");
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        qCritical() << "图片对比行为测试失败：" << error.what();
        return EXIT_FAILURE;
    }
}
