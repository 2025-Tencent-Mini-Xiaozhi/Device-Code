#ifndef CAMERA_H
#define CAMERA_H

#include <cstddef>
#include <cstdint>
#include <string>

struct CameraJpegData
{
    const uint8_t *data;
    size_t size;
};

struct CameraRawData
{
    const uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t format; // PIXFORMAT_RGB565, PIXFORMAT_JPEG, etc.
};

class Camera
{
public:
    virtual void SetExplainUrl(const std::string &url, const std::string &token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual std::string Explain(const std::string &question) = 0;

    // 获取当前捕获的JPEG数据
    virtual CameraJpegData GetJpegData() = 0;

    // 获取当前捕获的原始数据
    virtual CameraRawData GetRawData() = 0;
};

#endif // CAMERA_H
