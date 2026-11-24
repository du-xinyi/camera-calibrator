#include "algorithm_comparison_dialog.h"
#include "calibration_project.h"
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <opencv2/core.hpp>

#include <cmath>
#include <cstdlib>
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
            throw std::runtime_error(std::string("测试所需控件不存在：") + name);
        }
        return widget;
    }

    CalibrationResult successfulResult(double rms, double focalLength)
    {
        CalibrationResult result;
        result.success = true;
        result.rmsError = rms;
        result.imagesUsed = 3;
        result.imagesTotal = 3;
        result.imageSize = {320, 240};
        result.options.boardType = CalibrationBoardType::Chessboard;
        result.options.boardSize = {5, 4};
        result.cameraMatrix = cv::Mat(cv::Matx33d(
            focalLength, 0.0, 160.0, 0.0, focalLength, 120.0, 0.0, 0.0, 1.0));
        result.distCoeffs = cv::Mat::zeros(1, 5, CV_64F);
        result.report = QStringLiteral("界面功能测试结果");
        return result;
    }

    void testComparisonApplication()
    {
        CalibrationResult failure;
        failure.report = QStringLiteral("候选求解失败");
        AlgorithmComparisonDialog dialog({
            successfulResult(0.8, 500.0), failure, successfulResult(0.2, 600.0)
        });
        auto *table = child<QTableWidget>(dialog, "algorithmComparisonTable");
        auto *apply = child<QPushButton>(dialog, "applyComparisonResultButton");
        require(table->rowCount() == 3, "对比窗口丢失候选结果");
        require(table->columnCount() == 9
                && table->horizontalHeaderItem(3)->text() == QStringLiteral("RMS (px)"),
            "算法对比表头与列数不正确");
        require(table->currentRow() == 0 && apply->isEnabled(),
            "首个成功候选未默认选中");
        require(std::abs(table->item(0, 3)->text().toDouble() - 0.2) < 1e-8,
            "成功候选未按 RMS 排序");

        table->selectRow(2);
        require(!apply->isEnabled(), "失败候选不应允许应用");
        table->clearSelection();
        require(!apply->isEnabled(), "无选中候选时不应允许应用");
        table->selectRow(0);
        table->clearSelection();
        require(!apply->isEnabled(), "取消成功行选择后仍允许应用");

        int emissionCount = 0;
        double selectedRms = -1.0;
        double selectedFocalLength = -1.0;
        QObject::connect(&dialog, &AlgorithmComparisonDialog::resultSelected,
            &dialog, [&](const CalibrationResult &result)
            {
                ++emissionCount;
                selectedRms = result.rmsError;
                selectedFocalLength = result.cameraMatrix.at<double>(0, 0);
            });
        table->selectRow(0);
        require(apply->isEnabled(), "重新选择成功候选后按钮未恢复");
        apply->click();
        require(emissionCount == 1 && std::abs(selectedRms - 0.2) < 1e-8
                && std::abs(selectedFocalLength - 600.0) < 1e-8,
            "应用信号未携带排序后选中行对应的完整结果");
        require(dialog.result() == QDialog::Accepted, "应用后未确认关闭窗口");

        AlgorithmComparisonDialog emptyDialog({});
        require(!child<QPushButton>(emptyDialog,
                    "applyComparisonResultButton")->isEnabled(),
            "空候选列表不应允许应用");
    }

    void testDropImagesAndFolders()
    {
        QTemporaryDir directory;
        require(directory.isValid() && QDir(directory.path()).mkpath("folder/nested"),
            "无法建立拖放测试目录");
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::gray);
        for (const QString &name: QStringList{"single.png", "folder/b.png",
                 "folder/a.PNG", "folder/nested/ignored.png"})
        {
            require(image.save(directory.filePath(name)), "无法保存拖放测试图片");
        }
        QFile other(directory.filePath("notes.txt"));
        require(other.open(QIODevice::WriteOnly), "无法建立非图片文件");
        other.close();

        MainWindow window;
        window.show();
        auto *preview = child<QWidget>(window, "imageDisplay");
        const QUrl textFile = QUrl::fromLocalFile(other.fileName());
        const QUrl remote(QStringLiteral("https://example.invalid/photo.png"));
        QMimeData data;
        data.setUrls({textFile, remote});
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        const QPointF position(5, 5);
#else
        const QPoint position(5, 5);
#endif
        QDragEnterEvent unsupported(position, Qt::CopyAction, &data,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(preview, &unsupported);
        require(!unsupported.isAccepted(), "不支持的拖放内容不应被接受");

        data.setUrls({QUrl::fromLocalFile(directory.filePath("single.png")),
            QUrl::fromLocalFile(directory.filePath("folder")),
            QUrl::fromLocalFile(directory.filePath("folder/b.png")), textFile, remote});
        QDragEnterEvent enter(position, Qt::CopyAction, &data,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(preview, &enter);
        require(enter.isAccepted(), "预览区未接受图片和文件夹混合拖入");
        QDropEvent drop(position, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(preview, &drop);
        auto *list = child<QListWidget>(window, "imageList");
        require(drop.isAccepted() && list->count() == 3
                && list->item(0)->text().startsWith("single.png")
                && list->item(1)->text().startsWith("a.PNG")
                && list->item(2)->text().startsWith("b.png"),
            "拖入图片与文件夹后的列表内容、去重或排序错误");
    }

    void testImageParticipationAndFilters()
    {
        QTemporaryDir directory;
        require(directory.isValid(), "无法建立临时图片目录");
        QStringList paths;
        for (int index = 0; index < 3; ++index)
        {
            QImage image(320, 240, QImage::Format_RGB32);
            // 均匀图像保证质量预检提示模糊，同时不存在可检测的标定板。
            image.fill(QColor(80 + index * 40, 80 + index * 40, 80 + index * 40));
            const QString path = directory.filePath(
                QStringLiteral("image_%1.png").arg(index));
            require(image.save(path), "无法保存临时 PNG 图片");
            paths << path;
        }

        MainWindow window;
        window.loadImages(paths);
        auto *list = child<QListWidget>(window, "imageList");
        auto *filter = child<QComboBox>(window, "imageFilterCombo");
        auto *calibrate = child<QAction>(window, "calibrateAction");
        auto *compare = child<QAction>(window, "compareAlgorithmsAction");
        auto *exportParameters = child<QAction>(window, "exportAction");
        auto *undistort = child<QCheckBox>(window, "showUndistortedCheck");
        require(list->count() == 3, "图片载入数量不正确");
        for (int row = 0; row < list->count(); ++row)
        {
            require(list->item(row)->flags().testFlag(Qt::ItemIsUserCheckable)
                    && list->item(row)->checkState() == Qt::Checked,
                "载入图片未默认启用或不支持勾选");
        }
        require(calibrate->isEnabled() && compare->isEnabled(),
            "三张启用图片未允许标定和算法对比");
        list->item(1)->setCheckState(Qt::Unchecked);
        require(!calibrate->isEnabled() && !compare->isEnabled(),
            "启用图片少于三张时仍允许标定或算法对比");

        const auto setFilter = [filter](int value)
        {
            const int index = filter->findData(value);
            require(index >= 0, "图片筛选项缺失");
            filter->setCurrentIndex(index);
        };
        setFilter(2);
        require(list->item(0)->isHidden() && !list->item(1)->isHidden()
                && list->item(2)->isHidden(),
            "停用图片筛选显示错误");
        setFilter(1);
        require(!list->item(0)->isHidden() && list->item(1)->isHidden()
                && !list->item(2)->isHidden(),
            "启用图片筛选显示错误");
        setFilter(3);
        for (int row = 0; row < list->count(); ++row)
        {
            require(!list->item(row)->isHidden(), "质量提示图片未正确显示");
        }
        setFilter(0);
        for (int row = 0; row < list->count(); ++row)
        {
            require(!list->item(row)->isHidden(), "全部图片筛选仍隐藏图片");
            list->setCurrentRow(row);
            require(QMetaObject::invokeMethod(&window,
                        "onImageSelectionChanged", Qt::DirectConnection),
                "无法刷新图片检测状态");
        }
        setFilter(4);
        for (int row = 0; row < list->count(); ++row)
        {
            require(!list->item(row)->isHidden(), "检测失败图片未正确显示");
        }
        require(list->count() == 3
                && list->item(0)->checkState() == Qt::Checked
                && list->item(1)->checkState() == Qt::Unchecked
                && list->item(2)->checkState() == Qt::Checked,
            "筛选操作改变了图片列表或启用状态");
        require(!calibrate->isEnabled() && !compare->isEnabled(),
            "筛选操作改变了标定可用状态");

        setFilter(0);
        list->item(1)->setCheckState(Qt::Checked);
        require(calibrate->isEnabled() && compare->isEnabled(),
            "恢复图片参与后未重新允许标定和算法对比");
        require(!exportParameters->isEnabled() && !undistort->isEnabled(),
            "无标定结果时不应允许导出和去畸变");

        int resultMessages = 0;
        QTimer messageCloser;
        QObject::connect(&messageCloser, &QTimer::timeout, &window,
            [&resultMessages]
            {
                for (QWidget *widget: QApplication::topLevelWidgets())
                {
                    if (auto *message = qobject_cast<QMessageBox *>(widget);
                        message != nullptr && message->isVisible())
                    {
                        ++resultMessages;
                        message->accept();
                    }
                }
            });
        messageCloser.start(10);
        const CalibrationResult result = successfulResult(0.25, 550.0);
        require(QMetaObject::invokeMethod(&window,
                    "onApplyComparisonResult", Qt::DirectConnection,
                    Q_ARG(CalibrationResult, result)),
            "无法应用算法对比结果");
        messageCloser.stop();
        require(resultMessages == 1, "应用结果未显示标定结果通知");
        require(exportParameters->isEnabled() && undistort->isEnabled(),
            "应用结果后未允许导出参数和去畸变");
        require(!child<QAction>(window, "calibrationDiagnosticsAction")->isEnabled(),
            "没有逐角点数据的结果不应启用诊断视图");
        require(child<QAction>(window, "undistortionComparisonAction")->isEnabled(),
            "有效参数和选中图片应允许去畸变对比");
        auto *boardColumns = child<QSpinBox>(window, "boardColsSpin");
        boardColumns->setValue(boardColumns->value() + 1);
        require(!exportParameters->isEnabled() && !undistort->isEnabled(),
            "修改标定参数后旧结果未失效");
        require(!child<QAction>(window, "undistortionComparisonAction")->isEnabled(),
            "修改参数后旧结果不应继续用于对比预览");

        auto *boardRows = child<QSpinBox>(window, "boardRowsSpin");
        auto *boardType = child<QComboBox>(window, "boardTypeCombo");
        require(boardColumns->minimum() == 4 && boardRows->minimum() == 4,
            "普通棋盘应至少有 4×4 个方格");
        boardType->setCurrentIndex(boardType->findData(static_cast<int>(CalibrationBoardType::Charuco)));
        require(boardColumns->minimum() == 2 && boardRows->minimum() == 2,
            "ChArUco 应保留 2×2 方格选项");
    }

    void testImportParameters()
    {
        QTemporaryDir directory;
        require(directory.isValid(), "无法建立参数导入测试目录");
        const QString path = directory.filePath("parameters.yaml");
        QFile parameters(path);
        const QByteArray contents =
            "camera_model: pinhole\n"
            "image_width: 320\n"
            "image_height: 240\n"
            "camera_matrix: [[500, 0, 160], [0, 500, 120], [0, 0, 1]]\n"
            "distortion_coefficients: [1, 0, 0, 0, 0]\n"
            "poses:\n"
            "  - image: board.png\n"
            "    rotation_vector: [0.1, 0.2, 0.3]\n"
            "    translation_vector: [10, 20, 500]\n"
            "    reprojection_error: 0.75\n";
        require(parameters.open(QIODevice::WriteOnly)
                && parameters.write(contents) == contents.size(), "无法写入参数测试文件");
        parameters.close();

        QImage image(240, 320, QImage::Format_RGB32);
        image.fill(Qt::white);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "JPEG"),
            "无法编码 JPEG 测试图片");
        buffer.close();
        // EXIF 方向 6 将保存的 240×320 图像顺时针旋转为 320×240。
        jpeg.insert(2, QByteArray::fromHex(
            "ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        QFile rotated(directory.filePath("rotated.jpg"));
        require(rotated.open(QIODevice::WriteOnly) && rotated.write(jpeg) == jpeg.size(),
            "无法写入带 EXIF 方向的 JPEG");
        rotated.close();

        MainWindow window;
        window.loadImages({rotated.fileName()});
        auto *list = child<QListWidget>(window, "imageList");
        list->setCurrentRow(0);
        bool resolutionWarning = false;
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, &window, [&]
        {
            for (QWidget *widget: QApplication::topLevelWidgets())
            {
                if (auto *file = qobject_cast<QFileDialog *>(widget); file && file->isVisible())
                {
                    file->setDirectory(directory.path());
                    child<QLineEdit>(*file, "fileNameEdit")->setText(path);
                    QMetaObject::invokeMethod(file, "accept", Qt::QueuedConnection);
                }
                else if (auto *message = qobject_cast<QMessageBox *>(widget);
                    message && message->isVisible())
                {
                    if (message->objectName() == QStringLiteral("unsavedChangesDialog"))
                    {
                        message->button(QMessageBox::Discard)->click();
                    }
                    else
                    {
                        resolutionWarning |= message->windowTitle() == QStringLiteral("分辨率不一致");
                        message->accept();
                    }
                }
            }
        });
        driver.start(10);
        child<QAction>(window, "importParametersAction")->trigger();
        driver.stop();

        require(!resolutionWarning, "带方向标记的 JPEG 已匹配标定分辨率，不应提示冲突");
        auto *preview = child<QLabel>(window, "imageDisplay");
        const QImage original = preview->pixmap().toImage();
        child<QCheckBox>(window, "showUndistortedCheck")->setChecked(true);
        require(QMetaObject::invokeMethod(&window, "onImageSelectionChanged", Qt::DirectConnection)
            && !original.isNull() && !preview->pixmap().isNull()
            && preview->pixmap().toImage() != original,
            "去畸变预览应使用 EXIF 旋转后的尺寸");

        require(child<QLabel>(window, "resultRmsValue")->text() == QStringLiteral("未提供"),
            "导入文件缺少 RMS 时应显示未提供");
        auto *poseAction = child<QAction>(window, "poseResultAction");
        require(poseAction->isEnabled()
                && !child<QAction>(window, "calibrationDiagnosticsAction")->isEnabled(),
            "导入位姿应可查看且无需逐角点诊断数据");
        poseAction->trigger();
        auto *table = child<QTableWidget>(window, "poseTable");
        require(table->rowCount() == 1 && table->item(0, 1)->text() == QStringLiteral("board.png"),
            "位姿窗口未显示导入的图片位姿");
    }

    void testSessionRestoration()
    {
        QTemporaryDir directory;
        require(directory.isValid(), "无法建立会话测试目录");
        QStringList paths;
        for (int index = 0; index < 4; ++index)
        {
            QImage image(320, 240, QImage::Format_RGB32);
            image.fill(QColor(100 + index * 10, 100, 100));
            const QString path = directory.filePath(QStringLiteral("view_%1.png").arg(index));
            require(image.save(path), "无法保存会话测试图片");
            paths << path;
        }
        MainWindow window;
        window.loadImages(paths);
        auto *list = child<QListWidget>(window, "imageList");
        list->item(3)->setCheckState(Qt::Unchecked);
        CalibrationResult result = successfulResult(0.35, 575.0);
        for (int index = 0; index < 3; ++index)
        {
            CalibrationPose pose;
            pose.imagePath = paths[index];
            pose.rotationVector = {0.1, 0.2, 0.3};
            pose.translationVector = {10.0, 20.0, 500.0 + index};
            pose.reprojectionError = 0.35;
            pose.detectedPoints = {{40.0f, 30.0f}, {280.0f, 30.0f},
                {280.0f, 210.0f}, {40.0f, 210.0f}};
            pose.projectedPoints = {{40.35f, 30.0f}, {280.35f, 30.0f},
                {280.35f, 210.0f}, {40.35f, 210.0f}};
            result.poses.push_back(pose);
        }

        // 使用真实保存/打开动作验证主窗口与会话层的集成。
        const QString projectPath = directory.filePath("session.calibration.json");
        int diagnosticsShown = 0;
        int comparisonShown = 0;
        QTimer dialogDriver;
        QObject::connect(&dialogDriver, &QTimer::timeout, &window, [&]
        {
            for (QWidget *widget: QApplication::topLevelWidgets())
            {
                if (auto *file = qobject_cast<QFileDialog *>(widget);
                    file != nullptr && file->isVisible())
                {
                    file->setDirectory(directory.path());
                    file->selectFile(QFileInfo(projectPath).fileName());
                    // 已显示的文件对话框会保留原文件名，直接填写其标准文件名输入框。
                    child<QLineEdit>(*file, "fileNameEdit")->setText(projectPath);
                    QMetaObject::invokeMethod(file, "accept", Qt::QueuedConnection);
                }
                else if (auto *message = qobject_cast<QMessageBox *>(widget);
                    message != nullptr && message->isVisible())
                {
                    message->accept();
                }
                else if (widget->isVisible()
                    && widget->objectName() == QStringLiteral("calibrationDiagnosticsDialog"))
                {
                    ++diagnosticsShown;
                    qobject_cast<QDialog *>(widget)->accept();
                }
                else if (widget->isVisible()
                    && widget->objectName() == QStringLiteral("imageComparisonDialog"))
                {
                    ++comparisonShown;
                    qobject_cast<QDialog *>(widget)->accept();
                }
            }
        });
        dialogDriver.start(10);
        require(QMetaObject::invokeMethod(&window, "onApplyComparisonResult",
                    Qt::DirectConnection, Q_ARG(CalibrationResult, result)),
            "无法建立待保存结果");
        child<QAction>(window, "saveProjectAction")->trigger();
        QString saveError;
        const auto saved = CalibrationProjectIo::load(projectPath, &saveError);
        if (!saved)
        {
            qCritical() << "无法读取界面保存的项目：" << saveError << projectPath;
        }
        require(saved.has_value() && saved->result.has_value()
                && saved->imagePaths == paths
                && saved->disabledImagePaths == QStringList{paths[3]}
                && saved->result->poses.size() == 3
                && saved->result->poses[0].detectedPoints.size() == 4,
            "保存动作未记录完整结果及图片停用状态");

        child<QAction>(window, "clearAction")->trigger();
        require(list->count() == 0 && !child<QAction>(window, "exportAction")->isEnabled(),
            "清空未移除当前会话");
        child<QAction>(window, "openProjectAction")->trigger();
        require(list->count() == 4 && list->item(3)->checkState() == Qt::Unchecked,
            "打开动作未恢复全部图片和停用状态");
        require(child<QAction>(window, "exportAction")->isEnabled()
                && child<QAction>(window, "poseResultAction")->isEnabled()
                && child<QCheckBox>(window, "showUndistortedCheck")->isEnabled(),
            "恢复后未直接启用参数导出、位姿查看及去畸变");
        list->setCurrentRow(0);
        require(child<QAction>(window, "calibrationDiagnosticsAction")->isEnabled(),
            "恢复逐角点数据后未启用覆盖与误差诊断");
        require(child<QAction>(window, "undistortionComparisonAction")->isEnabled(),
            "恢复结果并选中图片后未启用对比预览");
        child<QAction>(window, "calibrationDiagnosticsAction")->trigger();
        child<QAction>(window, "undistortionComparisonAction")->trigger();
        require(diagnosticsShown == 1 && comparisonShown == 1,
            "诊断或对比动作未打开对应窗口");
        dialogDriver.stop();
        list->item(3)->setCheckState(Qt::Checked);
        require(!child<QAction>(window, "exportAction")->isEnabled(),
            "恢复后更改图片参与状态未使结果失效");
        require(!child<QAction>(window, "calibrationDiagnosticsAction")->isEnabled()
                && !child<QAction>(window, "undistortionComparisonAction")->isEnabled(),
            "改变图片参与状态后诊断和对比应一起失效");
    }

    void testBatchExportKeepsExistingFiles()
    {
        QTemporaryDir directory;
        require(directory.isValid(), "无法建立批量导出目录");
        const QString first = directory.filePath("frame.png");
        const QString second = directory.filePath("frame_undistorted.png");
        const QString previous = directory.filePath("frame_undistorted_2.png");
        QImage image(320, 240, QImage::Format_RGB32);
        image.fill(Qt::red);
        require(image.save(first), "无法保存第一张批量输入");
        image.fill(Qt::blue);
        require(image.save(second), "无法保存与输出同名的输入");
        image.fill(Qt::green);
        require(image.save(previous), "无法保存已有导出结果");

        MainWindow window;
        window.loadImages({first, second});
        QEventLoop wait;
        QTimer driver;
        QString summary;
        QObject::connect(&driver, &QTimer::timeout, &window, [&]
        {
            for (QWidget *widget: QApplication::topLevelWidgets())
            {
                if (!widget->isVisible()) continue;
                if (auto *dialog = qobject_cast<QFileDialog *>(widget))
                {
                    child<QLineEdit>(*dialog, "fileNameEdit")->setText(directory.path());
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
                else if (auto *message = qobject_cast<QMessageBox *>(widget))
                {
                    if (message->windowTitle() == QStringLiteral("批量导出去畸变图片"))
                    {
                        summary = message->text();
                        wait.quit();
                    }
                    message->accept();
                }
            }
        });
        driver.start(10);
        const auto result = successfulResult(0.2, 500.0);
        require(QMetaObject::invokeMethod(&window, "onApplyComparisonResult",
            Qt::DirectConnection, Q_ARG(CalibrationResult, result)), "无法应用批量导出参数");
        child<QAction>(window, "batchExportAction")->trigger();
        if (summary.isEmpty()) wait.exec();
        driver.stop();

        require(summary.contains("已导出 2 张，跳过 0 张") && !summary.contains("取消"),
            "批量任务应正确报告完成状态");
        require(QImage(second).pixelColor(160, 120) == QColor(Qt::blue)
                && QImage(previous).pixelColor(160, 120) == QColor(Qt::green),
            "批量导出覆盖了输入或已有输出");
        require(QImage(directory.filePath("frame_undistorted_3.png")).pixelColor(160, 120) == QColor(Qt::red)
                && QImage(directory.filePath("frame_undistorted_undistorted.png")).pixelColor(160, 120) == QColor(Qt::blue),
            "导出文件未分别对应两张输入图");
    }

    void testFailureReasons()
    {
        QTemporaryDir directory;
        require(directory.isValid(), "无法建立失败原因测试目录");
        QStringList paths;
        for (int index = 0; index < 4; ++index)
        {
            QImage image(index == 3 ? QSize(160, 120) : QSize(320, 240), QImage::Format_RGB32);
            image.fill(Qt::gray);
            const QString path = directory.filePath(QStringLiteral("blank_%1.png").arg(index));
            require(image.save(path), "无法保存失败原因测试图片");
            paths << path;
        }
        paths << directory.filePath("missing.png") << directory.filePath("corrupt.png");
        QFile corrupt(paths.last());
        require(corrupt.open(QIODevice::WriteOnly), "无法创建损坏图片");
        corrupt.write("not an image");
        corrupt.close();

        MainWindow window;
        window.loadImages(paths);
        auto *list = child<QListWidget>(window, "imageList");
        auto *filter = child<QComboBox>(window, "imageFilterCombo");
        require(list->count() == 6 && list->item(4)->text().contains(QStringLiteral("读取失败"))
                && list->item(5)->toolTip().contains(QStringLiteral("无法读取")),
            "无法读取的图片必须保留并展示具体原因");
        int resultMessages = 0;
        QEventLoop loop;
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, &window, [&]
        {
            for (QWidget *widget: QApplication::topLevelWidgets())
            {
                if (auto *message = qobject_cast<QMessageBox *>(widget);
                    message != nullptr && message->isVisible())
                {
                    ++resultMessages;
                    message->accept();
                }
            }
            if (resultMessages > 0 && child<QAction>(window, "calibrateAction")->isEnabled())
            {
                loop.quit();
            }
        });
        driver.start(10);
        child<QAction>(window, "calibrateAction")->trigger();
        loop.exec();
        driver.stop();
        const auto countVisible = [&]
        {
            int count = 0;
            for (int row = 0; row < list->count(); ++row)
            {
                count += !list->item(row)->isHidden();
            }
            return count;
        };
        filter->setCurrentIndex(filter->findData(5));
        require(countVisible() == 2, "读取失败筛选未正确区分损坏/缺失文件");
        filter->setCurrentIndex(filter->findData(6));
        require(countVisible() == 1 && list->item(3)->toolTip().contains(QStringLiteral("160")),
            "分辨率冲突必须单独筛选并显示实际尺寸");
        list->setCurrentRow(3);
        require(list->item(3)->text().contains(imageDetectionStatusText(ImageDetectionStatus::SizeMismatch)),
            "单图预览不得覆盖失败批次的分辨率冲突原因");
        filter->setCurrentIndex(filter->findData(7));
        require(countVisible() == 3, "未找到标定板筛选不应混入读取或尺寸错误");
        filter->setCurrentIndex(filter->findData(8));
        require(countVisible() == 0, "空白图片不能虚报为角点不足");
        filter->setCurrentIndex(filter->findData(4));
        require(countVisible() == 6, "全部失败筛选应包含所有失败类别");
    }
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    // 模态对话框若未被自动关闭，直接使测试失败，避免无人值守运行挂起。
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, &application,
        []
        {
            qCritical("界面功能测试超时");
            std::_Exit(EXIT_FAILURE);
        });
    watchdog.start(15000);
    try
    {
        testComparisonApplication();
        testDropImagesAndFolders();
        testImageParticipationAndFilters();
        testImportParameters();
        testSessionRestoration();
        testBatchExportKeepsExistingFiles();
        testFailureReasons();
        qInfo("界面功能测试通过");
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        qCritical() << "界面功能测试失败：" << error.what();
        return EXIT_FAILURE;
    }
}
