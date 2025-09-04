#ifndef ESP32_CAMERA_H
#define ESP32_CAMERA_H

#include <esp_camera.h>
#include <lvgl.h>
#include <memory>
#include <thread>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"

struct JpegChunk
{
    uint8_t *data;
    size_t len;
};

class Esp32Camera : public Camera
{
private:
    camera_fb_t *fb_ = nullptr;
    lv_img_dsc_t preview_image_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

public:
    Esp32Camera(const camera_config_t &config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string &url, const std::string &token);
    virtual bool Capture();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string &question);
    virtual CameraJpegData GetJpegData() override;
    virtual CameraRawData GetRawData() override;

    // 获取帧缓冲区
    camera_fb_t *GetFrameBuffer() const { return fb_; }
};

#endif // ESP32_CAMERA_H