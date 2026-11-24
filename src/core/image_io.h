#pragma once

#include <QString>

#include <opencv2/imgcodecs.hpp>

/// 按图片的显示方向读取首帧；IMREAD_UNCHANGED 保留原始位深和通道。
cv::Mat readOrientedImage(const QString &path, cv::ImreadModes mode);
