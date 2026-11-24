#include "calibration_project.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>

#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace
{
    QString enumName(CameraModel value)
    {
        switch (value)
        {
            case CameraModel::Pinhole: return QStringLiteral("pinhole");
            case CameraModel::Fisheye: return QStringLiteral("fisheye");
        }
        return {};
    }

    QString enumName(CalibrationBoardType value)
    {
        return value == CalibrationBoardType::Chessboard ? QStringLiteral("chessboard") : QStringLiteral("charuco");
    }

    QString enumName(CalibrationMethod value)
    {
        return value == CalibrationMethod::SectorBased ? QStringLiteral("sector_based") : QStringLiteral("classic");
    }

    QString enumName(ArucoDictionary value)
    {
        switch (value)
        {
            case ArucoDictionary::Dict4x4_50:
                return QStringLiteral("DICT_4X4_50");
            case ArucoDictionary::Dict5x5_100:
                return QStringLiteral("DICT_5X5_100");
            case ArucoDictionary::Dict5x5_250:
                return QStringLiteral("DICT_5X5_250");
            case ArucoDictionary::Dict6x6_250:
                return QStringLiteral("DICT_6X6_250");
            case ArucoDictionary::Original:
                return QStringLiteral("DICT_ARUCO_ORIGINAL");
        }
        return {};
    }

    QString enumName(ImageDetectionStatus value)
    {
        switch (value)
        {
            case ImageDetectionStatus::NotProcessed: return QStringLiteral("not_processed");
            case ImageDetectionStatus::Success: return QStringLiteral("success");
            case ImageDetectionStatus::ReadFailed: return QStringLiteral("read_failed");
            case ImageDetectionStatus::SizeMismatch: return QStringLiteral("size_mismatch");
            case ImageDetectionStatus::BoardNotFound: return QStringLiteral("board_not_found");
            case ImageDetectionStatus::InsufficientCorners: return QStringLiteral("insufficient_corners");
            case ImageDetectionStatus::ProcessingFailed: return QStringLiteral("processing_failed");
        }
        return {};
    }

    template<typename Enum>
    bool parseEnum(const QString &, Enum *) = delete;

    template<>
    bool parseEnum(const QString &text, CameraModel *value)
    {
        if (text == QStringLiteral("pinhole"))
        {
            *value = CameraModel::Pinhole;
            return true;
        }
        if (text == QStringLiteral("fisheye"))
        {
            *value = CameraModel::Fisheye;
            return true;
        }
        return false;
    }

    template<>
    bool parseEnum(const QString &text, CalibrationBoardType *value)
    {
        if (text == QStringLiteral("charuco"))
        {
            *value = CalibrationBoardType::Charuco;
            return true;
        }
        if (text == QStringLiteral("chessboard"))
        {
            *value = CalibrationBoardType::Chessboard;
            return true;
        }
        return false;
    }

    template<>
    bool parseEnum(const QString &text, CalibrationMethod *value)
    {
        if (text == QStringLiteral("classic"))
        {
            *value = CalibrationMethod::Classic;
            return true;
        }
        if (text == QStringLiteral("sector_based"))
        {
            *value = CalibrationMethod::SectorBased;
            return true;
        }
        return false;
    }

    template<>
    bool parseEnum(const QString &text, ArucoDictionary *value)
    {
        const std::pair<const char *, ArucoDictionary> values[] = {
            {"DICT_4X4_50", ArucoDictionary::Dict4x4_50},
            {"DICT_5X5_100", ArucoDictionary::Dict5x5_100},
            {"DICT_5X5_250", ArucoDictionary::Dict5x5_250},
            {"DICT_6X6_250", ArucoDictionary::Dict6x6_250},
            {"DICT_ARUCO_ORIGINAL", ArucoDictionary::Original},
        };
        for (const auto &entry: values)
        {
            if (text == QString::fromLatin1(entry.first))
            {
                *value = entry.second;
                return true;
            }
        }
        return false;
    }

    bool validOptions(const CalibrationOptions &options)
    {
        // 项目文件只保存能够被当前界面和求解器共同接受的参数组合
        return (options.cameraModel == CameraModel::Pinhole
                    || options.cameraModel == CameraModel::Fisheye)
                && (options.boardType == CalibrationBoardType::Chessboard
                    || options.boardType == CalibrationBoardType::Charuco)
                && (options.method == CalibrationMethod::Classic
                    || options.method == CalibrationMethod::SectorBased)
                && !enumName(options.dictionary).isEmpty()
                && options.boardSize.width() >= 2
                && options.boardSize.height() >= 2
                && std::isfinite(options.squareSize) && options.squareSize > 0.0
                && std::isfinite(options.markerSize)
                && (options.boardType != CalibrationBoardType::Charuco
                    || (options.markerSize > 0.0
                        && options.markerSize < options.squareSize))
                && options.radialCoeffs >= 2
                && options.radialCoeffs
                <= (options.cameraModel == CameraModel::Fisheye ? 4 : 3);
    }

    bool validReportSize(const QSize &size)
    {
        return (size.width() == -1 && size.height() == -1)
                || (size.width() > 0 && size.height() > 0);
    }

    bool validImageReport(const ImageDetectionReport &report)
    {
        if (report.imagePath.isEmpty() || enumName(report.status).isEmpty()
            || report.cornersFound < 0 || !validReportSize(report.actualSize)
            || !validReportSize(report.expectedSize))
        {
            return false;
        }
        if (report.status == ImageDetectionStatus::Success
            || report.status == ImageDetectionStatus::InsufficientCorners)
        {
            return report.actualSize.isValid() && report.cornersFound > 0;
        }
        if (report.status == ImageDetectionStatus::SizeMismatch)
        {
            return report.actualSize.isValid() && report.expectedSize.isValid()
                    && report.actualSize != report.expectedSize;
        }
        return true;
    }

    bool sameOptions(const CalibrationOptions &left, const CalibrationOptions &right)
    {
        return left.cameraModel == right.cameraModel && left.boardType == right.boardType
                && left.boardSize == right.boardSize && left.squareSize == right.squareSize
                && left.markerSize == right.markerSize && left.dictionary == right.dictionary
                && left.method == right.method && left.skew == right.skew
                && left.tangential == right.tangential && left.radialCoeffs == right.radialCoeffs;
    }

    QString absolutePath(const QDir &baseDirectory, const QString &path)
    {
        return QDir::cleanPath(QDir::isAbsolutePath(path)
            ? path : baseDirectory.absoluteFilePath(path));
    }

    bool validImageLists(const CalibrationProject &project, const QDir &baseDirectory)
    {
        QSet<QString> images;
        for (const QString &path: project.imagePaths)
        {
            const QString normalized = absolutePath(baseDirectory, path);
            if (path.isEmpty() || images.contains(normalized))
            {
                return false;
            }
            images.insert(normalized);
        }
        QSet<QString> disabled;
        for (const QString &path: project.disabledImagePaths)
        {
            const QString normalized = absolutePath(baseDirectory, path);
            if (path.isEmpty() || !images.contains(normalized) || disabled.contains(normalized))
            {
                return false;
            }
            disabled.insert(normalized);
        }
        return true;
    }

    bool validPose(const CalibrationPose &pose)
    {
        if (pose.imagePath.isEmpty() || !std::isfinite(pose.reprojectionError)
            || pose.reprojectionError < 0.0
            || pose.detectedPoints.size() != pose.projectedPoints.size())
        {
            return false;
        }
        for (int index = 0; index < 3; ++index)
        {
            if (!std::isfinite(pose.rotationVector[index])
                || !std::isfinite(pose.translationVector[index]))
            {
                return false;
            }
        }
        for (std::size_t index = 0; index < pose.detectedPoints.size(); ++index)
        {
            const cv::Point2f &detected = pose.detectedPoints[index];
            const cv::Point2f &projected = pose.projectedPoints[index];
            if (!std::isfinite(detected.x) || !std::isfinite(detected.y)
                || !std::isfinite(projected.x) || !std::isfinite(projected.y))
            {
                return false;
            }
        }
        return true;
    }

    bool validResult(const CalibrationResult &result, const CalibrationOptions &options)
    {
        const cv::Mat &matrix = result.cameraMatrix;
        const cv::Mat &distortion = result.distCoeffs;
        const int coefficients = options.cameraModel == CameraModel::Fisheye ? 4 : 5;
        if (!result.success || !sameOptions(result.options, options)
            || !std::isfinite(result.rmsError) || result.rmsError < 0.0
            || result.imagesUsed < 0 || result.imagesTotal < 0
            || (result.hasMetadata("images_total") && result.hasMetadata("images_used")
                && result.imagesTotal < result.imagesUsed)
            || (result.hasMetadata("images_used")
                && result.poses.size() > static_cast<std::size_t>(result.imagesUsed))
            || (result.hasMetadata("images_total")
                && (result.imageReports.size() > static_cast<std::size_t>(result.imagesTotal)
                    || result.poses.size() > static_cast<std::size_t>(result.imagesTotal)))
            || result.imageSize.width() <= 0 || result.imageSize.height() <= 0
            || matrix.type() != CV_64FC1 || matrix.rows != 3 || matrix.cols != 3
            || distortion.type() != CV_64FC1 || (distortion.rows != 1 && distortion.cols != 1)
            || distortion.total() != static_cast<std::size_t>(coefficients)
            || !cv::checkRange(matrix) || !cv::checkRange(distortion)
            || matrix.at<double>(0, 0) <= 0.0 || matrix.at<double>(1, 1) <= 0.0
            || matrix.at<double>(2, 0) != 0.0 || matrix.at<double>(2, 1) != 0.0
            || matrix.at<double>(2, 2) != 1.0)
        {
            return false;
        }
        for (const CalibrationPose &pose: result.poses)
        {
            if (!validPose(pose))
            {
                return false;
            }
        }
        for (const ImageDetectionReport &report: result.imageReports)
        {
            if (!validImageReport(report))
            {
                return false;
            }
        }
        return true;
    }

    QJsonObject matrixObject(const cv::Mat &matrix)
    {
        QJsonArray data;
        for (int row = 0; row < matrix.rows; ++row)
        {
            for (int column = 0; column < matrix.cols; ++column)
            {
                data.append(matrix.at<double>(row, column));
            }
        }
        return {{QStringLiteral("rows"), matrix.rows}, {QStringLiteral("columns"), matrix.cols},
                {QStringLiteral("data"), data}};
    }

    bool readInteger(const QJsonValue &value, int *output)
    {
        const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(number) || std::floor(number) != number
            || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        {
            return false;
        }
        *output = static_cast<int>(number);
        return true;
    }

    bool readMatrix(const QJsonValue &value, cv::Mat *matrix)
    {
        const QJsonObject object = value.toObject();
        int rows = 0;
        int columns = 0;
        if (!readInteger(object.value(QStringLiteral("rows")), &rows)
            || !readInteger(object.value(QStringLiteral("columns")), &columns)
            || rows < 1 || rows > 5 || columns < 1 || columns > 5)
        {
            return false;
        }
        const QJsonArray data = object.value(QStringLiteral("data")).toArray();
        if (data.size() != rows * columns)
        {
            return false;
        }
        *matrix = cv::Mat(rows, columns, CV_64F);
        for (int index = 0; index < data.size(); ++index)
        {
            const double number = data[index].toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(number))
            {
                return false;
            }
            matrix->at<double>(index / columns, index % columns) = number;
        }
        return true;
    }

    QJsonArray imageReportObjects(const std::vector<ImageDetectionReport> &reports,
        const QDir &baseDirectory)
    {
        QJsonArray imageReports;
        for (const ImageDetectionReport &report: reports)
        {
            imageReports.append(QJsonObject{
                {QStringLiteral("image"), baseDirectory.relativeFilePath(report.imagePath)},
                {QStringLiteral("status"), enumName(report.status)},
                {QStringLiteral("message"), report.message},
                {QStringLiteral("actual_width"), report.actualSize.width()},
                {QStringLiteral("actual_height"), report.actualSize.height()},
                {QStringLiteral("expected_width"), report.expectedSize.width()},
                {QStringLiteral("expected_height"), report.expectedSize.height()},
                {QStringLiteral("corners_found"), report.cornersFound},
            });
        }
        return imageReports;
    }

    QJsonArray poseObjects(const std::vector<CalibrationPose> &values,
        const QDir &baseDirectory)
    {
        QJsonArray poses;
        for (const CalibrationPose &pose: values)
        {
            QJsonObject poseObject{
                {QStringLiteral("image"), baseDirectory.relativeFilePath(pose.imagePath)},
                {QStringLiteral("rotation"), QJsonArray{pose.rotationVector[0], pose.rotationVector[1], pose.rotationVector[2]}},
                {QStringLiteral("translation"), QJsonArray{pose.translationVector[0], pose.translationVector[1], pose.translationVector[2]}},
                {QStringLiteral("rms"), pose.reprojectionError},
            };
            QJsonArray detected;
            QJsonArray projected;
            for (std::size_t index = 0; index < pose.detectedPoints.size(); ++index)
            {
                detected.append(QJsonArray{pose.detectedPoints[index].x, pose.detectedPoints[index].y});
                projected.append(QJsonArray{pose.projectedPoints[index].x, pose.projectedPoints[index].y});
            }
            poseObject.insert(QStringLiteral("detected_points"), detected);
            poseObject.insert(QStringLiteral("projected_points"), projected);
            poses.append(poseObject);
        }
        return poses;
    }

    QJsonObject resultObject(const CalibrationResult &result, const QDir &baseDirectory)
    {
        QJsonObject object{
            {QStringLiteral("camera_model"), enumName(result.options.cameraModel)},
            {QStringLiteral("success"), result.success},
            {QStringLiteral("camera_matrix"), matrixObject(result.cameraMatrix)},
            {QStringLiteral("distortion_coefficients"), matrixObject(result.distCoeffs)},
            {QStringLiteral("image_width"), result.imageSize.width()},
            {QStringLiteral("image_height"), result.imageSize.height()},
            {QStringLiteral("poses"), poseObjects(result.poses, baseDirectory)},
            {QStringLiteral("image_reports"), imageReportObjects(result.imageReports, baseDirectory)},
            {QStringLiteral("source_directory"), result.sourceDirectory.isEmpty()
                ? QString() : baseDirectory.relativeFilePath(result.sourceDirectory)},
            {QStringLiteral("report"), result.report},
        };
        if (result.importedMetadataFields)
        {
            QStringList fields = result.importedMetadataFields->values();
            fields.sort();
            object.insert(QStringLiteral("imported_metadata_fields"), QJsonArray::fromStringList(fields));
        }
        if (result.hasMetadata("rms_reprojection_error"))
        {
            object.insert("rms", result.rmsError);
        }
        if (result.hasMetadata("quality_warning"))
        {
            object.insert("quality_warning", result.qualityWarning);
        }
        if (result.hasMetadata("images_used"))
        {
            object.insert("images_used", result.imagesUsed);
        }
        if (result.hasMetadata("images_total"))
        {
            object.insert("images_total", result.imagesTotal);
        }
        return object;
    }

    bool readPoints(const QJsonValue &value, std::vector<cv::Point2f> *points)
    {
        if (!value.isArray())
        {
            return false;
        }
        const QJsonArray array = value.toArray();
        points->reserve(static_cast<std::size_t>(array.size()));
        for (const QJsonValue &entry: array)
        {
            const QJsonArray coordinates = entry.toArray();
            if (coordinates.size() != 2)
            {
                return false;
            }
            const double x = coordinates[0].toDouble(std::numeric_limits<double>::quiet_NaN());
            const double y = coordinates[1].toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(x) || !std::isfinite(y)
                || std::abs(x) > std::numeric_limits<float>::max()
                || std::abs(y) > std::numeric_limits<float>::max())
            {
                return false;
            }
            points->emplace_back(static_cast<float>(x), static_cast<float>(y));
        }
        return true;
    }

    std::optional<ImageDetectionReport> readImageReport(const QJsonValue &value,
        const QDir &baseDirectory)
    {
        const QJsonObject object = value.toObject();
        const QString path = object.value(QStringLiteral("image")).toString();
        const QString status = object.value(QStringLiteral("status")).toString();
        ImageDetectionReport report;
        bool knownStatus = false;
        for (ImageDetectionStatus candidate: {ImageDetectionStatus::NotProcessed,
                 ImageDetectionStatus::Success, ImageDetectionStatus::ReadFailed,
                 ImageDetectionStatus::SizeMismatch, ImageDetectionStatus::BoardNotFound,
                 ImageDetectionStatus::InsufficientCorners, ImageDetectionStatus::ProcessingFailed})
        {
            if (status == enumName(candidate))
            {
                report.status = candidate;
                knownStatus = true;
                break;
            }
        }
        int actualWidth = 0;
        int actualHeight = 0;
        int expectedWidth = 0;
        int expectedHeight = 0;
        if (path.isEmpty() || !knownStatus || !object.value(QStringLiteral("message")).isString()
            || !readInteger(object.value(QStringLiteral("actual_width")), &actualWidth)
            || !readInteger(object.value(QStringLiteral("actual_height")), &actualHeight)
            || !readInteger(object.value(QStringLiteral("expected_width")), &expectedWidth)
            || !readInteger(object.value(QStringLiteral("expected_height")), &expectedHeight)
            || !readInteger(object.value(QStringLiteral("corners_found")), &report.cornersFound))
        {
            return std::nullopt;
        }
        report.imagePath = absolutePath(baseDirectory, path);
        report.message = object.value(QStringLiteral("message")).toString();
        report.actualSize = {actualWidth, actualHeight};
        report.expectedSize = {expectedWidth, expectedHeight};
        return validImageReport(report) ? std::make_optional(report) : std::nullopt;
    }

    bool readPoses(const QJsonValue &value, const QDir &baseDirectory,
        std::vector<CalibrationPose> *output)
    {
        if (!value.isArray())
        {
            return false;
        }
        const QJsonArray poses = value.toArray();
        for (const QJsonValue &value: poses)
        {
            const QJsonObject poseObject = value.toObject();
            const QString path = poseObject.value(QStringLiteral("image")).toString();
            const QJsonArray rotation = poseObject.value(QStringLiteral("rotation")).toArray();
            const QJsonArray translation = poseObject.value(QStringLiteral("translation")).toArray();
            if (path.isEmpty() || rotation.size() != 3 || translation.size() != 3)
            {
                return false;
            }
            CalibrationPose pose;
            pose.imagePath = absolutePath(baseDirectory, path);
            pose.reprojectionError = poseObject.value(QStringLiteral("rms"))
                .toDouble(std::numeric_limits<double>::quiet_NaN());
            for (int index = 0; index < 3; ++index)
            {
                pose.rotationVector[index] = rotation[index]
                    .toDouble(std::numeric_limits<double>::quiet_NaN());
                pose.translationVector[index] = translation[index]
                    .toDouble(std::numeric_limits<double>::quiet_NaN());
            }
            if (!readPoints(poseObject.value(QStringLiteral("detected_points")), &pose.detectedPoints)
                || !readPoints(poseObject.value(QStringLiteral("projected_points")), &pose.projectedPoints)
                || pose.detectedPoints.size() != pose.projectedPoints.size())
            {
                return false;
            }
            output->push_back(pose);
        }
        return true;
    }

    bool readImageReports(const QJsonValue &value, const QDir &baseDirectory,
        std::vector<ImageDetectionReport> *output)
    {
        if (!value.isArray())
        {
            return false;
        }
        for (const QJsonValue &entry: value.toArray())
        {
            const auto report = readImageReport(entry, baseDirectory);
            if (!report)
            {
                return false;
            }
            output->push_back(*report);
        }
        return true;
    }

    std::optional<CalibrationResult> readResult(const QJsonObject &object,
        const CalibrationOptions &options, const QDir &baseDirectory)
    {
        CalibrationResult result;
        result.options = options;
        const QJsonValue fields = object.value(QStringLiteral("imported_metadata_fields"));
        if (!fields.isUndefined())
        {
            if (!fields.isArray()) return std::nullopt;
            result.importedMetadataFields.emplace();
            for (const auto &field: fields.toArray())
            {
                if (!field.isString()) return std::nullopt;
                result.importedMetadataFields->insert(field.toString());
            }
        }
        int width = 0;
        int height = 0;
        CameraModel storedModel;
        if (!parseEnum(object.value(QStringLiteral("camera_model")).toString(), &storedModel)
            || storedModel != options.cameraModel
            || !object.value(QStringLiteral("success")).isBool()
            || (result.hasMetadata("quality_warning") && !object.value("quality_warning").isBool())
            || (result.hasMetadata("rms_reprojection_error") && !object.value("rms").isDouble())
            || !object.value(QStringLiteral("poses")).isArray()
            || !object.value(QStringLiteral("report")).isString()
            || !object.value(QStringLiteral("source_directory")).isString()
            || (result.hasMetadata("images_used") && !readInteger(object.value("images_used"), &result.imagesUsed))
            || (result.hasMetadata("images_total") && !readInteger(object.value("images_total"), &result.imagesTotal))
            || !readInteger(object.value(QStringLiteral("image_width")), &width)
            || !readInteger(object.value(QStringLiteral("image_height")), &height)
            || !readMatrix(object.value(QStringLiteral("camera_matrix")), &result.cameraMatrix)
            || !readMatrix(object.value(QStringLiteral("distortion_coefficients")), &result.distCoeffs))
        {
            return std::nullopt;
        }
        result.success = object.value(QStringLiteral("success")).toBool();
        result.qualityWarning = object.value(QStringLiteral("quality_warning")).toBool();
        result.rmsError = object.value(QStringLiteral("rms")).toDouble();
        result.imageSize = {width, height};
        result.report = object.value(QStringLiteral("report")).toString();
        const QString source = object.value(QStringLiteral("source_directory")).toString();
        result.sourceDirectory = source.isEmpty() ? QString() : absolutePath(baseDirectory, source);
        if (!readPoses(object.value(QStringLiteral("poses")), baseDirectory, &result.poses))
        {
            return std::nullopt;
        }
        if (!readImageReports(object.value(QStringLiteral("image_reports")),
                baseDirectory, &result.imageReports))
        {
            return std::nullopt;
        }
        return validResult(result, options) ? std::make_optional(result) : std::nullopt;
    }

    QJsonObject imageFingerprint(const QString &path)
    {
        const QFileInfo info(path);
        // 使用十进制字符串保存 64 位指纹，避免 JSON 浮点表示丢失精度。
        return {{QStringLiteral("exists"), info.isFile()},
                {QStringLiteral("size"), QString::number(info.size())},
                {QStringLiteral("modified_ms"), QString::number(info.lastModified().toMSecsSinceEpoch())}};
    }
}

bool CalibrationProjectIo::save(const QString &filePath,
    const CalibrationProject &project,
    QString *error)
{
    if (error)
    {
        error->clear();
    }
    if (!validOptions(project.options))
    {
        if (error)
        {
            *error = QStringLiteral("标定项目包含无效参数。");
        }
        return false;
    }

    const QDir baseDirectory = QFileInfo(filePath).absoluteDir();
    if (!validImageLists(project, baseDirectory)
        || (project.result && !validResult(*project.result, project.options)))
    {
        if (error)
        {
            *error = QStringLiteral("项目图片启用列表或标定结果无效，未保存项目。");
        }
        return false;
    }

    const CalibrationOptions &options = project.options;
    QJsonObject optionObject{
        {QStringLiteral("camera_model"), enumName(options.cameraModel)},
        {QStringLiteral("board_type"), enumName(options.boardType)},
        {QStringLiteral("board_columns"), options.boardSize.width()},
        {QStringLiteral("board_rows"), options.boardSize.height()},
        {QStringLiteral("square_size_mm"), options.squareSize},
        {QStringLiteral("marker_size_mm"), options.markerSize},
        {QStringLiteral("dictionary"), enumName(options.dictionary)},
        {QStringLiteral("method"), enumName(options.method)},
        {QStringLiteral("estimate_skew"), options.skew},
        {QStringLiteral("estimate_tangential"), options.tangential},
        {QStringLiteral("radial_coefficients"), options.radialCoeffs},
    };
    QJsonArray images;
    QJsonArray fingerprints;
    for (const QString &imagePath: project.imagePaths)
    {
        // 相对路径让项目文件与图片目录能够作为一个整体迁移
        images.append(baseDirectory.relativeFilePath(imagePath));
        fingerprints.append(imageFingerprint(absolutePath(baseDirectory, imagePath)));
    }
    QJsonArray disabledImages;
    for (const QString &imagePath: project.disabledImagePaths)
    {
        disabledImages.append(baseDirectory.relativeFilePath(imagePath));
    }
    QJsonObject root{
        {QStringLiteral("format_version"), 3},
        {QStringLiteral("options"), optionObject},
        {QStringLiteral("images"), images},
        {QStringLiteral("disabled_images"), disabledImages},
        {QStringLiteral("image_fingerprints"), fingerprints},
        {QStringLiteral("result_imported"), project.result.has_value() && project.resultImported},
    };
    if (project.result)
    {
        root.insert(QStringLiteral("result"), resultObject(*project.result, baseDirectory));
    }
    const QJsonDocument document(root);

    // 写入失败时保留旧项目文件，不暴露不完整的 JSON
    QSaveFile output(filePath);
    const QByteArray data = document.toJson(QJsonDocument::Indented);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(data) != data.size()
        || !output.commit())
    {
        if (error)
        {
            *error = output.errorString();
        }
        return false;
    }
    return true;
}

std::optional<CalibrationProject> CalibrationProjectIo::load(
    const QString &filePath, QString *error)
{
    if (error)
    {
        error->clear();
    }
    QFile input(filePath);
    if (!input.open(QIODevice::ReadOnly))
    {
        if (error)
        {
            *error = input.errorString();
        }
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
            QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject())
    {
        if (error)
        {
            *error = QStringLiteral("项目 JSON 无效：%1").arg(parseError.errorString());
        }
        return std::nullopt;
    }

    const QJsonObject root = document.object();
    const QJsonObject object = root.value(QStringLiteral("options")).toObject();
    const QJsonArray images = root.value(QStringLiteral("images")).toArray();
    const int version = root.value(QStringLiteral("format_version")).toInt();
    if (version != 3 || object.isEmpty()
        || !root.value(QStringLiteral("images")).isArray()
        || !root.value(QStringLiteral("image_fingerprints")).isArray()
        || root.value(QStringLiteral("image_fingerprints")).toArray().size() != images.size())
    {
        if (error)
        {
            *error = QStringLiteral("项目版本、参数或图片列表无效。");
        }
        return std::nullopt;
    }

    // 字符串枚举采用严格解析，未知值不能悄悄回退到默认模型
    CalibrationProject project;
    CalibrationOptions &options = project.options;
    if (!parseEnum(object.value(QStringLiteral("camera_model")).toString(),
            &options.cameraModel)
        || !parseEnum(object.value(QStringLiteral("board_type")).toString(),
            &options.boardType)
        || !parseEnum(object.value(QStringLiteral("method")).toString(),
            &options.method)
        || !parseEnum(object.value(QStringLiteral("dictionary")).toString(),
            &options.dictionary))
    {
        if (error)
        {
            *error = QStringLiteral("项目中包含未知的模型、标定板或检测选项。");
        }
        return std::nullopt;
    }
    int columns = 0;
    int rows = 0;
    if (!readInteger(object.value(QStringLiteral("board_columns")), &columns)
        || !readInteger(object.value(QStringLiteral("board_rows")), &rows)
        || !readInteger(object.value(QStringLiteral("radial_coefficients")), &options.radialCoeffs)
        || !object.value(QStringLiteral("square_size_mm")).isDouble()
        || !object.value(QStringLiteral("marker_size_mm")).isDouble()
        || !object.value(QStringLiteral("estimate_skew")).isBool()
        || !object.value(QStringLiteral("estimate_tangential")).isBool())
    {
        if (error)
        {
            *error = QStringLiteral("项目中的标定参数字段无效。");
        }
        return std::nullopt;
    }
    options.boardSize = {columns, rows};
    options.squareSize = object.value(QStringLiteral("square_size_mm")).toDouble();
    options.markerSize = object.value(QStringLiteral("marker_size_mm")).toDouble();
    options.skew = object.value(QStringLiteral("estimate_skew")).toBool();
    options.tangential =
            object.value(QStringLiteral("estimate_tangential")).toBool();
    if (!validOptions(options))
    {
        if (error)
        {
            *error = QStringLiteral("项目中的标定参数超出有效范围。");
        }
        return std::nullopt;
    }

    const QDir baseDirectory = QFileInfo(filePath).absoluteDir();
    for (const QJsonValue &value: images)
    {
        if (!value.isString() || value.toString().isEmpty())
        {
            if (error)
            {
                *error = QStringLiteral("项目图片路径无效。");
            }
            return std::nullopt;
        }
        const QString path = value.toString();
        // 调用方始终获得规范化后的路径，不再承担项目目录解析职责
        project.imagePaths.push_back(
            QDir::cleanPath(QDir::isAbsolutePath(path) ? path : baseDirectory.absoluteFilePath(path)));
    }
    if (!root.value(QStringLiteral("disabled_images")).isArray()
        || !root.value(QStringLiteral("result_imported")).isBool())
    {
        if (error)
        {
            *error = QStringLiteral("项目图片启用列表或结果来源无效。");
        }
        return std::nullopt;
    }
    const QJsonArray disabled = root.value(QStringLiteral("disabled_images")).toArray();
    for (const QJsonValue &value: disabled)
    {
        if (!value.isString() || value.toString().isEmpty())
        {
            if (error)
            {
                *error = QStringLiteral("项目禁用图片路径无效。");
            }
            return std::nullopt;
        }
        project.disabledImagePaths.push_back(absolutePath(baseDirectory, value.toString()));
    }
    if (root.contains(QStringLiteral("result")))
    {
        project.result = readResult(root.value(QStringLiteral("result")).toObject(),
            options, baseDirectory);
        if (!project.result)
        {
            if (error)
            {
                *error = QStringLiteral("项目中的标定结果无效或与模型不一致。");
            }
            return std::nullopt;
        }
        project.resultImported = root.value(QStringLiteral("result_imported")).toBool();
        const QJsonArray fingerprints = root.value(QStringLiteral("image_fingerprints")).toArray();
        bool inputsChanged = false;
        for (qsizetype index = 0; !inputsChanged && index < project.imagePaths.size(); ++index)
        {
            const QJsonObject current = imageFingerprint(project.imagePaths[index]);
            inputsChanged = !current.value(QStringLiteral("exists")).toBool()
                    || fingerprints[index].toObject() != current;
        }
        if (inputsChanged)
        {
            // 输入仍可继续编辑，但旧结果不能再冒充当前图片的有效标定。
            project.result.reset();
            project.resultImported = false;
            project.resultWarning = QStringLiteral("图片已变化或缺失，已保留项目配置和图片列表，旧标定结果已失效，请重新标定或导入参数。");
        }
    }
    if (!validImageLists(project, baseDirectory))
    {
        if (error)
        {
            *error = QStringLiteral("项目图片列表重复，或禁用图片不属于项目。");
        }
        return std::nullopt;
    }
    return project;
}
