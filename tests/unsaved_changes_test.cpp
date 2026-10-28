#include "calibration_project.h"
#include "mainwindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

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

    template<typename Object>
    Object *child(QObject &parent, const char *name)
    {
        auto *object = parent.findChild<Object *>(QString::fromLatin1(name));
        if (object == nullptr)
        {
            throw std::runtime_error(std::string("测试对象不存在：") + name);
        }
        return object;
    }

    enum class FileReply { Forbidden, ChoosePath, Cancel };

    // 用真实模态窗口驱动用户决策；不依赖界面语言对应的按钮文字。
    class DialogDriver
    {
    public:

        explicit DialogDriver(QMessageBox::StandardButton decision = QMessageBox::Cancel,
            FileReply fileReply = FileReply::Forbidden, const QString &path = {})
            : decision_(decision), fileReply_(fileReply), path_(path)
        {
            QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] { handleDialog(); });
            timer_.start(5);
        }

        void verify(int unsavedPrompts, int fileDialogs, int minimumOtherMessages = 0)
        {
            timer_.stop();
            if (!error_.isEmpty())
            {
                throw std::runtime_error(error_.toStdString());
            }
            require(unsavedCount_ == unsavedPrompts, "未保存提醒出现次数不符合预期");
            require(fileCount_ == fileDialogs, "文件选择窗口出现次数不符合预期");
            require(otherMessageCount_ >= minimumOtherMessages, "缺少保存结果或错误通知");
        }

    private:

        void markHandled(QWidget *modal)
        {
            handled_.insert(modal);
            QObject::connect(modal, &QObject::destroyed, &timer_,
                [this, modal] { handled_.remove(modal); });
        }

        void handleDialog()
        {
            QWidget *modal = QApplication::activeModalWidget();
            if (modal == nullptr || handled_.contains(modal))
            {
                return;
            }
            if (auto *message = qobject_cast<QMessageBox *>(modal))
            {
                markHandled(modal);
                if (message->objectName() == QStringLiteral("unsavedChangesDialog"))
                {
                    ++unsavedCount_;
                    auto *save = message->button(QMessageBox::Save);
                    auto *discard = message->button(QMessageBox::Discard);
                    auto *cancel = message->button(QMessageBox::Cancel);
                    if (save == nullptr || discard == nullptr || cancel == nullptr
                        || message->defaultButton() != save || message->escapeButton() != cancel)
                    {
                        error_ = QStringLiteral("未保存提醒缺少标准按钮、默认保存或 Escape 取消配置");
                        message->reject();
                        return;
                    }
                    auto *button = message->button(decision_);
                    if (button == nullptr)
                    {
                        error_ = QStringLiteral("找不到指定的未保存决策按钮");
                        message->reject();
                        return;
                    }
                    button->click();
                }
                else
                {
                    ++otherMessageCount_;
                    message->accept();
                }
            }
            else if (auto *file = qobject_cast<QFileDialog *>(modal))
            {
                markHandled(modal);
                ++fileCount_;
                if (fileReply_ == FileReply::Forbidden)
                {
                    error_ = QStringLiteral("操作意外弹出文件选择窗口");
                    file->reject();
                    return;
                }
                if (fileReply_ == FileReply::Cancel)
                {
                    file->reject();
                    return;
                }
                file->setDirectory(QFileInfo(path_).absolutePath());
                file->selectFile(QFileInfo(path_).fileName());
                auto *name = file->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
                if (name == nullptr)
                {
                    error_ = QStringLiteral("非原生文件选择窗口缺少文件名输入框");
                    file->reject();
                    return;
                }
                name->setText(path_);
                if (!QMetaObject::invokeMethod(file, "accept", Qt::QueuedConnection))
                {
                    error_ = QStringLiteral("无法确认项目文件路径");
                    file->reject();
                }
            }
        }

        QMessageBox::StandardButton decision_;
        FileReply fileReply_;
        QString path_;
        QTimer timer_;
        QSet<QWidget *> handled_;
        QString error_;
        int unsavedCount_ = 0;
        int fileCount_ = 0;
        int otherMessageCount_ = 0;
    };

    QString writeImage(const QTemporaryDir &directory)
    {
        QImage image(320, 240, QImage::Format_RGB32);
        image.fill(Qt::gray);
        const QString path = directory.filePath(QStringLiteral("source.png"));
        require(image.save(path), "无法创建测试图片");
        return path;
    }

    void writeProject(const QString &projectPath, const QString &imagePath)
    {
        CalibrationProject project;
        project.options.boardType = CalibrationBoardType::Chessboard;
        project.options.boardSize = {5, 4};
        project.imagePaths = {imagePath};
        QString error;
        require(CalibrationProjectIo::save(projectPath, project, &error),
            "无法准备干净项目文件");
    }

    void openProject(MainWindow &window, const QString &path)
    {
        DialogDriver driver(QMessageBox::Cancel, FileReply::ChoosePath, path);
        child<QAction>(window, "openProjectAction")->trigger();
        driver.verify(0, 1);
        require(!window.isWindowModified(), "打开项目完成后仍被标记为未保存");
        require(child<QListWidget>(window, "imageList")->count() == 1,
            "干净项目未正确载入图片");
    }

    void testClearDecisions(const QString &imagePath, const QString &savePath)
    {
        MainWindow window;
        require(!window.isWindowModified(), "新窗口不应处于未保存状态");
        window.loadImages({imagePath});
        auto *list = child<QListWidget>(window, "imageList");
        auto *clear = child<QAction>(window, "clearAction");
        require(list->count() == 1 && window.isWindowModified(),
            "添加图片未标记项目变化");
        {
            DialogDriver driver(QMessageBox::Cancel);
            clear->trigger();
            driver.verify(1, 0);
        }
        require(list->count() == 1 && window.isWindowModified(),
            "取消清空后图片或未保存状态丢失");
        {
            DialogDriver driver(QMessageBox::Discard);
            clear->trigger();
            driver.verify(1, 0);
        }
        require(list->count() == 0 && !window.isWindowModified(),
            "放弃保存后未清空或未清除变化状态");

        window.loadImages({imagePath});
        {
            DialogDriver driver(QMessageBox::Save, FileReply::ChoosePath, savePath);
            clear->trigger();
            driver.verify(1, 1);
        }
        require(list->count() == 0 && !window.isWindowModified(),
            "保存后未继续清空项目");
        QString error;
        const auto saved = CalibrationProjectIo::load(savePath, &error);
        require(saved.has_value() && saved->imagePaths == QStringList{imagePath},
            "保存并清空未保留清空前的图片列表");
    }

    void testCancelSaveAndClose(const QString &imagePath)
    {
        MainWindow window;
        window.show();
        QApplication::processEvents();
        window.loadImages({imagePath});
        auto *list = child<QListWidget>(window, "imageList");
        {
            DialogDriver driver(QMessageBox::Save, FileReply::Cancel);
            child<QAction>(window, "clearAction")->trigger();
            driver.verify(1, 1);
        }
        require(list->count() == 1 && window.isWindowModified(),
            "取消保存路径后仍清空项目或错误清除了变化状态");

        QCloseEvent event;
        {
            DialogDriver driver(QMessageBox::Cancel);
            QApplication::sendEvent(&window, &event);
            driver.verify(1, 0);
        }
        require(!event.isAccepted() && window.isVisible()
                && list->count() == 1 && window.isWindowModified(),
            "取消关闭后窗口、图片或未保存状态未保留");
    }

    void testCleanOperationsAndSaveReuse(const QString &imagePath,
        const QString &projectPath)
    {
        writeProject(projectPath, imagePath);
        MainWindow window;
        openProject(window, projectPath);
        require(child<QAction>(window, "saveProjectAsAction") != nullptr,
            "缺少另存为项目动作");
        auto *list = child<QListWidget>(window, "imageList");
        auto *filter = child<QComboBox>(window, "imageFilterCombo");
        list->setCurrentRow(0);
        require(QMetaObject::invokeMethod(&window, "onImageSelectionChanged",
                    Qt::DirectConnection), "无法刷新项目预览");
        filter->setCurrentIndex(filter->findData(3));
        filter->setCurrentIndex(filter->findData(0));
        require(!window.isWindowModified(), "图片预览或筛选不应标记项目变化");

        list->item(0)->setCheckState(Qt::Unchecked);
        require(window.isWindowModified(), "停用图片未标记项目变化");
        {
            DialogDriver driver;
            child<QAction>(window, "saveProjectAction")->trigger();
            driver.verify(0, 0);
        }
        require(!window.isWindowModified(), "保存成功后未清除变化状态");
        const auto saved = CalibrationProjectIo::load(projectPath);
        require(saved.has_value()
                && saved->disabledImagePaths == QStringList{imagePath},
            "保存动作未复用原项目路径或未保存图片停用状态");

        auto *columns = child<QSpinBox>(window, "boardColsSpin");
        const int originalColumns = columns->value();
        columns->setValue(originalColumns + 1);
        require(window.isWindowModified(), "修改标定参数未标记项目变化");
        columns->setValue(originalColumns);
        require(window.isWindowModified(), "改回参数后不应静默丢弃未保存提醒");
        {
            DialogDriver driver(QMessageBox::Cancel);
            child<QAction>(window, "clearAction")->trigger();
            driver.verify(1, 0);
        }
        require(list->count() == 1 && window.isWindowModified(),
            "取消参数变化的保存提醒后项目未保留");
    }

    void testSaveFailure(const QString &imagePath, const QString &projectPath)
    {
        writeProject(projectPath, imagePath);
        MainWindow window;
        openProject(window, projectPath);
        auto *columns = child<QSpinBox>(window, "boardColsSpin");
        columns->setValue(columns->value() + 1);
        require(window.isWindowModified(), "保存失败测试未建立未保存状态");
        // 当前路径变为目录后，QSaveFile 提交必然失败；不依赖运行用户的写权限。
        require(QFile::remove(projectPath), "无法移除测试项目文件");
        require(QDir().mkdir(projectPath), "无法建立不可覆盖的同名目录");
        {
            DialogDriver driver(QMessageBox::Save);
            child<QAction>(window, "clearAction")->trigger();
            driver.verify(1, 0, 1);
        }
        require(child<QListWidget>(window, "imageList")->count() == 1
                && window.isWindowModified(),
            "保存失败仍清空了项目或错误清除了变化状态");
        require(QFileInfo(projectPath).isDir(), "保存失败破坏了原有目标目录");
    }

    void testMissingImageChange(const QTemporaryDir &directory)
    {
        MainWindow window;
        const QString missing = directory.filePath(QStringLiteral("missing.png"));
        window.loadImages({missing});
        require(child<QListWidget>(window, "imageList")->count() == 1
                && window.isWindowModified(),
            "载入失败的图片占位未保留或未标记项目变化");
    }

    void testReopenCurrentProject(const QString &imagePath, const QString &projectPath)
    {
        writeProject(projectPath, imagePath);
        MainWindow window;
        openProject(window, projectPath);
        auto *columns = child<QSpinBox>(window, "boardColsSpin");
        const int savedColumns = columns->value() + 1;
        columns->setValue(savedColumns);
        {
            DialogDriver driver(QMessageBox::Cancel, FileReply::ChoosePath, projectPath);
            child<QAction>(window, "openProjectAction")->trigger();
            driver.verify(1, 1);
        }
        require(window.isWindowModified() && columns->value() == savedColumns,
            "取消重新打开应保留当前配置和未保存状态");
        {
            DialogDriver driver(QMessageBox::Save, FileReply::ChoosePath, projectPath);
            child<QAction>(window, "openProjectAction")->trigger();
            driver.verify(1, 1);
        }
        const auto saved = CalibrationProjectIo::load(projectPath);
        require(saved && saved->options.boardSize.width() == savedColumns
                && columns->value() == savedColumns && !window.isWindowModified(),
            "重新打开当前文件时必须恢复刚保存的内容，不能使用保存前的快照");
    }
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, &application, []
        {
            qCritical("未保存提醒测试超时");
            std::_Exit(EXIT_FAILURE);
        });
    watchdog.start(20000);
    try
    {
        QTemporaryDir directory;
        require(directory.isValid(), "无法建立未保存提醒测试目录");
        const QString imagePath = writeImage(directory);
        testClearDecisions(imagePath, directory.filePath(QStringLiteral("saved.calibration.json")));
        testCancelSaveAndClose(imagePath);
        testCleanOperationsAndSaveReuse(imagePath,
            directory.filePath(QStringLiteral("clean.calibration.json")));
        testSaveFailure(imagePath, directory.filePath(QStringLiteral("failure.calibration.json")));
        testMissingImageChange(directory);
        testReopenCurrentProject(imagePath,
            directory.filePath(QStringLiteral("reopen.calibration.json")));
        qInfo("未保存提醒测试通过");
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        qCritical() << "未保存提醒测试失败：" << error.what();
        return EXIT_FAILURE;
    }
}
