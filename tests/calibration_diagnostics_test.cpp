#include "calibration.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <opencv2/calib.hpp>
#include <opencv2/geometry/3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#include <array>
#include <cmath>
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

    QImage imageCopy(const cv::Mat &image)
    {
        return QImage(image.data, image.cols, image.rows, static_cast<int>(image.step),
            QImage::Format_BGR888).copy();
    }

    CalibrationResult previewParameters(CameraModel model)
    {
        CalibrationResult result;
        result.success = true;
        result.options.cameraModel = model;
        result.imageSize = {160, 120};
        result.cameraMatrix = cv::Mat(cv::Matx33d(
            130.0, 0.0, 80.0, 0.0, 131.0, 60.0, 0.0, 0.0, 1.0));
        result.distCoeffs = cv::Mat::zeros(model == CameraModel::Fisheye ? 4 : 5, 1, CV_64F);
        return result;
    }

    void testUndistortionPreview(const QString &directory)
    {
        cv::Mat input(120, 160, CV_8UC3);
        for (int row = 0; row < input.rows; ++row)
        {
            for (int column = 0; column < input.cols; ++column)
            {
                input.at<cv::Vec3b>(row, column) = cv::Vec3b(
                    static_cast<unsigned char>(column), static_cast<unsigned char>(row),
                    static_cast<unsigned char>((column + row) % 256));
            }
        }
        const QString path = directory + "/preview.png";
        require(cv::imwrite(path.toStdString(), input), "无法写入预览测试图片");
        const Calibrator calibrator;
        for (CameraModel model: {CameraModel::Pinhole, CameraModel::Fisheye})
        {
            CalibrationResult result = previewParameters(model);
            const auto preview = calibrator.undistortionPreview(path, result);
            require(preview.error.isEmpty() && !preview.undistorted.isNull(), "有效参数预览失败");
            require(preview.original == imageCopy(input), "对照原图不得叠加角点或发生变化");
            cv::Mat expected;
            if (model == CameraModel::Fisheye)
            {
                // 鱼眼零系数仍需从等距投影转换到针孔投影，不能误认为恒等变换。
                cv::fisheye::undistortImage(input, expected, result.cameraMatrix,
                    result.distCoeffs, result.cameraMatrix, input.size());
            }
            else
            {
                cv::undistort(input, expected, result.cameraMatrix, result.distCoeffs);
                require(preview.original == preview.undistorted, "针孔零畸变应保留原始图像");
            }
            require(preview.undistorted == imageCopy(expected), "预览与模型对应的去畸变输出不一致");

            result.imageSize = {320, 240};
            auto rejected = calibrator.undistortionPreview(path, result);
            require(!rejected.error.isEmpty() && rejected.undistorted.isNull()
                && !rejected.original.isNull(), "分辨率不匹配不得返回成功校正图");
            result.imageSize = {};
            rejected = calibrator.undistortionPreview(path, result);
            require(!rejected.error.isEmpty() && rejected.undistorted.isNull(),
                "缺少标定分辨率不得返回成功校正图");
            result.imageSize = {160, 120};
            result.cameraMatrix.at<double>(0, 0) = std::numeric_limits<double>::quiet_NaN();
            rejected = calibrator.undistortionPreview(path, result);
            require(!rejected.error.isEmpty() && rejected.undistorted.isNull(), "无效内参必须被拒绝");
        }
        const auto missing = calibrator.undistortionPreview(directory + "/missing.png",
            previewParameters(CameraModel::Pinhole));
        require(!missing.error.isEmpty() && missing.undistorted.isNull(), "缺图必须返回错误");
    }

    cv::Mat renderBoard(CameraModel model, const cv::Vec3d &rotation,
        const cv::Vec3d &translation)
    {
        // 用像素射线与棋盘平面的交点渲染，避免拿待测角点求解器生成测试预期值。
        constexpr int scale = 2;
        constexpr double focal = 600.0;
        constexpr double alpha = 0.025;
        cv::Matx33d cameraRotation;
        cv::Rodrigues(rotation, cameraRotation);
        const cv::Matx33d inverseRotation = cameraRotation.t();
        const cv::Vec3d boardTranslation = inverseRotation * translation;
        cv::Mat large(480 * scale, 640 * scale, CV_8UC1, cv::Scalar(180));
        for (int row = 0; row < large.rows; ++row)
        {
            unsigned char *pixels = large.ptr<unsigned char>(row);
            const double y = ((row + 0.5) / scale - 0.5 - 240.0) / focal;
            for (int column = 0; column < large.cols; ++column)
            {
                double x = ((column + 0.5) / scale - 0.5 - 320.0) / focal;
                double rayY = y;
                if (model == CameraModel::Fisheye)
                {
                    // 合成非零 skew 的等距鱼眼相机；D=0 时半径就是入射角。
                    x -= alpha * y;
                    const double radius = std::sqrt(x * x + y * y);
                    const double factor = radius > 0.0 ? std::tan(radius) / radius : 1.0;
                    x *= factor;
                    rayY *= factor;
                }
                const cv::Vec3d ray = inverseRotation * cv::Vec3d(x, rayY, 1.0);
                if (std::abs(ray[2]) < 1e-9)
                {
                    continue;
                }
                const double distance = boardTranslation[2] / ray[2];
                const cv::Vec3d point = distance * ray - boardTranslation;
                if (distance <= 0.0 || point[0] < -20.0 || point[0] >= 160.0
                    || point[1] < -20.0 || point[1] >= 120.0)
                {
                    continue;
                }
                const int squareX = static_cast<int>(std::floor(point[0] / 20.0)) + 1;
                const int squareY = static_cast<int>(std::floor(point[1] / 20.0)) + 1;
                pixels[column] = (squareX + squareY) % 2 == 0 ? 20 : 240;
            }
        }
        cv::Mat image;
        cv::resize(large, image, {640, 480}, 0.0, 0.0, cv::INTER_AREA);
        return image;
    }

    void testCalibrationDiagnostics(const QString &directory, CameraModel model)
    {
        const std::array<cv::Vec3d, 8> rotations{{
            {0.15, -0.25, 0.03}, {-0.3, 0.1, -0.07}, {0.3, 0.3, 0.1},
            {-0.2, -0.3, -0.1}, {0.35, -0.15, 0.15}, {-0.3, 0.3, 0.08},
            {0.1, 0.4, -0.12}, {-0.4, -0.15, 0.05}
        }};
        QStringList paths;
        for (std::size_t index = 0; index < rotations.size(); ++index)
        {
            const cv::Vec3d translation(-95.0 + static_cast<double>(index % 3) * 22.0,
                -70.0 + static_cast<double>(index % 4) * 13.0,
                430.0 + static_cast<double>(index % 3) * 45.0);
            const QString path = directory + QString("/board_%1_%2.png")
                .arg(static_cast<int>(model)).arg(static_cast<int>(index));
            require(cv::imwrite(path.toStdString(), renderBoard(model, rotations[index], translation)),
                "无法写入合成棋盘格");
            paths.push_back(path);
        }
        CalibrationOptions options;
        options.boardType = CalibrationBoardType::Chessboard;
        options.boardSize = {9, 7};
        options.squareSize = 20.0;
        options.cameraModel = model;
        options.method = CalibrationMethod::Classic;
        options.skew = model == CameraModel::Fisheye;
        options.radialCoeffs = 2;
        options.tangential = false;
        Calibrator calibrator;
        std::vector<ImageDetectionReport> callbacks;
        const CalibrationResult result = calibrator.calibrate(paths, options,
            [&callbacks, &paths](int processed, int total, const ImageDetectionReport &report)
            {
                require(processed == static_cast<int>(callbacks.size()) + 1
                    && total == paths.size(), "进度计数必须与已处理图片一致");
                callbacks.push_back(report);
            });
        if (!result.success)
        {
            qCritical().noquote() << result.report;
        }
        require(result.success && result.poses.size() >= 5, "合成棋盘格应能够实际完成标定");
        require(result.imageReports.size() == static_cast<std::size_t>(paths.size())
            && callbacks.size() == result.imageReports.size(), "结果必须保留每张已处理图片的报告");
        int successfulViews = 0;
        for (std::size_t index = 0; index < callbacks.size(); ++index)
        {
            const ImageDetectionReport &report = result.imageReports[index];
            if (report.status == ImageDetectionStatus::Success)
            {
                ++successfulViews;
                bool hasPose = false;
                for (const CalibrationPose &pose: result.poses)
                {
                    hasPose = hasPose || pose.imagePath == report.imagePath;
                }
                require(hasPose, "每张检测成功的输入图片都必须参与共同标定并产生位姿");
            }
            require(callbacks[index].imagePath == report.imagePath
                && callbacks[index].status == report.status
                && callbacks[index].message == report.message
                && callbacks[index].actualSize == report.actualSize
                && callbacks[index].expectedSize == report.expectedSize
                && callbacks[index].cornersFound == report.cornersFound,
                "回调必须与最终逐图报告一致");
        }
        require(result.imagesTotal == paths.size() && result.imagesUsed == successfulViews
            && result.poses.size() == static_cast<std::size_t>(successfulViews),
            "全部输入中的成功视图必须共同用于标定，不得额外划分图片");
        require(result.imageReports[0].status == ImageDetectionStatus::Success
            && result.imageReports[0].cornersFound == 48, "成功检测应记录实际角点数");
        ImageDetectionReport previewReport;
        bool found = false;
        require(!calibrator.previewImage(paths[0], options, result, false, &found, &previewReport).isNull()
            && found && previewReport.status == ImageDetectionStatus::Success
            && previewReport.cornersFound == 48, "成功预览必须返回可用角点报告");
        CalibrationResult mismatched = result;
        mismatched.imageSize = {320, 240};
        require(!calibrator.previewImage(paths[0], options, mismatched, false, &found, &previewReport).isNull()
            && !found && previewReport.status == ImageDetectionStatus::SizeMismatch,
            "角点检测成功不得覆盖有效标定参数的分辨率冲突");
        std::vector<cv::Point3f> objectPoints;
        for (int row = 0; row < 6; ++row)
        {
            for (int column = 0; column < 8; ++column)
            {
                objectPoints.emplace_back(column * 20.0f, row * 20.0f, 0.0f);
            }
        }
        if (model == CameraModel::Fisheye)
        {
            require(std::abs(result.cameraMatrix.at<double>(0, 1)) > 0.1,
                "鱼眼测试应覆盖非零 skew，避免遗漏 alpha 的回归");
        }
        for (const CalibrationPose &pose: result.poses)
        {
            require(pose.detectedPoints.size() == objectPoints.size()
                && pose.projectedPoints.size() == objectPoints.size(), "成功标定必须提供完整诊断点");
            std::vector<cv::Point2f> expected;
            if (model == CameraModel::Fisheye)
            {
                cv::fisheye::projectPoints(objectPoints, expected, pose.rotationVector,
                    pose.translationVector, result.cameraMatrix, result.distCoeffs,
                    result.cameraMatrix.at<double>(0, 1) / result.cameraMatrix.at<double>(0, 0));
            }
            else
            {
                cv::projectPoints(objectPoints, pose.rotationVector, pose.translationVector,
                    result.cameraMatrix, result.distCoeffs, expected);
            }
            require(cv::norm(expected, pose.projectedPoints, cv::NORM_INF) < 1e-4,
                "诊断重投影必须匹配当前相机模型和位姿");
            double squaredError = 0.0;
            for (std::size_t index = 0; index < pose.detectedPoints.size(); ++index)
            {
                const cv::Point2f delta = pose.projectedPoints[index] - pose.detectedPoints[index];
                require(std::isfinite(delta.x) && std::isfinite(delta.y), "诊断点必须为有限值");
                squaredError += static_cast<double>(delta.dot(delta));
            }
            const double rms = std::sqrt(squaredError / pose.detectedPoints.size());
            require(std::abs(rms - pose.reprojectionError) < 1e-6,
                "逐点诊断残差必须与单图 RMS 一致");
        }
    }

    void testDetectionFailures(const QString &directory)
    {
        const QString blankPath = directory + "/blank.png";
        const QString smallPath = directory + "/small.png";
        const QString missingPath = directory + "/missing_detection.png";
        require(cv::imwrite(blankPath.toStdString(), cv::Mat(480, 640, CV_8UC1, cv::Scalar(180)))
            && cv::imwrite(smallPath.toStdString(), cv::Mat(240, 320, CV_8UC1, cv::Scalar(180))),
            "无法写入失败原因测试图片");
        CalibrationOptions options;
        options.boardType = CalibrationBoardType::Chessboard;
        options.boardSize = {9, 7};
        Calibrator calibrator;
        const auto result = calibrator.calibrate({blankPath, smallPath, missingPath}, options);
        require(!result.success && result.imageReports.size() == 3, "失败标定也必须保留逐图原因");
        require(result.imageReports[0].status == ImageDetectionStatus::BoardNotFound
            && result.imageReports[0].cornersFound == 0, "空白图片应报告未找到标定板");
        require(result.imageReports[1].status == ImageDetectionStatus::SizeMismatch
            && result.imageReports[1].actualSize == QSize(320, 240)
            && result.imageReports[1].expectedSize == QSize(640, 480), "错尺寸应记录实际和预期尺寸");
        require(result.imageReports[2].status == ImageDetectionStatus::ReadFailed,
            "不存在的图片应报告读取失败");
        ImageDetectionReport report;
        bool found = true;
        require(!calibrator.previewImage(smallPath, options, result, false, &found, &report).isNull()
            && !found && report.status == ImageDetectionStatus::SizeMismatch,
            "单图预览不得覆盖批次中的尺寸冲突原因");
        const CalibrationResult noResult;
        for (int repetition = 0; repetition < 2; ++repetition)
        {
            require(!calibrator.previewImage(blankPath, options, noResult, false, &found, &report).isNull()
                && !found && report.status == ImageDetectionStatus::BoardNotFound,
                "检测缓存必须保留未找到标定板的报告");
        }
        require(calibrator.previewImage(missingPath, options, noResult, false, &found, &report).isNull()
            && !found && report.status == ImageDetectionStatus::ReadFailed && !report.message.isEmpty(),
            "无法读取的预览必须返回具体原因");

        // 2×2 ChArUco 板只有一个内部角点，能够稳定区分“找到部分点”和“完全未找到”。
        CalibrationOptions tinyOptions;
        tinyOptions.boardType = CalibrationBoardType::Charuco;
        tinyOptions.boardSize = {2, 2};
        tinyOptions.squareSize = 40.0;
        tinyOptions.markerSize = 28.0;
        tinyOptions.dictionary = ArucoDictionary::Dict4x4_50;
        const cv::aruco::CharucoBoard board({2, 2}, 40.0f, 28.0f,
            cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50));
        cv::Mat boardImage;
        board.generateImage({400, 400}, boardImage, 20);
        const QString tinyPath = directory + "/tiny_charuco.png";
        require(cv::imwrite(tinyPath.toStdString(), boardImage), "无法写入小 ChArUco 板");
        const auto insufficient = calibrator.calibrate({tinyPath}, tinyOptions);
        require(insufficient.imageReports.size() == 1
            && insufficient.imageReports[0].status == ImageDetectionStatus::InsufficientCorners
            && insufficient.imageReports[0].cornersFound > 0
            && insufficient.imageReports[0].cornersFound < 4, "检测到少量角点应报告有效角点不足");
        require(!calibrator.previewImage(tinyPath, tinyOptions, noResult, false, &found, &report).isNull()
            && !found && report.status == ImageDetectionStatus::InsufficientCorners,
            "角点不足预览必须返回具体原因");

        // 普通棋盘 1×1 内角点不满足 OpenCV 检测器条件，单图异常不能中止整个批次。
        options.boardSize = {2, 2};
        const auto processingFailure = calibrator.calibrate({blankPath, blankPath}, options);
        require(processingFailure.imageReports.size() == 2
            && processingFailure.imageReports[0].status == ImageDetectionStatus::ProcessingFailed
            && processingFailure.imageReports[1].status == ImageDetectionStatus::ProcessingFailed,
            "逐图处理异常必须记录后继续处理下一张图片");
        require(calibrator.previewImage(blankPath, options, noResult, false, &found, &report).isNull()
            && report.status == ImageDetectionStatus::ProcessingFailed && !report.message.isEmpty(),
            "预览处理异常必须返回具体原因");
    }

    void testParameterFormat(const QString &directory)
    {
        Calibrator calibrator;
        QString error;
        for (CameraModel model: {CameraModel::Pinhole, CameraModel::Fisheye})
        {
            for (CalibrationBoardType board: {CalibrationBoardType::Chessboard, CalibrationBoardType::Charuco})
            {
                CalibrationResult parameters = previewParameters(model);
                parameters.options.boardType = board;
                const QString path = directory + QString("/parameters_%1_%2.yaml")
                    .arg(static_cast<int>(model)).arg(static_cast<int>(board));
                require(calibrator.exportParameters(path, parameters, &error), "当前参数导出失败");
                const auto imported = calibrator.importParameters(path, &error);
                require(imported && imported->options.cameraModel == model
                    && imported->options.boardType == board
                    && imported->options.boardSize == parameters.options.boardSize
                    && imported->options.radialCoeffs == parameters.options.radialCoeffs
                    && imported->distCoeffs.rows == 1
                    && cv::norm(imported->cameraMatrix, parameters.cameraMatrix, cv::NORM_INF) == 0.0
                    && cv::norm(imported->distCoeffs, parameters.distCoeffs.reshape(1, 1), cv::NORM_INF) == 0.0,
                    "两种相机和标定板的当前 YAML 必须正确往返");
            }
        }

        const QString path = directory + "/parameter_format.yaml";
        require(calibrator.exportParameters(path, previewParameters(CameraModel::Pinhole), &error),
            "参数格式测试基线导出失败");
        QFile input(path);
        require(input.open(QIODevice::ReadOnly), "参数基线读取失败");
        const QByteArray baseline = input.readAll();
        input.close();
        const auto reject = [&](QByteArray contents, const char *message) {
            QFile output(path);
            require(output.open(QIODevice::WriteOnly | QIODevice::Truncate), "参数测试写入失败");
            require(output.write(contents) == contents.size(), "参数测试写入不完整");
            output.close();
            require(!calibrator.importParameters(path, &error) && !error.isEmpty(), message);
        };
        reject(QByteArray(baseline).replace("format_version: 3", "format_version: 2"),
            "不应再读取旧版本参数");
        reject(QByteArray(baseline).replace("camera_model: pinhole\n", ""),
            "缺少模型不应默认采用 Pinhole");
        reject(QByteArray(baseline).replace("board_columns:", "missing_board_columns:"),
            "缺少元信息不应回退到默认板尺寸");
        reject(QByteArray(baseline).replace("camera_model: pinhole", "camera_model: unknown"),
            "未知模型必须拒绝");
        reject(QByteArray(baseline).replace("radial_coefficients: 3", "radial_coefficients: 8"),
            "不匹配的径向系数数量必须拒绝");
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
        testUndistortionPreview(directory.path());
        testParameterFormat(directory.path());
        testDetectionFailures(directory.path());
        testCalibrationDiagnostics(directory.path(), CameraModel::Pinhole);
        testCalibrationDiagnostics(directory.path(), CameraModel::Fisheye);
    }
    catch (const std::exception &exception)
    {
        qCritical() << exception.what();
        return 1;
    }
    qInfo() << "标定诊断核心测试通过";
    return 0;
}
