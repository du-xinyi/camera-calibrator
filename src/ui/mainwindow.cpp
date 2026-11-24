#include "mainwindow.h"
#include "algorithm_comparison_dialog.h"
#include "calibration_project.h"
#include "calibration_diagnostics_dialog.h"
#include "image_comparison_dialog.h"
#include "image_quality.h"
#include "pose_result_dialog.h"
#include "theme.h"
#include "ui_mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFont>
#include <QFutureWatcher>
#include <QIcon>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <QtConcurrent>

#include <algorithm>
#include <bitset>
#include <cmath>
#include <utility>

namespace
{
    constexpr int kThumbnailSize = 96;
    constexpr int kRoleFilePath = Qt::UserRole + 1;
    constexpr int kRoleImageHash = Qt::UserRole + 2;
    constexpr int kRoleQualityWarnings = Qt::UserRole + 3;
    constexpr int kRoleEnabled = Qt::UserRole + 4;
    constexpr int kRoleDetection = Qt::UserRole + 5;
    constexpr int kRoleDetectionMessage = Qt::UserRole + 6;
    constexpr int kRoleBaseToolTip = Qt::UserRole + 7;
    constexpr int kRoleBaseText = Qt::UserRole + 8;

    enum ImageFilter
    {
        AllImages,
        EnabledImages,
        DisabledImages,
        QualityWarnings,
        DetectionFailures,
        ReadFailures,
        SizeMismatches,
        MissingBoards,
        InsufficientCorners,
    };

    QIcon appIcon(const char *name)
    {
        // 使用随程序编译的统一线性图标，避免不同桌面主题风格混杂
        return QIcon(QStringLiteral(":/icons/%1.svg").arg(QLatin1String(name)));
    }

    // 刷新动态属性选择器，使样式表按新状态重新着色
    void setStyleState(QWidget *widget, const char *state)
    {
        widget->setProperty("state", QString::fromLatin1(state));
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    }

    QStringList supportedImageNameFilters()
    {
        const auto formats = QImageReader::supportedImageFormats();
        QStringList filters;
        filters.reserve(formats.size());
        for (const QByteArray &fmt: formats)
        {
            filters << QStringLiteral("*.") + QString::fromLatin1(fmt);
        }
        return filters;
    }

    QString supportedImageDialogFilter()
    {
        return QStringLiteral("图片 (%1)").arg(supportedImageNameFilters().join(QLatin1Char(' ')));
    }

    QStringList imagesInFolder(const QString &path)
    {
        const QDir directory(path);
        const auto entries = directory.entryInfoList(supportedImageNameFilters(),
            QDir::Files | QDir::Readable, QDir::Name);
        QStringList files;
        for (const QFileInfo &entry: entries)
        {
            files << entry.absoluteFilePath();
        }
        return files;
    }

    QFileInfoList droppedImageSources(const QMimeData *data)
    {
        const auto filters = supportedImageNameFilters();
        QFileInfoList sources;
        const auto urls = data->urls();
        for (const QUrl &url: urls)
        {
            if (url.isLocalFile())
            {
                const QFileInfo info(url.toLocalFile());
                if (info.isDir() || (info.isFile() && QDir::match(filters, info.fileName())))
                {
                    sources << info;
                }
            }
        }
        return sources;
    }

    bool sameCalibrationOptions(const CalibrationOptions &lhs,
        const CalibrationOptions &rhs)
    {
        return lhs.cameraModel == rhs.cameraModel
                && lhs.boardType == rhs.boardType
                && lhs.boardSize == rhs.boardSize
                && lhs.squareSize == rhs.squareSize
                && lhs.markerSize == rhs.markerSize
                && lhs.dictionary == rhs.dictionary
                && lhs.method == rhs.method
                && lhs.skew == rhs.skew
                && lhs.tangential == rhs.tangential
                && lhs.radialCoeffs == rhs.radialCoeffs;
    }

    QString resultRmsText(const CalibrationResult &result)
    {
        if (!result.success)
        {
            return QStringLiteral("--");
        }
        return result.hasMetadata(QStringLiteral("rms_reprojection_error"))
            ? QObject::tr("%1 px").arg(result.rmsError, 0, 'f', 3)
            : QObject::tr("未提供");
    }

    void setComboValue(QComboBox *combo, int value)
    {
        const int index = combo->findData(value);
        if (index >= 0)
        {
            combo->setCurrentIndex(index);
        }
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      ui_(std::make_unique<Ui::MainWindow>())
{
    ui_->setupUi(this);
    setAcceptDrops(true);

#ifdef PROJECT_SOURCE_DIR

    lastDir_ = QString::fromUtf8(PROJECT_SOURCE_DIR);
#else
    const QString pictures =
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    lastDir_ = pictures.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation) : pictures;
#endif

    buildMenuBar();
    buildToolBar();
    configureCentralWidget();
    buildStatusBar();
    Theme::repolish(this);
    restoringProject_ = false;
    setProjectModified(false);
}

MainWindow::~MainWindow()
{
    // 先移除完成回调再等待任务，避免析构过程中重新进入窗口逻辑
    if (calibWatcher_ != nullptr)
    {
        disconnect(calibWatcher_, &QFutureWatcher<CalibrationResult>::finished,
            this, nullptr);
        if (calibWatcher_->isRunning())
        {
            calibCanceled_.store(true);
            calibWatcher_->waitForFinished();
        }
    }

    // 两类辅助任务也捕获了窗口成员，必须在 QObject 子对象销毁前结束
    auxiliaryCanceled_.store(true);
    if (comparisonWatcher_ != nullptr && comparisonWatcher_->isRunning())
    {
        disconnect(comparisonWatcher_, nullptr, this, nullptr);
        comparisonWatcher_->waitForFinished();
    }
    if (batchWatcher_ != nullptr && batchWatcher_->isRunning())
    {
        disconnect(batchWatcher_, nullptr, this, nullptr);
        batchWatcher_->waitForFinished();
    }
    QCoreApplication::removePostedEvents(this);
}

void MainWindow::buildMenuBar()
{
    auto *fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    openProjectAction_ = fileMenu->addAction(
        tr("打开项目(&O)..."), QKeySequence(QStringLiteral("Ctrl+Alt+O")),
        this, &MainWindow::onOpenProject);
    openProjectAction_->setObjectName(QStringLiteral("openProjectAction"));
    saveProjectAction_ = fileMenu->addAction(
        tr("保存项目(&S)..."), QKeySequence(QStringLiteral("Ctrl+Alt+S")),
        this, &MainWindow::onSaveProject);
    saveProjectAction_->setObjectName(QStringLiteral("saveProjectAction"));
    saveProjectAsAction_ = fileMenu->addAction(
        tr("项目另存为..."), this, &MainWindow::onSaveProjectAs);
    saveProjectAsAction_->setObjectName(QStringLiteral("saveProjectAsAction"));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("添加图片(&A)..."), QKeySequence::Open, this,
        &MainWindow::onAddImages);
    fileMenu->addAction(tr("从文件夹添加(&F)..."),
        QKeySequence("Ctrl+Shift+O"), this,
        &MainWindow::onAddImagesFromFolder);
    fileMenu->addSeparator();
    importAction_ = fileMenu->addAction(
        tr("导入相机参数(&I)..."), this, &MainWindow::onImportParameters);
    importAction_->setObjectName(QStringLiteral("importParametersAction"));
    batchExportAction_ = fileMenu->addAction(
        tr("批量导出去畸变图片(&U)..."), this,
        &MainWindow::onExportUndistortedImages);
    batchExportAction_->setObjectName(QStringLiteral("batchExportAction"));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("退出(&X)"), QKeySequence::Quit, this, &QWidget::close);

    auto *calibrationMenu = menuBar()->addMenu(tr("标定(&C)"));
    compareAction_ = calibrationMenu->addAction(
        tr("比较标定算法..."), this, &MainWindow::onCompareAlgorithms);
    compareAction_->setObjectName(QStringLiteral("compareAlgorithmsAction"));

    auto *viewMenu = menuBar()->addMenu(tr("视图(&V)"));
    viewMenu->addAction(tr("适应窗口"), QKeySequence("Ctrl+0"), this,
        &MainWindow::fitImageToView);
    viewMenu->addSeparator();
    diagnosticsAction_ = viewMenu->addAction(
        tr("覆盖与误差..."), this, &MainWindow::onShowDiagnostics);
    diagnosticsAction_->setObjectName(QStringLiteral("calibrationDiagnosticsAction"));
    undistortionComparisonAction_ = viewMenu->addAction(
        tr("去畸变对比..."), this, &MainWindow::onShowUndistortionComparison);
    undistortionComparisonAction_->setObjectName(QStringLiteral("undistortionComparisonAction"));

    menuBar()->addMenu(tr("帮助(&H)"))->addAction(tr("关于(&A)"), this, [this]
    {
        QMessageBox::about(this, tr("关于"),
            tr("相机标定\n基于 Qt 的相机标定工具，"
                "布局参考 MATLAB Camera Calibrator。"));
    });
}

void MainWindow::buildToolBar()
{
    auto *bar = addToolBar(tr("主工具栏"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->setMovable(false);
    bar->setIconSize({20, 20});
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto *addImagesAction = bar->addAction(appIcon("image"),
        tr("添加图片"), this, &MainWindow::onAddImages);
    addImagesAction->setToolTip(tr("添加图片"));
    auto *addFolderAction = bar->addAction(appIcon("folder"),
        tr("文件夹"), this, &MainWindow::onAddImagesFromFolder);
    addFolderAction->setToolTip(tr("从文件夹添加图片"));
    bar->addSeparator();
    calibrateAction_ = bar->addAction(appIcon("calibrate"),
        tr("标定"), this, &MainWindow::onCalibrate);
    calibrateAction_->setObjectName(QStringLiteral("calibrateAction"));
    calibrateAction_->setToolTip(tr("至少添加 3 张图片后开始标定"));
    bar->addAction(compareAction_);
    compareAction_->setIcon(appIcon("compare"));
    compareAction_->setText(tr("算法对比"));
    compareAction_->setToolTip(tr("比较 Pinhole、Fisheye 模型与可用角点检测器"));
    bar->addSeparator();
    poseAction_ = bar->addAction(appIcon("pose"),
        tr("位姿"), this, &MainWindow::onShowPoses);
    poseAction_->setObjectName(QStringLiteral("poseResultAction"));
    poseAction_->setToolTip(tr("查看位姿估计结果"));
    bar->addAction(diagnosticsAction_);
    diagnosticsAction_->setIcon(appIcon("diagnostics"));
    bar->addAction(undistortionComparisonAction_);
    undistortionComparisonAction_->setIcon(appIcon("split"));
    exportAction_ = bar->addAction(appIcon("export"),
        tr("导出"), this, &MainWindow::onExportParameters);
    exportAction_->setObjectName(QStringLiteral("exportAction"));
    exportAction_->setToolTip(tr("导出相机参数"));

    // 清空属于破坏性操作，放在工具栏最右侧与常用操作分开
    auto *spacer = new QWidget(bar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    clearAction_ = bar->addAction(appIcon("clear"),
        tr("清空"), this, &MainWindow::onClearAll);
    clearAction_->setObjectName(QStringLiteral("clearAction"));
    clearAction_->setToolTip(tr("清空已添加图片"));

    // 右侧面板的主按钮与工具栏共用同一个动作，启用状态和提示随动作同步
    auto *calibrateButton = ui_->calibrateButton;
    calibrateButton->setIcon(appIcon("calibrate-light"));
    calibrateButton->setIconSize({18, 18});
    const auto syncCalibrateButton = [this, calibrateButton]
    {
        calibrateButton->setEnabled(calibrateAction_->isEnabled());
        calibrateButton->setToolTip(calibrateAction_->toolTip());
    };
    connect(calibrateAction_, &QAction::changed, calibrateButton, syncCalibrateButton);
    connect(calibrateButton, &QPushButton::clicked, calibrateAction_, &QAction::trigger);
    syncCalibrateButton();
}

void MainWindow::configureCentralWidget()
{
    imageList_ = ui_->imageList;
    imageScroll_ = ui_->imageScroll;
    imageDisplay_ = ui_->imageDisplay;
    cameraModelCombo_ = ui_->cameraModelCombo;
    boardTypeCombo_ = ui_->boardTypeCombo;
    boardColsSpin_ = ui_->boardColsSpin;
    boardRowsSpin_ = ui_->boardRowsSpin;
    squareSizeSpin_ = ui_->squareSizeSpin;
    markerSizeSpin_ = ui_->markerSizeSpin;
    dictionaryCombo_ = ui_->dictionaryCombo;
    methodCombo_ = ui_->methodCombo;
    skewCheck_ = ui_->skewCheck;
    tangentialCheck_ = ui_->tangentialCheck;
    radialCoeffSpin_ = ui_->radialCoeffSpin;
    showUndistortedCheck_ = ui_->showUndistortedCheck;

    imageList_->setIconSize({kThumbnailSize, kThumbnailSize});
    connect(imageList_, &QListWidget::itemSelectionChanged, this,
        &MainWindow::onImageSelectionChanged);
    connect(imageList_, &QListWidget::itemChanged, this,
        &MainWindow::onImageParticipationChanged);
    imageList_->setToolTip(tr("勾选图片参与标定；右键可批量启用或停用，停用后仍可查看图片。"));
    imageList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(imageList_, &QListWidget::customContextMenuRequested, this,
        [this](const QPoint &position)
        {
            if (isBusy())
            {
                return;
            }
            QStringList selected;
            for (const QListWidgetItem *item: imageList_->selectedItems())
            {
                if (!item->isHidden())
                {
                    selected << item->data(kRoleFilePath).toString();
                }
            }
            QMenu menu(this);
            auto *enable = menu.addAction(tr("启用选中图片"));
            auto *disable = menu.addAction(tr("停用选中图片"));
            enable->setEnabled(!selected.isEmpty());
            disable->setEnabled(!selected.isEmpty());
            menu.addSeparator();
            auto *restore = menu.addAction(tr("恢复全部图片参与标定"));
            QAction *chosen = menu.exec(imageList_->viewport()->mapToGlobal(position));
            if (chosen == enable || chosen == disable)
            {
                setImageParticipation(selected, chosen == enable);
            }
            else if (chosen == restore)
            {
                setImageParticipation(collectFiles(false), true);
            }
        });

    imageFilterCombo_ = new QComboBox(ui_->imagePanel);
    imageFilterCombo_->setObjectName(QStringLiteral("imageFilterCombo"));
    imageFilterCombo_->addItem(tr("全部图片"), AllImages);
    imageFilterCombo_->addItem(tr("参与标定"), EnabledImages);
    imageFilterCombo_->addItem(tr("已停用"), DisabledImages);
    imageFilterCombo_->addItem(tr("质量提示"), QualityWarnings);
    imageFilterCombo_->addItem(tr("检测失败"), DetectionFailures);
    imageFilterCombo_->addItem(tr("读取失败"), ReadFailures);
    imageFilterCombo_->addItem(tr("分辨率不符"), SizeMismatches);
    imageFilterCombo_->addItem(tr("未找到标定板"), MissingBoards);
    imageFilterCombo_->addItem(tr("角点不足"), InsufficientCorners);
    ui_->imagePanelLayout->insertWidget(1, imageFilterCombo_);
    connect(imageFilterCombo_, &QComboBox::currentIndexChanged, this,
        [this] { refreshImageFilter(); });

    imageDisplay_->setBackgroundRole(QPalette::Dark);
    imageDisplay_->setAutoFillBackground(true);
    imageScroll_->setBackgroundRole(QPalette::Dark);
    imageScroll_->viewport()->setBackgroundRole(QPalette::Dark);

    cameraModelCombo_->setItemData(
        0, static_cast<int>(CameraModel::Pinhole));
    cameraModelCombo_->setItemData(
        1, static_cast<int>(CameraModel::Fisheye));
    boardTypeCombo_->setItemData(
        0, static_cast<int>(CalibrationBoardType::Charuco));
    boardTypeCombo_->setItemData(
        1, static_cast<int>(CalibrationBoardType::Chessboard));
    dictionaryCombo_->setItemData(
        0, static_cast<int>(ArucoDictionary::Dict5x5_100));
    dictionaryCombo_->setItemData(
        1, static_cast<int>(ArucoDictionary::Dict5x5_250));
    dictionaryCombo_->setItemData(
        2, static_cast<int>(ArucoDictionary::Dict4x4_50));
    dictionaryCombo_->setItemData(
        3, static_cast<int>(ArucoDictionary::Dict6x6_250));
    dictionaryCombo_->setItemData(
        4, static_cast<int>(ArucoDictionary::Original));
    methodCombo_->setItemData(
        0, static_cast<int>(CalibrationMethod::Classic));
    methodCombo_->setItemData(
        1, static_cast<int>(CalibrationMethod::SectorBased));

    boardTypeCombo_->setToolTip(tr("混合棋盘格（ChArUco）由棋盘格与 ArUco 标记组成；普通棋盘格不含编码标记。"));
    markerSizeSpin_->setToolTip(tr("仅混合棋盘格（ChArUco）使用"));
    dictionaryCombo_->setToolTip(tr("仅混合棋盘格（ChArUco）使用"));
    methodCombo_->setToolTip(tr("仅普通棋盘格使用：传统方法 / 扇区法"));
    skewCheck_->setToolTip(tr("仅鱼眼模型使用：是否估计成像坐标轴的倾斜参数"));
    tangentialCheck_->setToolTip(tr("仅 Pinhole 使用：是否估计切向畸变"));
    radialCoeffSpin_->setToolTip(tr("参与估计的径向系数数量：Pinhole 为 2–3，Fisheye 为 2–4"));

    connect(showUndistortedCheck_, &QCheckBox::toggled, this,
        &MainWindow::onDisplayModeChanged);

    // 连续调整参数时合并预览请求，避免反复执行角点检测
    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(150);
    connect(debounceTimer_, &QTimer::timeout, this,
        &MainWindow::onImageSelectionChanged);

    connect(cameraModelCombo_,
        qOverload<int>(&QComboBox::currentIndexChanged), this,
        &MainWindow::onCameraModelChanged);
    connect(boardTypeCombo_,
        qOverload<int>(&QComboBox::currentIndexChanged), this,
        &MainWindow::onBoardTypeChanged);
    connect(boardColsSpin_,
        qOverload<int>(&QSpinBox::valueChanged), this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(boardRowsSpin_,
        qOverload<int>(&QSpinBox::valueChanged), this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(methodCombo_,
        qOverload<int>(&QComboBox::currentIndexChanged), this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(markerSizeSpin_,
        qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(dictionaryCombo_,
        qOverload<int>(&QComboBox::currentIndexChanged), this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(skewCheck_, &QCheckBox::toggled, this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(tangentialCheck_, &QCheckBox::toggled, this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(radialCoeffSpin_,
        qOverload<int>(&QSpinBox::valueChanged), this,
        &MainWindow::onCalibrationOptionsChanged);
    connect(squareSizeSpin_,
        qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this](double squareSize)
        {
            markerSizeSpin_->setMaximum(
                qMax(0.001, squareSize - 0.001));
            onCalibrationOptionsChanged();
        });
    onCameraModelChanged();
    onBoardTypeChanged();

    ui_->mainSplitter->setStretchFactor(0, 0);
    ui_->mainSplitter->setStretchFactor(1, 1);
    ui_->mainSplitter->setStretchFactor(2, 0);
    ui_->mainSplitter->setSizes({260, 770, 330});
}

void MainWindow::buildStatusBar()
{
    statusBar()->setSizeGripEnabled(false);
    statusImageCount_ = new QLabel(tr("0 张图片"), this);
    statusRmsError_ = new QLabel(tr("RMS --"), this);
    statusRmsError_->setObjectName(QStringLiteral("statusRmsLabel"));
    statusBar()->addWidget(statusImageCount_);
    statusBar()->addPermanentWidget(statusRmsError_);
    updateUiState();
}

void MainWindow::updateResultSummary()
{
    if (statusRmsError_ == nullptr)
    {
        return;
    }
    const CalibrationResult &result = lastResult_;
    const bool valid = result.success && result.cameraMatrix.rows == 3
            && result.cameraMatrix.cols == 3;
    const char *state = !result.success || !result.hasMetadata(QStringLiteral("quality_warning"))
        ? "empty" : result.qualityWarning ? "warning" : "good";
    const QString rmsText = resultRmsText(result);
    statusRmsError_->setText(tr("RMS %1%2").arg(rmsText,
        lastResultImported_ ? tr("（导入）") : QString{}));
    setStyleState(statusRmsError_, state);

    ui_->resultRmsValue->setText(rmsText);
    setStyleState(ui_->resultRmsValue, state);
    if (!valid)
    {
        ui_->resultDetailsLabel->setText(tr("尚未标定"));
        return;
    }
    const auto at = [&result](int row, int col)
    {
        return QString::number(result.cameraMatrix.at<double>(row, col), 'f', 2);
    };
    QString details = tr("fx %1   fy %2\ncx %3   cy %4")
            .arg(at(0, 0), at(1, 1), at(0, 2), at(1, 2));
    if (lastResultImported_)
    {
        details += tr("\n来源：导入参数");
    }
    const QString imagesUsed = result.hasMetadata(QStringLiteral("images_used"))
        ? QString::number(result.imagesUsed) : tr("未提供");
    const QString imagesTotal = result.hasMetadata(QStringLiteral("images_total"))
        ? QString::number(result.imagesTotal) : tr("未提供");
    details += tr("\n有效图片 %1 / %2").arg(imagesUsed, imagesTotal);
    if (result.imageSize.isValid())
    {
        details += tr("\n分辨率 %1 × %2").arg(result.imageSize.width())
                .arg(result.imageSize.height());
    }
    ui_->resultDetailsLabel->setText(details);
}

void MainWindow::updateUiState()
{
    const int imageCount = imageList_ == nullptr ? 0 : imageList_->count();
    const int enabledCount = imageList_ == nullptr ? 0 : collectFiles().size();
    const bool hasImages = imageCount > 0;
    const bool hasCalibration = lastResult_.success;
    const bool hasDiagnostics = hasCalibration && std::any_of(
        lastResult_.poses.begin(), lastResult_.poses.end(),
        [](const CalibrationPose &pose)
        {
            return !pose.detectedPoints.empty()
                    && pose.detectedPoints.size() == pose.projectedPoints.size();
        });
    const bool busy = isBusy();

    if (statusImageCount_ != nullptr)
    {
        statusImageCount_->setText(tr("%1 / %2 张参与标定").arg(enabledCount).arg(imageCount));
    }
    ui_->imageCountBadge->setText(tr("%1 / %2").arg(enabledCount).arg(imageCount));
    ui_->imageListStack->setCurrentIndex(hasImages ? 1 : 0);
    updateResultSummary();

    if (currentPixmap_.isNull())
    {
        imageDisplay_->clear();
        const auto selected = imageList_->selectedItems();
        const QString failureMessage = selected.isEmpty() ? QString{}
                : selected.first()->data(kRoleDetectionMessage).toString();
        imageDisplay_->setText(!failureMessage.isEmpty() ? failureMessage
            : hasImages ? tr("选择左侧图片查看预览") : tr("添加图片或文件夹以开始标定"));
    }

    calibrateAction_->setEnabled(enabledCount >= 3 && !busy);
    compareAction_->setEnabled(enabledCount >= 3 && !busy);
    calibrateAction_->setToolTip(
        busy ? tr("标定正在进行") : enabledCount >= 3 ? tr("使用所有启用图片开始标定")
            : tr("至少勾选 3 张图片后开始标定"));
    clearAction_->setEnabled((hasImages || hasCalibration || projectDirty_) && !busy);
    saveProjectAction_->setEnabled((hasImages || hasCalibration || projectDirty_) && !busy);
    saveProjectAsAction_->setEnabled(saveProjectAction_->isEnabled());
    openProjectAction_->setEnabled(!busy);
    importAction_->setEnabled(!busy);
    exportAction_->setEnabled(hasCalibration && !busy);
    batchExportAction_->setEnabled(hasCalibration && enabledCount > 0 && !busy);
    exportAction_->setToolTip(
        hasCalibration ? tr("导出相机参数") : tr("完成一次成功标定后才能导出"));
    poseAction_->setEnabled(
        hasCalibration && !lastResult_.poses.empty() && !busy);
    poseAction_->setToolTip(
        hasCalibration ? tr("查看位姿估计结果") : tr("完成一次成功标定后查看位姿"));
    diagnosticsAction_->setEnabled(hasDiagnostics && !busy);
    diagnosticsAction_->setToolTip(hasDiagnostics
        ? tr("查看角点覆盖热力图与逐角点重投影误差")
        : tr("重新标定以生成角点诊断数据；仅导入参数或旧项目无法提供角点诊断"));
    undistortionComparisonAction_->setEnabled(
        hasCalibration && !imageList_->selectedItems().isEmpty() && !busy);
    undistortionComparisonAction_->setToolTip(
        tr("选择一张图片后，并排或拖动分割线对比原图与去畸变图，支持缩放和平移"));
    showUndistortedCheck_->setEnabled(hasCalibration && !busy);
    ui_->rightPanel->setEnabled(!busy);
    imageList_->setEnabled(!busy);
    imageFilterCombo_->setEnabled(!busy);
}

bool MainWindow::isBusy() const
{
    return calibActive_ || auxiliaryActive_;
}

void MainWindow::setProjectModified(bool modified)
{
    projectDirty_ = modified;
    setWindowTitle(tr("相机标定 — %1[*]").arg(currentProjectPath_.isEmpty()
            ? tr("未命名项目") : QFileInfo(currentProjectPath_).fileName()));
    setWindowModified(modified);
    updateUiState();
}

void MainWindow::markProjectModified()
{
    if (!restoringProject_)
    {
        setProjectModified(true);
    }
}

bool MainWindow::confirmDiscardChanges()
{
    if (!projectDirty_)
    {
        return true;
    }
    QMessageBox message(QMessageBox::Warning, tr("项目尚未保存"),
        tr("当前项目有未保存的修改，是否先保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    message.setObjectName(QStringLiteral("unsavedChangesDialog"));
    message.button(QMessageBox::Save)->setText(tr("保存"));
    message.button(QMessageBox::Discard)->setText(tr("放弃修改"));
    message.button(QMessageBox::Cancel)->setText(tr("取消"));
    message.setDefaultButton(QMessageBox::Save);
    message.setEscapeButton(QMessageBox::Cancel);
    const int answer = message.exec();
    if (answer == QMessageBox::Discard)
    {
        return true;
    }
    return answer == QMessageBox::Save && saveProject();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (isBusy())
    {
        statusBar()->showMessage(tr("请先等待当前任务完成或取消任务，再关闭窗口。"), 5000);
        event->ignore();
        return;
    }
    if (!confirmDiscardChanges())
    {
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::invalidateCalibrationResult()
{
    lastResult_ = {};
    lastResultImported_ = false;
    for (int row = 0; row < imageList_->count(); ++row)
    {
        const auto *item = imageList_->item(row);
        if (static_cast<ImageDetectionStatus>(item->data(kRoleDetection).toInt())
            == ImageDetectionStatus::SizeMismatch)
        {
            ImageDetectionReport report;
            report.imagePath = item->data(kRoleFilePath).toString();
            updateImageReport(report);
        }
    }
    showUndistortedCheck_->setChecked(false);
    if (poseDialog_)
    {
        poseDialog_->close();
        poseDialog_.clear();
    }
    updateUiState();
}

void MainWindow::onAddImages()
{
    if (isBusy())
    {
        return;
    }
    const auto files = QFileDialog::getOpenFileNames(
        this, tr("选择图片"), lastDir_, supportedImageDialogFilter());
    if (files.isEmpty())
    {
        return;
    }
    lastDir_ = QFileInfo(files.first()).absolutePath();
    loadImages(files);
}

void MainWindow::onAddImagesFromFolder()
{
    if (isBusy())
    {
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择图片文件夹"), lastDir_);
    if (dir.isEmpty())
    {
        return;
    }
    lastDir_ = dir;

    const QStringList files = imagesInFolder(dir);
    if (files.isEmpty())
    {
        QMessageBox::information(this, tr("从文件夹添加"),
            tr("所选文件夹中未找到图片文件。"));
        return;
    }
    loadImages(files);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!isBusy() && event->possibleActions().testFlag(Qt::CopyAction)
        && !droppedImageSources(event->mimeData()).isEmpty())
    {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (isBusy() || !event->possibleActions().testFlag(Qt::CopyAction))
    {
        return;
    }
    const auto sources = droppedImageSources(event->mimeData());
    if (sources.isEmpty())
    {
        return;
    }
    QStringList files;
    for (const QFileInfo &source: sources)
    {
        if (source.isDir())
        {
            files << imagesInFolder(source.absoluteFilePath());
        }
        else
        {
            files << source.absoluteFilePath();
        }
    }
    if (files.isEmpty())
    {
        statusBar()->showMessage(tr("拖入的文件夹中未找到图片文件。"), 5000);
    }
    else
    {
        loadImages(files);
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void MainWindow::onOpenProject()
{
    if (isBusy())
    {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开标定项目"), lastDir_,
        tr("标定项目 (*.calibration.json *.json)"));
    if (path.isEmpty())
    {
        return;
    }
    // 先处理当前修改，再读取目标；重新打开同一文件时必须读到刚保存的新内容。
    if (!confirmDiscardChanges())
    {
        return;
    }
    QString error;
    const auto project = CalibrationProjectIo::load(path, &error);
    if (!project.has_value())
    {
        QMessageBox::warning(this, tr("打开标定项目"),
            tr("打开失败：%1").arg(error));
        return;
    }
    const QScopedValueRollback<bool> restoring(restoringProject_, true);
    imageList_->clear();
    currentPixmap_ = {};
    invalidateCalibrationResult();
    imageFilterCombo_->setCurrentIndex(AllImages);
    applyOptions(project->options);
    loadImages(project->imagePaths);
    setImageParticipation(project->disabledImagePaths, false);
    // 导入参数保留原始精度，编辑控件按自身精度显示标定选项。
    const bool optionsRestored = project->resultImported
        || sameCalibrationOptions(project->options, currentOptions());
    if (project->result.has_value()
        && collectFiles(false).size() == project->imagePaths.size()
        && optionsRestored)
    {
        lastResult_ = *project->result;
        lastResultImported_ = project->resultImported;
        for (const ImageDetectionReport &report: lastResult_.imageReports)
        {
            updateImageReport(report);
        }
        updateUiState();
        onImageSelectionChanged();
    }
    lastDir_ = QFileInfo(path).absolutePath();
    currentProjectPath_ = QFileInfo(path).absoluteFilePath();
    setProjectModified(!project->resultWarning.isEmpty() || !optionsRestored);
    if (!project->resultWarning.isEmpty()
        || (project->result.has_value() && !lastResult_.success))
    {
        QMessageBox::warning(this, tr("项目结果已失效"),
            project->resultWarning.isEmpty()
                ? tr("部分图片无法加载或参数超出界面范围，已恢复输入，请重新标定。")
                : project->resultWarning);
    }
    statusBar()->showMessage(
        lastResult_.success ? tr("已恢复项目及标定结果：%1").arg(path)
                            : tr("已打开项目：%1").arg(path), 5000);
}

void MainWindow::onSaveProject()
{
    saveProject();
}

void MainWindow::onSaveProjectAs()
{
    saveProject(true);
}

bool MainWindow::saveProject(bool saveAs)
{
    if (isBusy())
    {
        return false;
    }
    QString path = currentProjectPath_;
    if (saveAs || path.isEmpty())
    {
        path = QFileDialog::getSaveFileName(
            this, tr("保存标定项目"),
            path.isEmpty() ? QDir(lastDir_).filePath(QStringLiteral("calibration.calibration.json")) : path,
            tr("标定项目 (*.calibration.json *.json)"));
    }
    if (path.isEmpty())
    {
        return false;
    }
    QString error;
    CalibrationProject project;
    project.options = lastResultImported_ ? lastResult_.options : currentOptions();
    project.imagePaths = collectFiles(false);
    for (int row = 0; row < imageList_->count(); ++row)
    {
        const QListWidgetItem *item = imageList_->item(row);
        if (item->checkState() != Qt::Checked)
        {
            project.disabledImagePaths << item->data(kRoleFilePath).toString();
        }
    }
    if (lastResult_.success)
    {
        project.result = lastResult_;
        project.resultImported = lastResultImported_;
    }
    if (!CalibrationProjectIo::save(path, project, &error))
    {
        QMessageBox::warning(this, tr("保存标定项目"),
            tr("保存失败：%1").arg(error));
        return false;
    }
    lastDir_ = QFileInfo(path).absolutePath();
    currentProjectPath_ = QFileInfo(path).absoluteFilePath();
    setProjectModified(false);
    statusBar()->showMessage(tr("项目已保存到：%1").arg(path), 5000);
    return true;
}

void MainWindow::onImportParameters()
{
    if (isBusy())
    {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("导入相机参数"), lastDir_, tr("YAML 文件 (*.yaml *.yml)"));
    if (path.isEmpty())
    {
        return;
    }
    QString error;
    const auto result = calibrator_.importParameters(path, &error);
    if (!result.has_value())
    {
        QMessageBox::warning(this, tr("导入相机参数"),
            tr("导入失败：%1").arg(error));
        return;
    }
    if (!confirmDiscardChanges())
    {
        return;
    }
    invalidateCalibrationResult();
    applyOptions(result->options);
    lastResult_ = *result;
    lastResultImported_ = true;
    markProjectModified();
    lastDir_ = QFileInfo(path).absolutePath();
    updateUiState();

    QStringList mismatches;
    for (const QString &file: collectFiles())
    {
        QImageReader reader(file);
        QSize size = reader.size();
        if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
        {
            size.transpose();
        }
        if (size.isValid() && size != result->imageSize)
        {
            mismatches << QFileInfo(file).fileName();
        }
    }
    if (!mismatches.isEmpty())
    {
        QMessageBox::warning(
            this, tr("分辨率不一致"),
            tr("已导入参数，但 %1 张图片与标定分辨率 %2×%3 不一致。"
                "这些图片不会应用去畸变。")
            .arg(mismatches.size())
            .arg(result->imageSize.width())
            .arg(result->imageSize.height()));
    }
    else
    {
        QMessageBox::information(this, tr("导入相机参数"), result->report);
    }
    onImageSelectionChanged();
}

void MainWindow::onCalibrate()
{
    startCalibration();
}

void MainWindow::onCompareAlgorithms()
{
    if (isBusy())
    {
        return;
    }
    const QStringList files = collectFiles();
    if (files.size() < 3)
    {
        QMessageBox::warning(this, tr("算法对比"), tr("至少需要 3 张启用的图片。"));
        return;
    }

    // 固定标定板配置，只展开相机模型和适用于当前板型的检测器组合
    const CalibrationOptions base = currentOptions();
    std::vector<CalibrationOptions> candidates;
    for (CameraModel model: {CameraModel::Pinhole, CameraModel::Fisheye})
    {
        CalibrationOptions options = base;
        options.cameraModel = model;
        options.radialCoeffs = model == CameraModel::Fisheye ? 4 : 3;
        if (base.boardType == CalibrationBoardType::Chessboard)
        {
            options.method = CalibrationMethod::Classic;
            candidates.push_back(options);
            options.method = CalibrationMethod::SectorBased;
            candidates.push_back(options);
        }
        else
        {
            candidates.push_back(options);
        }
    }

    auxiliaryProgressDialog_ = new QProgressDialog(
        tr("正在比较标定算法..."), tr("取消"), 0,
        static_cast<int>(candidates.size()), this);
    auxiliaryProgressDialog_->setWindowModality(Qt::WindowModal);
    auxiliaryProgressDialog_->setMinimumDuration(0);
    auxiliaryProgressDialog_->setValue(0);
    auxiliaryCanceled_.store(false);
    auxiliaryActive_ = true;
    updateUiState();
    connect(auxiliaryProgressDialog_, &QProgressDialog::canceled, this,
        [this] { auxiliaryCanceled_.store(true); });

    // watcher 留在 GUI 线程，完成回调集中恢复界面并释放进度窗口
    comparisonWatcher_ =
            new QFutureWatcher<std::vector<CalibrationResult>>(this);
    connect(comparisonWatcher_,
        &QFutureWatcher<std::vector<CalibrationResult>>::finished,
        this, [this, files, base]
        {
            const auto results = comparisonWatcher_->result();
            comparisonWatcher_->deleteLater();
            comparisonWatcher_ = nullptr;
            auxiliaryActive_ = false;
            if (auxiliaryProgressDialog_)
            {
                auxiliaryProgressDialog_->close();
                auxiliaryProgressDialog_->deleteLater();
                auxiliaryProgressDialog_ = nullptr;
            }
            updateUiState();
            if (results.empty())
            {
                statusBar()->showMessage(tr("算法对比已取消"), 5000);
                return;
            }
            if (files != collectFiles()
                || !sameCalibrationOptions(base, currentOptions()))
            {
                statusBar()->showMessage(tr("输入已变化，请重新运行算法对比。"), 5000);
                return;
            }
            // 模态窗口防止查看候选期间修改输入，保证应用的是同一组图片的结果。
            AlgorithmComparisonDialog dialog(results, this);
            connect(&dialog, &AlgorithmComparisonDialog::resultSelected,
                this, &MainWindow::onApplyComparisonResult);
            dialog.exec();
        });
    Calibrator *worker = &calibrator_;

    // 候选项串行求解，取消请求可在每个候选边界立即停止后续工作
    comparisonWatcher_->setFuture(QtConcurrent::run(
        [this, worker, files, candidates]
        {
            std::vector<CalibrationResult> results;
            results.reserve(candidates.size());
            for (size_t index = 0; index < candidates.size(); ++index)
            {
                if (auxiliaryCanceled_.load())
                {
                    break;
                }
                CalibrationResult result = worker->calibrate(
                    files, candidates[index], {},
                    [this] { return auxiliaryCanceled_.load(); });
                if (auxiliaryCanceled_.load())
                {
                    break;
                }
                results.push_back(std::move(result));
                QMetaObject::invokeMethod(
                    this, [this, index, total = candidates.size()]
                    {
                        if (auxiliaryProgressDialog_)
                        {
                            auxiliaryProgressDialog_->setLabelText(
                                tr("已完成 %1 / %2 个候选算法")
                                .arg(index + 1)
                                .arg(total));

                            // 到达最大值可能触发嵌套事件并完成任务，因此此调用必须位于最后
                            auxiliaryProgressDialog_->setValue(
                                static_cast<int>(index + 1));
                        }
                    }, Qt::QueuedConnection);
            }
            return results;
        }));
}

void MainWindow::startCalibration(
    std::optional<CalibrationComparison> comparison)
{
    if (isBusy())
    {
        // 单一 busy 状态保护 watcher 和进度窗口指针不被新任务覆盖
        return;
    }

    const QStringList files = collectFiles();
    if (files.size() < 3)
    {
        QMessageBox::warning(this, tr("标定"),
            tr("至少需要 3 张启用的图片。"));
        return;
    }

    const CalibrationOptions opts = currentOptions();
    markProjectModified();
    invalidateCalibrationResult();

    // 角点检测和非线性求解放入工作线程，进度窗口只在 GUI 线程更新
    progressDialog_ = new QProgressDialog(tr("正在检测角点..."),
        tr("取消"), 0, files.size(), this);
    progressDialog_->setWindowModality(Qt::WindowModal);
    progressDialog_->setMinimumDuration(0);
    progressDialog_->setAutoClose(false);
    progressDialog_->setAutoReset(false);
    progressDialog_->setValue(0);

    calibCanceled_.store(false);
    calibActive_ = true;
    updateUiState();
    connect(progressDialog_, &QProgressDialog::canceled, this,
        [this] { calibCanceled_.store(true); });

    // Calibrator 在工作线程触发此回调，所有 QWidget 操作必须排队回到 GUI 线程
    auto progressCb = [this](int processed, int total, const ImageDetectionReport &report)
    {
        QMetaObject::invokeMethod(
            this,
            [this, processed, total, report]
            {
                if (progressDialog_)
                {
                    progressDialog_->setMaximum(total);
                    progressDialog_->setLabelText(
                        tr("正在处理 %1 / %2：%3").arg(processed).arg(total)
                            .arg(QFileInfo(report.imagePath).fileName()));

                    // setValue 可能让 finished 回调删除进度窗口，之后不得再解引用该指针
                    progressDialog_->setValue(processed);
                }
                updateImageReport(report);
            },
            Qt::QueuedConnection);
    };
    auto cancelCb = [this] { return calibCanceled_.load(); };

    calibWatcher_ = new QFutureWatcher<CalibrationResult>(this);
    connect(calibWatcher_, &QFutureWatcher<CalibrationResult>::finished, this,
        [this, files, opts, comparison]
        {
            CalibrationResult result = calibWatcher_->result();
            calibWatcher_->deleteLater();
            calibWatcher_ = nullptr;
            calibActive_ = false;
            if (progressDialog_)
            {
                progressDialog_->close();
                progressDialog_->deleteLater();
                progressDialog_ = nullptr;
            }

            if (!sameCalibrationOptions(opts, currentOptions())
                || files != collectFiles())
            {
                // 输入快照已过期时拒绝发布结果，避免导出与当前项目不匹配的参数
                result = {};
                result.report = tr(
                    "标定期间参数或图片列表发生了变化，结果已丢弃。请重新标定。");
            }
            if (comparison.has_value())
            {
                if (result.success)
                {
                    const double rmsChange =
                            result.rmsError - comparison->baselineRms;
                    result.report += tr(
                                "\n\n排除 %1 张图片后重新标定：RMS %2 → %3 px（%4 %5 px）。")
                            .arg(comparison->excludedImages)
                            .arg(comparison->baselineRms,
                                0, 'f', 3)
                            .arg(result.rmsError, 0, 'f', 3)
                            .arg(rmsChange <= 0.0 ? tr("下降") : tr("上升"))
                            .arg(std::abs(rmsChange),
                                0, 'f', 3);
                }
                else
                {
                    result.report += tr(
                                "\n\n已排除 %1 张图片，但重新标定未成功，无法比较 RMS。")
                            .arg(comparison->excludedImages);
                }
            }
            presentResult(result);
            refreshImageFilter();
        });

    // calibrate 不读写预览缓存，可与 GUI 线程中的预览操作分离执行
    Calibrator *worker = &calibrator_;
    calibWatcher_->setFuture(QtConcurrent::run(
        [worker, files, opts, progressCb, cancelCb]()
        {
            return worker->calibrate(files, opts, progressCb, cancelCb);
        }));
}

void MainWindow::presentResult(const CalibrationResult &result)
{
    lastResult_ = result;
    lastResultImported_ = false;
    for (const ImageDetectionReport &report: result.imageReports)
    {
        updateImageReport(report);
    }
    markProjectModified();
    updateUiState();
    if (result.success)
    {
        if (result.qualityWarning)
        {
            QMessageBox::warning(this, tr("标定结果质量警告"),
                result.report);
        }
        else
        {
            QMessageBox::information(this, tr("标定结果"),
                result.report);
        }
    }
    else
    {
        QMessageBox::warning(this, tr("标定"), result.report);
    }
    onImageSelectionChanged();
}

void MainWindow::onApplyComparisonResult(const CalibrationResult &result)
{
    if (isBusy() || !result.success)
    {
        return;
    }
    // 先关闭旧位姿窗口，再切换配置和结果，后续预览与导出共用这一份参数。
    invalidateCalibrationResult();
    applyOptions(result.options);
    presentResult(result);
    statusBar()->showMessage(tr("已应用选中算法的标定结果，无需重新求解。"), 5000);
}

void MainWindow::onShowPoses()
{
    if (!lastResult_.success || lastResult_.poses.empty())
    {
        QMessageBox::warning(this, tr("位姿结果"),
            tr("请先完成一次成功的相机标定。"));
        return;
    }

    if (poseDialog_)
    {
        poseDialog_->raise();
        poseDialog_->activateWindow();
        return;
    }

    auto *dialog = new PoseResultDialog(lastResult_, this, !lastResultImported_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &PoseResultDialog::excludeImagesRequested,
        this, &MainWindow::onExcludeImagesAndRecalibrate);
    poseDialog_ = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::onShowDiagnostics()
{
    if (isBusy() || !diagnosticsAction_->isEnabled())
    {
        return;
    }
    const auto items = imageList_->selectedItems();
    const QString selectedPath = items.isEmpty()
            ? QString{} : items.first()->data(kRoleFilePath).toString();
    CalibrationDiagnosticsDialog dialog(lastResult_, selectedPath, this);
    dialog.exec();
}

void MainWindow::onShowUndistortionComparison()
{
    if (isBusy() || !lastResult_.success)
    {
        return;
    }
    const auto items = imageList_->selectedItems();
    if (items.isEmpty())
    {
        statusBar()->showMessage(tr("请先选择一张图片。"), 5000);
        return;
    }
    const QString path = items.first()->data(kRoleFilePath).toString();
    const UndistortionPreview preview = calibrator_.undistortionPreview(path, lastResult_);
    if (!preview.error.isEmpty() || preview.original.isNull() || preview.undistorted.isNull())
    {
        QMessageBox::warning(this, tr("去畸变对比"),
            preview.error.isEmpty() ? tr("无法生成这张图片的去畸变预览。") : preview.error);
        return;
    }
    ImageComparisonDialog dialog(preview.original, preview.undistorted, this);
    dialog.setWindowTitle(tr("去畸变对比 — %1").arg(QFileInfo(path).fileName()));
    dialog.exec();
}

void MainWindow::onExcludeImagesAndRecalibrate(
    const QStringList &imagePaths, double baselineRms)
{
    if (isBusy() || !lastResult_.success || lastResultImported_ || imagePaths.isEmpty())
    {
        return;
    }

    const QSet<QString> excludedPaths(imagePaths.begin(), imagePaths.end());
    int matchedCount = 0;
    for (int row = 0; row < imageList_->count(); ++row)
    {
        const QListWidgetItem *item = imageList_->item(row);
        if (item != nullptr && item->checkState() == Qt::Checked
            && excludedPaths.contains(
                item->data(kRoleFilePath).toString()))
        {
            ++matchedCount;
        }
    }
    if (matchedCount == 0)
    {
        return;
    }
    if (collectFiles().size() - matchedCount < 3)
    {
        QMessageBox::warning(
            this, tr("排除图片"),
            tr("排除后少于 3 张图片，无法重新标定。"));
        return;
    }

    setImageParticipation(imagePaths, false);
    currentPixmap_ = {};
    imageDisplay_->clear();
    statusBar()->showMessage(
        tr("已排除 %1 张图片，正在重新标定").arg(matchedCount),
        5000);
    startCalibration(CalibrationComparison{baselineRms, matchedCount});
}

void MainWindow::onExportParameters()
{
    if (!lastResult_.success)
    {
        QMessageBox::warning(this, tr("导出相机参数"),
            tr("请先完成一次成功的相机标定。"));
        return;
    }

    const QString defaultName =
            QDir(lastResult_.sourceDirectory.isEmpty() ?
                QStandardPaths::writableLocation(
                    QStandardPaths::HomeLocation) :
                lastResult_.sourceDirectory)
            .filePath(QStringLiteral("camera_parameters.yaml"));
    const QString outputPath = QFileDialog::getSaveFileName(
        this, tr("导出相机参数"), defaultName,
        tr("YAML 文件 (*.yaml *.yml)"));
    if (outputPath.isEmpty())
    {
        return;
    }

    QString error;
    if (!calibrator_.exportParameters(outputPath, lastResult_, &error))
    {
        QMessageBox::warning(this, tr("导出相机参数"),
            tr("导出失败：%1").arg(error));
        return;
    }

    QMessageBox::information(
        this, tr("导出相机参数"),
        tr("相机参数已保存到：\n%1").arg(outputPath));
}

void MainWindow::onExportUndistortedImages()
{
    if (isBusy() || !lastResult_.success)
    {
        return;
    }
    const QString outputDirectory = QFileDialog::getExistingDirectory(
        this, tr("选择去畸变图片输出文件夹"), lastDir_);
    if (outputDirectory.isEmpty())
    {
        return;
    }
    const QStringList files = collectFiles();
    if (files.isEmpty())
    {
        return;
    }

    auxiliaryProgressDialog_ = new QProgressDialog(
        tr("正在批量导出去畸变图片..."), tr("取消"), 0, files.size(), this);
    auxiliaryProgressDialog_->setWindowModality(Qt::WindowModal);
    auxiliaryProgressDialog_->setMinimumDuration(0);
    auxiliaryProgressDialog_->setValue(0);
    auxiliaryCanceled_.store(false);
    auxiliaryActive_ = true;
    updateUiState();
    connect(auxiliaryProgressDialog_, &QProgressDialog::canceled, this,
        [this] { auxiliaryCanceled_.store(true); });

    // 后台批处理使用启动时的参数副本，不受后续窗口状态影响
    const CalibrationResult calibration = lastResult_;

    batchWatcher_ = new QFutureWatcher<BatchUndistortSummary>(this);
    connect(batchWatcher_, &QFutureWatcher<BatchUndistortSummary>::finished,
        this, [this, outputDirectory]
        {
            const BatchUndistortSummary summary = batchWatcher_->result();
            batchWatcher_->deleteLater();
            batchWatcher_ = nullptr;
            auxiliaryActive_ = false;
            if (auxiliaryProgressDialog_)
            {
                auxiliaryProgressDialog_->close();
                auxiliaryProgressDialog_->deleteLater();
                auxiliaryProgressDialog_ = nullptr;
            }
            updateUiState();
            QString message = summary.canceled ? tr("批量导出已取消。\n") : QString();
            message += tr("已导出 %1 张，跳过 %2 张。\n输出目录：%3")
                    .arg(summary.written)
                    .arg(summary.skipped)
                    .arg(outputDirectory);
            if (!summary.errors.isEmpty())
            {
                message += tr("\n\n前几项错误：\n%1")
                        .arg(summary.errors.mid(0, 5).join('\n'));
            }
            if (summary.skipped > 0)
            {
                QMessageBox::warning(this, tr("批量导出去畸变图片"), message);
            }
            else
            {
                QMessageBox::information(this, tr("批量导出去畸变图片"), message);
            }
        });
    Calibrator *worker = &calibrator_;
    batchWatcher_->setFuture(QtConcurrent::run(
        [this, worker, files, outputDirectory, calibration]
        {
            BatchUndistortSummary summary;
            const QDir outputDir(outputDirectory);
            const QSet<QString> inputPaths(files.cbegin(), files.cend());
            QTemporaryDir staging(outputDir.filePath(QStringLiteral(".undistort-XXXXXX")));
            if (!staging.isValid())
            {
                summary.skipped = files.size();
                summary.errors << tr("无法在输出目录创建临时文件夹：%1").arg(staging.errorString());
                return summary;
            }
            for (int index = 0; index < files.size(); ++index)
            {
                if (auxiliaryCanceled_.load())
                {
                    summary.canceled = true;
                    break;
                }
                const QFileInfo inputInfo(files[index]);
                const QString base = inputInfo.completeBaseName() + QStringLiteral("_undistorted");
                QString suffix = inputInfo.suffix().toLower();
                if (suffix.isEmpty())
                {
                    suffix = QStringLiteral("png");
                }

                QString error;
                QFile staged(staging.filePath(QStringLiteral("image.") + suffix));
                if (worker->undistortImageFile(files[index], staged.fileName(), calibration, &error))
                {
                    // 编码完成后才发布文件；QFile::rename 遇到已有目标时保留原文件。
                    QString name = base;
                    for (int number = 2; ; ++number)
                    {
                        const QString target = outputDir.filePath(name + QLatin1Char('.') + suffix);
                        if (!inputPaths.contains(target))
                        {
                            if (staged.rename(target))
                            {
                                ++summary.written;
                                break;
                            }
                            const QFileInfo occupied(target);
                            if (!occupied.exists() && !occupied.isSymLink())
                            {
                                error = staged.errorString();
                                break;
                            }
                        }
                        name = base + QStringLiteral("_%1").arg(number);
                    }
                }
                if (!error.isEmpty())
                {
                    ++summary.skipped;
                    summary.errors << tr("%1：%2")
                            .arg(inputInfo.fileName(), error);
                }
                QMetaObject::invokeMethod(
                    this, [this, value = index + 1, total = files.size()]
                    {
                        if (auxiliaryProgressDialog_)
                        {
                            auxiliaryProgressDialog_->setLabelText(
                                tr("正在批量导出... %1 / %2")
                                .arg(value)
                                .arg(total));

                            // 完成值会驱动模态事件处理，保持为本分支最后一次窗口访问
                            auxiliaryProgressDialog_->setValue(value);
                        }
                    }, Qt::QueuedConnection);
            }
            return summary;
        }));
}

void MainWindow::onClearAll()
{
    if (isBusy())
    {
        return;
    }
    if (!confirmDiscardChanges())
    {
        return;
    }
    const QScopedValueRollback<bool> restoring(restoringProject_, true);
    imageList_->clear();
    currentPixmap_ = {};
    imageDisplay_->clear();
    invalidateCalibrationResult();
    currentProjectPath_.clear();
    setProjectModified(false);
}

void MainWindow::onImageSelectionChanged()
{
    const auto items = imageList_->selectedItems();
    if (items.isEmpty())
    {
        currentPixmap_ = {};
        updateUiState();
        return;
    }
    auto *item = items.first();
    showImage(item->data(kRoleFilePath).toString());
    updateUiState();
}

void MainWindow::onImageParticipationChanged(QListWidgetItem *item)
{
    const bool enabled = item->checkState() == Qt::Checked;
    if (item->data(kRoleEnabled).toBool() == enabled)
    {
        return;
    }
    const QSignalBlocker blocker(imageList_);
    if (isBusy())
    {
        item->setCheckState(item->data(kRoleEnabled).toBool()
                ? Qt::Checked : Qt::Unchecked);
        return;
    }
    item->setData(kRoleEnabled, enabled);
    QFont font = item->font();
    font.setItalic(!enabled);
    item->setFont(font);
    markProjectModified();
    invalidateCalibrationResult();
    refreshImageFilter();
    onDisplayModeChanged();
}

void MainWindow::setImageParticipation(const QStringList &paths, bool enabled)
{
    if (isBusy())
    {
        return;
    }
    const QSet<QString> selected(paths.begin(), paths.end());
    bool changed = false;
    {
        const QSignalBlocker blocker(imageList_);
        for (int row = 0; row < imageList_->count(); ++row)
        {
            auto *item = imageList_->item(row);
            if (selected.contains(item->data(kRoleFilePath).toString())
                && item->data(kRoleEnabled).toBool() != enabled)
            {
                item->setData(kRoleEnabled, enabled);
                item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
                QFont font = item->font();
                font.setItalic(!enabled);
                item->setFont(font);
                changed = true;
            }
        }
    }
    if (changed)
    {
        markProjectModified();
        invalidateCalibrationResult();
        refreshImageFilter();
        onDisplayModeChanged();
    }
}

void MainWindow::refreshImageFilter()
{
    const int filter = imageFilterCombo_->currentData().toInt();
    for (int row = 0; row < imageList_->count(); ++row)
    {
        auto *item = imageList_->item(row);
        const bool enabled = item->checkState() == Qt::Checked;
        const auto status = static_cast<ImageDetectionStatus>(item->data(kRoleDetection).toInt());
        const bool failed = status != ImageDetectionStatus::NotProcessed
                && status != ImageDetectionStatus::Success;
        const bool visible = filter == AllImages
                || (filter == EnabledImages && enabled)
                || (filter == DisabledImages && !enabled)
                || (filter == QualityWarnings
                    && !item->data(kRoleQualityWarnings).toStringList().isEmpty())
                || (filter == DetectionFailures && failed)
                || (filter == ReadFailures && status == ImageDetectionStatus::ReadFailed)
                || (filter == SizeMismatches && status == ImageDetectionStatus::SizeMismatch)
                || (filter == MissingBoards && status == ImageDetectionStatus::BoardNotFound)
                || (filter == InsufficientCorners && status == ImageDetectionStatus::InsufficientCorners);
        item->setHidden(!visible);
    }
}

void MainWindow::updateImageReport(const ImageDetectionReport &report)
{
    const QSignalBlocker blocker(imageList_);
    for (int row = 0; row < imageList_->count(); ++row)
    {
        auto *item = imageList_->item(row);
        if (item->data(kRoleFilePath).toString() != report.imagePath)
        {
            continue;
        }
        const bool failed = report.status != ImageDetectionStatus::NotProcessed
                && report.status != ImageDetectionStatus::Success;
        const QString status = imageDetectionStatusText(report.status);
        item->setData(kRoleDetection, static_cast<int>(report.status));
        item->setData(kRoleDetectionMessage, report.message);
        const QString baseText = item->data(kRoleBaseText).toString();
        item->setText(failed ? baseText + QStringLiteral("\n") + status : baseText);
        item->setToolTip(item->data(kRoleBaseToolTip).toString()
            + tr("\n检测状态：%1").arg(status)
            + (report.message.isEmpty() ? QString{} : QStringLiteral("\n") + report.message));
        item->setForeground(failed ? Theme::kDanger
            : report.status == ImageDetectionStatus::Success ? Theme::kSuccess
            : palette().color(QPalette::Text));
        break;
    }
}

void MainWindow::onCameraModelChanged()
{
    const auto model = static_cast<CameraModel>(cameraModelCombo_->currentData().toInt());
    const bool isFisheye = model == CameraModel::Fisheye;
    skewCheck_->setVisible(isFisheye);
    tangentialCheck_->setVisible(!isFisheye);
    radialCoeffSpin_->setRange(2, isFisheye ? 4 : 3);
    radialCoeffSpin_->setValue(isFisheye ? 4 : 3);
    onCalibrationOptionsChanged();
}

void MainWindow::onBoardTypeChanged()
{
    const bool isCharuco =
            boardTypeCombo_->currentData().toInt()
            == static_cast<int>(CalibrationBoardType::Charuco);
    const int minimumSquares = isCharuco ? 2 : 4;
    boardColsSpin_->setMinimum(minimumSquares);
    boardRowsSpin_->setMinimum(minimumSquares);
    ui_->markerSizeLabel->setVisible(isCharuco);
    markerSizeSpin_->setVisible(isCharuco);
    ui_->dictionaryLabel->setVisible(isCharuco);
    dictionaryCombo_->setVisible(isCharuco);
    ui_->methodLabel->setVisible(!isCharuco);
    methodCombo_->setVisible(!isCharuco);
    onCalibrationOptionsChanged();
}

void MainWindow::onCalibrationOptionsChanged()
{
    markProjectModified();
    {
        const QSignalBlocker blocker(imageList_);
        for (int row = 0; row < imageList_->count(); ++row)
        {
            auto *item = imageList_->item(row);
            // 读取失败不依赖标定选项，其余旧检测结论需要重新计算。
            if (static_cast<ImageDetectionStatus>(item->data(kRoleDetection).toInt())
                != ImageDetectionStatus::ReadFailed)
            {
                ImageDetectionReport report;
                report.imagePath = item->data(kRoleFilePath).toString();
                updateImageReport(report);
            }
        }
    }
    invalidateCalibrationResult();
    refreshImageFilter();
    onDisplayModeChanged();
}

void MainWindow::onDisplayModeChanged()
{
    debounceTimer_->start();
}

void MainWindow::loadImages(const QStringList &files)
{
    // 路径级去重在解码前完成，避免重复读取同一文件
    QSet<QString> loaded;
    loaded.reserve(imageList_->count() + files.size());
    for (int i = 0; i < imageList_->count(); ++i)
    {
        if (const QListWidgetItem *item = imageList_->item(i))
        {
            loaded.insert(item->data(kRoleFilePath).toString());
        }
    }

    int added = 0;
    int duplicates = 0;
    int failures = 0;
    int qualityWarnings = 0;

    // 感知哈希用于发现路径不同但画面近似的输入，仅生成提示而不自动排除
    QSet<quint64> loadedHashes;
    for (int i = 0; i < imageList_->count(); ++i)
    {
        if (const QListWidgetItem *item = imageList_->item(i))
        {
            if (item->data(kRoleImageHash).isValid())
            {
                loadedHashes.insert(item->data(kRoleImageHash).toULongLong());
            }
        }
    }
    for (const QString &inputPath: files)
    {
        const QString file = QDir::cleanPath(QFileInfo(inputPath).absoluteFilePath());
        if (loaded.contains(file))
        {
            ++duplicates;
            continue;
        }
        loaded.insert(file);

        QImageReader reader(file);
        reader.setAutoTransform(true);
        const QSize decodedSize = reader.size().scaled(
            kThumbnailSize, kThumbnailSize, Qt::KeepAspectRatio);
        if (decodedSize.isValid())
        {
            reader.setScaledSize(decodedSize);
        }
        const QImage decoded = reader.read();
        if (decoded.isNull())
        {
            ++failures;
            auto *item = new QListWidgetItem(appIcon("warning"),
                QFileInfo(file).fileName());
            item->setData(kRoleFilePath, file);
            item->setData(kRoleBaseText, QFileInfo(file).fileName());
            item->setData(kRoleBaseToolTip, file);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
            item->setData(kRoleEnabled, true);
            imageList_->addItem(item);
            ImageDetectionReport report;
            report.imagePath = file;
            report.status = ImageDetectionStatus::ReadFailed;
            report.message = tr("无法读取图片：%1").arg(reader.errorString());
            updateImageReport(report);
            ++added;
            continue;
        }

        const QImage thumb = decoded.scaled(
            kThumbnailSize, kThumbnailSize, Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
        auto *item = new QListWidgetItem(
            QIcon(QPixmap::fromImage(thumb)), QFileInfo(file).fileName());
        item->setData(kRoleFilePath, file);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(kRoleEnabled, true);
        const ImageQualityResult quality = analyzeImageQuality(file);
        QStringList warnings = quality.warnings;

        // 64 位哈希的汉明距离不超过 3 时视为高度相似
        const bool similarImage = std::any_of(
            loadedHashes.cbegin(), loadedHashes.cend(),
            [&quality](quint64 existingHash)
            {
                return std::bitset < 64 > (
                            existingHash ^ quality.similarityHash)
                        .count()
                        <= 3;
            });
        if (similarImage)
        {
            warnings << tr("与已加载图片画面高度相似");
        }
        loadedHashes.insert(quality.similarityHash);
        item->setData(kRoleImageHash,
            QVariant::fromValue<qulonglong>(quality.similarityHash));
        item->setData(kRoleQualityWarnings, warnings);
        QString toolTip = file;
        if (!warnings.isEmpty())
        {
            ++qualityWarnings;
            item->setBackground(Theme::kWarningBackground);
            item->setText(item->text() + QStringLiteral(" ⚠"));
            toolTip += tr("\n质量预检：%1\n清晰度：%2，平均亮度：%3")
                    .arg(warnings.join(QStringLiteral("；")))
                    .arg(quality.sharpness, 0, 'f', 1)
                    .arg(quality.meanBrightness, 0, 'f', 1);
        }
        item->setToolTip(toolTip);
        item->setData(kRoleBaseToolTip, toolTip);
        item->setData(kRoleBaseText, item->text());
        imageList_->addItem(item);
        ++added;
    }

    if (failures > 0)
    {
        statusBar()->showMessage(
            tr("%1 张图片加载失败").arg(failures), 5000);
    }

    if (added > 0)
    {
        markProjectModified();
        if (lastResultImported_)
        {
            updateUiState();
        }
        else
        {
            invalidateCalibrationResult();
        }
        if (failures > 0)
        {
            statusBar()->showMessage(
                tr("已加入 %1 张图片，其中 %2 张读取失败；失败项已保留，可查看原因或停用。")
                    .arg(added).arg(failures), 8000);
        }
        else if (qualityWarnings > 0)
        {
            statusBar()->showMessage(
                tr("已添加 %1 张图片，其中 %2 张有质量提示；鼠标悬停查看详情。")
                .arg(added)
                .arg(qualityWarnings),
                8000);
        }
        else if (duplicates > 0)
        {
            statusBar()->showMessage(
                tr("已忽略 %1 个重复路径").arg(duplicates), 5000);
        }
    }
    else
    {
        updateUiState();
    }
    refreshImageFilter();
}

void MainWindow::showImage(const QString &filePath, bool *found)
{
    bool detected = false;
    ImageDetectionReport report;
    const bool applyUndistortion = showUndistortedCheck_->isChecked();
    const QImage annotated = calibrator_.previewImage(
        filePath, currentOptions(), lastResult_,
        applyUndistortion, &detected, &report);
    if (applyUndistortion && report.status == ImageDetectionStatus::SizeMismatch)
    {
        statusBar()->showMessage(
            tr("当前图片为 %1×%2，标定参数适用于 %3×%4；未应用去畸变。")
            .arg(report.actualSize.width())
            .arg(report.actualSize.height())
            .arg(report.expectedSize.width())
            .arg(report.expectedSize.height()),
            8000);
    }
    currentPixmap_ = QPixmap::fromImage(annotated);
    fitImageToView();
    if (!isBusy())
    {
        updateImageReport(report);
    }
    if (annotated.isNull() && !report.message.isEmpty())
    {
        imageDisplay_->setText(report.message);
    }

    if (found)
    {
        *found = detected;
    }
}

void MainWindow::applyOptions(const CalibrationOptions &options)
{
    setComboValue(cameraModelCombo_, static_cast<int>(options.cameraModel));
    setComboValue(boardTypeCombo_, static_cast<int>(options.boardType));
    boardColsSpin_->setValue(options.boardSize.width());
    boardRowsSpin_->setValue(options.boardSize.height());
    squareSizeSpin_->setValue(options.squareSize);
    markerSizeSpin_->setMaximum(qMax(0.001, options.squareSize - 0.001));
    markerSizeSpin_->setValue(options.markerSize);
    setComboValue(dictionaryCombo_, static_cast<int>(options.dictionary));
    setComboValue(methodCombo_, static_cast<int>(options.method));
    skewCheck_->setChecked(options.skew);
    tangentialCheck_->setChecked(options.tangential);
    radialCoeffSpin_->setValue(options.radialCoeffs);
}

CalibrationOptions MainWindow::currentOptions() const
{
    CalibrationOptions opts;
    opts.cameraModel = static_cast<CameraModel>(
        cameraModelCombo_->currentData().toInt());
    opts.boardType = static_cast<CalibrationBoardType>(
        boardTypeCombo_->currentData().toInt());
    opts.boardSize = {boardColsSpin_->value(), boardRowsSpin_->value()};
    opts.squareSize = squareSizeSpin_->value();
    opts.markerSize = markerSizeSpin_->value();
    opts.dictionary = static_cast<ArucoDictionary>(
        dictionaryCombo_->currentData().toInt());
    opts.method = static_cast<CalibrationMethod>(
        methodCombo_->currentData().toInt());
    opts.skew = skewCheck_->isChecked();
    opts.tangential = tangentialCheck_->isChecked();
    opts.radialCoeffs = radialCoeffSpin_->value();
    return opts;
}

QStringList MainWindow::collectFiles(bool enabledOnly) const
{
    QStringList files;
    files.reserve(imageList_->count());
    for (int i = 0; i < imageList_->count(); ++i)
    {
        if (const QListWidgetItem *item = imageList_->item(i))
        {
            if (!enabledOnly || item->checkState() == Qt::Checked)
            {
                files << item->data(kRoleFilePath).toString();
            }
        }
    }
    return files;
}

void MainWindow::fitImageToView()
{
    if (currentPixmap_.isNull())
    {
        imageDisplay_->clear();
        const bool hasImages =
                imageList_ != nullptr && imageList_->count() > 0;
        imageDisplay_->setText(
            hasImages ? tr("选择左侧图片查看预览") : tr("添加图片或文件夹以开始标定"));
        return;
    }
    const QSize area =
            (imageScroll_->viewport()->size() - QSize(20, 20)).expandedTo({1, 1});
    imageDisplay_->setPixmap(
        currentPixmap_.scaled(area, Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    fitImageToView();
}
