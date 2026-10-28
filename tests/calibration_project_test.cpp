#include "calibration_project.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>

#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void writeFile(const QString &path, const QByteArray &data)
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "无法写入测试文件");
        require(file.write(data) == data.size(), "测试文件写入不完整");
    }

    QJsonObject readJson(const QString &path)
    {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "无法读取项目 JSON");
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    void writeJson(const QString &path, const QJsonObject &object)
    {
        writeFile(path, QJsonDocument(object).toJson());
    }

    CalibrationProject makeProject(const QString &directory)
    {
        CalibrationProject project;
        project.imagePaths = {directory + "/first.png", directory + "/second.png"};
        project.disabledImagePaths = {project.imagePaths[1]};
        writeFile(project.imagePaths[0], "first image");
        writeFile(project.imagePaths[1], "second image");

        CalibrationResult result;
        result.success = true;
        result.qualityWarning = true;
        result.options = project.options;
        result.rmsError = 0.45;
        result.imagesUsed = 1;
        result.imagesTotal = 1;
        result.imageSize = {1920, 1080};
        result.cameraMatrix = cv::Mat(cv::Matx33d(
            820.0, 0.0, 960.0, 0.0, 821.0, 540.0, 0.0, 0.0, 1.0));
        result.distCoeffs = cv::Mat(
            std::vector<double>{-0.2, 0.05, 0.001, -0.001, -0.004}, true).reshape(1, 1);
        result.poses.push_back({project.imagePaths[0], {0.1, 0.2, 0.3}, {10.0, 20.0, 500.0}, 0.45,
            {{100.5f, 200.25f}, {300.0f, 400.0f}}, {{100.75f, 200.5f}, {300.25f, 400.5f}}});
        result.sourceDirectory = directory;
        result.report = QStringLiteral("测试标定报告");
        result.imageReports.push_back({project.imagePaths[0], ImageDetectionStatus::Success,
            QStringLiteral("检测成功，共 2 个有效角点。"), {1920, 1080}, {1920, 1080}, 2});
        project.result = result;
        return project;
    }

    void testRoundTrip(const QString &directory)
    {
        const CalibrationProject original = makeProject(directory);
        const QString path = directory + "/session.calibration.json";
        QString error;
        require(CalibrationProjectIo::save(path, original, &error), "v3 保存失败");
        const auto restored = CalibrationProjectIo::load(path, &error);
        require(restored.has_value() && restored->result.has_value(), "v3 未恢复完整结果");
        require(error.isEmpty() && restored->resultWarning.isEmpty(), "未改图时不应产生警告");
        require(restored->imagePaths == original.imagePaths, "图片路径往返不一致");
        require(restored->disabledImagePaths == original.disabledImagePaths, "禁用状态往返不一致");
        const CalibrationResult &result = *restored->result;
        require(result.options.boardSize == original.options.boardSize, "标定选项未恢复");
        require(result.success && result.qualityWarning && !restored->resultImported, "结果状态未恢复");
        require(result.rmsError == 0.45 && result.imagesUsed == 1 && result.imagesTotal == 1,
            "误差或图片数量未恢复");
        require(result.imageSize == QSize(1920, 1080), "标定分辨率未恢复");
        require(cv::norm(result.cameraMatrix, original.result->cameraMatrix) == 0.0, "内参未完整恢复");
        require(cv::norm(result.distCoeffs, original.result->distCoeffs) == 0.0, "畸变系数未完整恢复");
        require(result.poses.size() == 1 && result.poses[0].imagePath == original.imagePaths[0],
            "位姿路径未恢复");
        require(result.poses[0].rotationVector == original.result->poses[0].rotationVector
            && result.poses[0].translationVector == original.result->poses[0].translationVector
            && result.poses[0].reprojectionError == original.result->poses[0].reprojectionError,
            "位姿数值未完整恢复");
        require(result.report == original.result->report && result.sourceDirectory == directory,
            "结果说明或来源目录未恢复");
        require(result.poses[0].detectedPoints == original.result->poses[0].detectedPoints
            && result.poses[0].projectedPoints == original.result->poses[0].projectedPoints,
            "角点及重投影诊断数据未完整恢复");
        require(result.imageReports.size() == 1
            && result.imageReports[0].imagePath == original.imagePaths[0]
            && result.imageReports[0].status == ImageDetectionStatus::Success
            && result.imageReports[0].message == original.result->imageReports[0].message
            && result.imageReports[0].actualSize == QSize(1920, 1080)
            && result.imageReports[0].expectedSize == QSize(1920, 1080)
            && result.imageReports[0].cornersFound == 2, "逐图检测报告未完整恢复");
        const QJsonObject saved = readJson(path);
        require(saved.value("format_version").toInt() == 3, "保存版本应为 v3");
        require(!QDir::isAbsolutePath(saved.value("images").toArray()[0].toString()),
            "项目应保存相对图片路径");
        require(!QDir::isAbsolutePath(saved.value("result").toObject().value("poses")
                .toArray()[0].toObject().value("image").toString()),
            "位姿应保存相对图片路径");

        // 外部导入参数允许没有图片和位姿，同时保留来源标记与鱼眼矩阵形状。
        CalibrationProject imported = original;
        imported.imagePaths.clear();
        imported.disabledImagePaths.clear();
        imported.options.cameraModel = CameraModel::Fisheye;
        imported.result->options = imported.options;
        imported.result->poses.clear();
        imported.result->imageReports.clear();
        imported.result->distCoeffs = cv::Mat::zeros(4, 1, CV_64F);
        imported.resultImported = true;
        const QString importedPath = directory + "/imported.calibration.json";
        require(CalibrationProjectIo::save(importedPath, imported, &error), "导入参数项目保存失败");
        const auto importedAgain = CalibrationProjectIo::load(importedPath, &error);
        require(importedAgain.has_value() && importedAgain->result && importedAgain->resultImported,
            "导入参数来源或结果未恢复");
        require(importedAgain->result->distCoeffs.rows == 4
            && importedAgain->result->distCoeffs.cols == 1
            && importedAgain->result->options.cameraModel == CameraModel::Fisheye,
            "鱼眼参数未完整恢复");
    }

    void testInputInvalidation(const QString &directory)
    {
        const CalibrationProject original = makeProject(directory);
        const QString path = directory + "/changed.calibration.json";
        QString error;
        require(CalibrationProjectIo::save(path, original, &error), "指纹测试保存失败");
        writeFile(original.imagePaths[0], "modified image with different size");
        auto changed = CalibrationProjectIo::load(path, &error);
        require(changed.has_value() && !changed->result && !changed->resultWarning.isEmpty(),
            "改图后旧结果应失效");
        require(changed->imagePaths == original.imagePaths
            && changed->disabledImagePaths == original.disabledImagePaths && error.isEmpty(),
            "改图不应丢弃项目或图片启用状态");

        const CalibrationProject fresh = makeProject(directory);
        require(CalibrationProjectIo::save(path, fresh, &error), "时间戳测试保存失败");
        QFile image(fresh.imagePaths[0]);
        require(image.open(QIODevice::ReadWrite), "无法打开时间戳测试图片");
        require(image.setFileTime(QDateTime::currentDateTime().addSecs(10),
                QFileDevice::FileModificationTime), "无法修改测试图片时间戳");
        image.close();
        changed = CalibrationProjectIo::load(path, &error);
        require(changed.has_value() && !changed->result, "仅时间戳变化也应使结果失效");

        require(CalibrationProjectIo::save(path, fresh, &error), "缺图测试保存失败");
        require(QFile::remove(fresh.imagePaths[1]), "无法删除测试图片");
        changed = CalibrationProjectIo::load(path, &error);
        require(changed.has_value() && !changed->result && !changed->resultWarning.isEmpty(),
            "图片缺失应仅丢弃结果");
    }

    void testInvalidData(const QString &directory)
    {
        CalibrationProject project = makeProject(directory);
        const QString path = directory + "/invalid.calibration.json";
        QString error;
        require(CalibrationProjectIo::save(path, project, &error), "无效数据基线保存失败");
        const QJsonObject baseline = readJson(path);
        project.result->rmsError = std::numeric_limits<double>::quiet_NaN();
        require(!CalibrationProjectIo::save(path, project, &error) && !error.isEmpty(),
            "非有限 RMS 必须拒绝保存");
        require(readJson(path) == baseline, "保存失败不应破坏旧文件");

        project.result->rmsError = 0.45;
        project.result->cameraMatrix.at<double>(0, 0) = -1.0;
        require(!CalibrationProjectIo::save(path, project, &error), "负焦距必须拒绝保存");
        project.result->cameraMatrix.at<double>(0, 0) = 820.0;
        project.result->options.cameraModel = CameraModel::Fisheye;
        require(!CalibrationProjectIo::save(path, project, &error), "结果模型不一致必须拒绝保存");
        project.result->options = project.options;
        project.result->poses[0].projectedPoints.pop_back();
        require(!CalibrationProjectIo::save(path, project, &error), "不配对的诊断点必须拒绝保存");
        project.result->poses[0].projectedPoints = project.result->poses[0].detectedPoints;
        project.result->poses[0].detectedPoints[0].x = std::numeric_limits<float>::infinity();
        require(!CalibrationProjectIo::save(path, project, &error), "非有限诊断点必须拒绝保存");
        project.result->poses[0].detectedPoints[0].x = 100.5f;
        project.disabledImagePaths = {directory + "/unlisted.png"};
        require(!CalibrationProjectIo::save(path, project, &error), "禁用列表必须是图片列表子集");
        project.disabledImagePaths = {project.imagePaths[0], project.imagePaths[0]};
        require(!CalibrationProjectIo::save(path, project, &error), "禁用列表不允许重复");

        for (int version: {1, 2, 4})
        {
            QJsonObject unsupported = baseline;
            unsupported.insert("format_version", version);
            writeJson(path, unsupported);
            require(!CalibrationProjectIo::load(path, &error), "仅允许加载当前 v3 项目");
        }
        for (const QString &field: {QStringLiteral("image_fingerprints"),
                 QStringLiteral("disabled_images"), QStringLiteral("result_imported")})
        {
            QJsonObject incomplete = baseline;
            incomplete.remove(field);
            writeJson(path, incomplete);
            require(!CalibrationProjectIo::load(path, &error), "项目缺少必需字段时必须拒绝加载");
        }
        QJsonObject incomplete = baseline;
        QJsonObject incompleteResult = incomplete.value("result").toObject();
        incompleteResult.remove("image_reports");
        incomplete.insert("result", incompleteResult);
        writeJson(path, incomplete);
        require(!CalibrationProjectIo::load(path, &error), "项目结果缺少逐图报告字段时必须拒绝加载");

        QJsonObject invalid = baseline;
        QJsonObject invalidResult = invalid.value("result").toObject();
        invalidResult.insert("camera_model", "fisheye");
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "加载时必须拒绝不匹配的结果模型");
        invalid = baseline;
        invalidResult = invalid.value("result").toObject();
        invalidResult.insert("distortion_coefficients", QJsonObject{
            {"rows", 1}, {"columns", 4}, {"data", QJsonArray{0, 0, 0, 0}}});
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "针孔畸变系数数量错误必须拒绝加载");
        invalid = baseline;
        invalid.insert("disabled_images", QJsonArray{"unlisted.png"});
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "加载时必须拒绝无效禁用列表");

        invalid = baseline;
        invalidResult = invalid.value("result").toObject();
        QJsonArray poses = invalidResult.value("poses").toArray();
        QJsonObject pose = poses[0].toObject();
        pose.remove("projected_points");
        poses[0] = pose;
        invalidResult.insert("poses", poses);
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "只有一组诊断点必须拒绝加载");
        pose.insert("projected_points", QJsonArray{QJsonArray{100.0, 200.0}});
        poses[0] = pose;
        invalidResult.insert("poses", poses);
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "诊断点长度不匹配必须拒绝加载");
        pose.insert("projected_points", QJsonArray{QJsonArray{100.0, 200.0}, QJsonArray{"bad", 10.0}});
        poses[0] = pose;
        invalidResult.insert("poses", poses);
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "非数值诊断点必须拒绝加载");

        invalid = baseline;
        invalidResult = invalid.value("result").toObject();
        QJsonArray imageReports = invalidResult.value("image_reports").toArray();
        QJsonObject imageReport = imageReports[0].toObject();
        imageReport.insert("status", "unknown_status");
        imageReports[0] = imageReport;
        invalidResult.insert("image_reports", imageReports);
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "未知逐图报告状态必须拒绝加载");
        imageReport.insert("status", "success");
        imageReport.insert("corners_found", -1);
        imageReports[0] = imageReport;
        invalidResult.insert("image_reports", imageReports);
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "负角点数必须拒绝加载");
        imageReport.insert("corners_found", 2);
        imageReport.insert("image", "");
        imageReports[0] = imageReport;
        invalidResult.insert("image_reports", imageReports);
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "空逐图报告路径必须拒绝加载");
        imageReport.insert("image", "first.png");
        imageReport.insert("actual_width", 0);
        imageReports[0] = imageReport;
        invalidResult.insert("image_reports", imageReports);
        invalid.insert("result", invalidResult);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error), "无效报告尺寸必须拒绝加载");
    }

    void testModelData(const QString &directory)
    {
        CalibrationProject project = makeProject(directory);
        const QString path = directory + "/models.calibration.json";
        QString error;
        project.result->distCoeffs = project.result->distCoeffs.t();
        require(CalibrationProjectIo::save(path, project, &error), "针孔模型列向量保存失败");
        const auto columnVector = CalibrationProjectIo::load(path, &error);
        require(columnVector && columnVector->result
            && columnVector->result->distCoeffs.rows == 5
            && columnVector->result->distCoeffs.cols == 1
            && cv::norm(columnVector->result->distCoeffs, project.result->distCoeffs) == 0.0,
            "针孔模型5×1矩阵未完整恢复");
        const QJsonObject baseline = readJson(path);

        project.options.radialCoeffs = 4;
        project.result->options = project.options;
        require(!CalibrationProjectIo::save(path, project, &error), "针孔模型径向系数数量必须限制到3");
        project.options.radialCoeffs = 3;
        project.result->options = project.options;
        project.result->distCoeffs = cv::Mat::zeros(1, 8, CV_64F);
        require(!CalibrationProjectIo::save(path, project, &error) && !error.isEmpty(),
            "针孔模型必须明确拒绝8个畸变系数");
        project.result->distCoeffs = cv::Mat::zeros(1, 5, CV_64F);
        project.options.cameraModel = static_cast<CameraModel>(2);
        project.result->options = project.options;
        require(!CalibrationProjectIo::save(path, project, &error) && !error.isEmpty(),
            "未知模型枚举值必须明确拒绝保存");

        QJsonObject invalid = baseline;
        QJsonObject result = invalid.value("result").toObject();
        result.insert("distortion_coefficients", QJsonObject{
            {"rows", 1}, {"columns", 8}, {"data", QJsonArray{0, 0, 0, 0, 0, 0, 0, 0}}});
        invalid.insert("result", result);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error) && !error.isEmpty(),
            "加载针孔项目时必须明确拒绝8个畸变系数");
        invalid = baseline;
        QJsonObject options = invalid.value("options").toObject();
        options.insert("camera_model", "unknown_model");
        invalid.insert("options", options);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error) && !error.isEmpty(),
            "未知项目模型必须明确拒绝加载");
        invalid = baseline;
        result = invalid.value("result").toObject();
        result.insert("camera_model", "unknown_model");
        invalid.insert("result", result);
        writeJson(path, invalid);
        require(!CalibrationProjectIo::load(path, &error) && !error.isEmpty(),
            "未知结果模型必须明确拒绝加载");
    }


}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
    {
        qCritical() << "无法创建测试目录";
        return 1;
    }
    try
    {
        testRoundTrip(directory.path());
        testInputInvalidation(directory.path());
        testInvalidData(directory.path());
        testModelData(directory.path());
    }
    catch (const std::exception &exception)
    {
        qCritical() << exception.what();
        return 1;
    }
    qInfo() << "标定项目持久化测试通过";
    return 0;
}
