#include "image_io.h"

#include <QImageIOHandler>
#include <QImageReader>

#include <vector>

cv::Mat readOrientedImage(const QString &path, cv::ImreadModes mode)
{
    QImageReader reader(path);
    if (reader.format() == QByteArrayLiteral("tiff"))
    {
        // TIFF 解码器自行应用方向。首帧接口允许转置改变行列数，保留位深和通道。
        std::vector<cv::Mat> pages;
        if (cv::imreadmulti(path.toStdString(), pages, 0, 1, mode))
        {
            return pages.front();
        }
        return {};
    }

    cv::Mat image = cv::imread(path.toStdString(), mode == cv::IMREAD_UNCHANGED
        ? mode : mode | cv::IMREAD_IGNORE_ORIENTATION);
    if (image.empty()) return image;

    const auto transformation = reader.transformation();
    const bool mirror = transformation.testFlag(QImageIOHandler::TransformationMirror);
    const bool flip = transformation.testFlag(QImageIOHandler::TransformationFlip);
    if (mirror || flip)
    {
        const int flipCode = mirror && flip ? -1 : (mirror ? 1 : 0);
        cv::flip(image, image, flipCode);
    }
    if (transformation.testFlag(QImageIOHandler::TransformationRotate90))
    {
        cv::rotate(image, image, cv::ROTATE_90_CLOCKWISE);
    }
    return image;
}
