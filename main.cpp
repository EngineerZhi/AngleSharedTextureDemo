/**
 * main.cpp
 *
 * Demo 入口：演示 ANGLE -> D3D11 共享纹理完整流程
 *
 * 运行逻辑：
 *   - 单线程顺序演示（先 ANGLE 写，再 D3D 读，验证像素）
 *   - 多线程演示（ANGLE 线程持续渲染，D3D 线程持续读取）
 */

#include "SharedTextureDemo.h"
#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>

// -----------------------------------------------------------------------
// 辅助：保存 BGRA 像素为 BMP（用于验证输出，无需第三方库）
// -----------------------------------------------------------------------
static bool SaveBMP(const char* path,
                    const uint8_t* bgra, UINT width, UINT height) {
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    UINT rowBytes = width * 4;
    UINT imageSize = rowBytes * height;

    bfh.bfType    = 0x4D42;  // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize    = bfh.bfOffBits + imageSize;

    bih.biSize        = sizeof(bih);
    bih.biWidth       = static_cast<LONG>(width);
    bih.biHeight      = -static_cast<LONG>(height);  // 负值 = 从顶部开始
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage   = imageSize;

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&bfh, 1, sizeof(bfh), f);
    fwrite(&bih, 1, sizeof(bih), f);
    fwrite(bgra, 1, imageSize, f);
    fclose(f);
    return true;
}

// -----------------------------------------------------------------------
// 验证像素：检查中心点颜色是否非黑（ANGLE 已写入）
// -----------------------------------------------------------------------
static bool VerifyPixels(const std::vector<uint8_t>& pixels,
                          UINT width, UINT height) {
    if (pixels.empty()) return false;
    // 采样中心点
    UINT cx = width / 2, cy = height / 2;
    UINT offset = (cy * width + cx) * 4;
    uint8_t b = pixels[offset + 0];
    uint8_t g = pixels[offset + 1];
    uint8_t r = pixels[offset + 2];
    uint8_t a = pixels[offset + 3];
    printf("  Center pixel BGRA = (%3u, %3u, %3u, %3u)\n", b, g, r, a);
    // 只要不全是 0 就说明有内容
    return (b | g | r) != 0;
}

// -----------------------------------------------------------------------
// Demo 1：单线程顺序模式（最简演示）
// -----------------------------------------------------------------------
static void RunSequentialDemo() {
    printf("\n========== [Demo 1: Sequential Mode] ==========\n");

    SharedTextureDesc desc;
    desc.width  = 640;
    desc.height = 360;

    ANGLEProducer producer;
    D3D11Consumer consumer;

    producer.Init(desc);
    consumer.Init();
    consumer.OpenSharedTexture(producer.GetShareHandle(), desc);

    for (int frame = 0; frame < 5; ++frame) {
        float hue = static_cast<float>(frame) / 5.0f;

        printf("\n[Frame %d] hue=%.2f\n", frame, hue);

        // ANGLE 渲染
        producer.RenderFrame(hue);

        // D3D 消费（顺序模式：在同一线程内，Producer 刚 ReleaseSync(1)，
        //           Consumer 立刻 AcquireSync(1)）
        auto pixels = consumer.ConsumeFrame();

        bool ok = VerifyPixels(pixels, desc.width, desc.height);
        printf("  Pixel verify: %s\n", ok ? "PASS" : "FAIL");

        // 保存第 0 帧为 BMP 验证输出
        if (frame == 0) {
            bool saved = SaveBMP("frame0.bmp", pixels.data(), desc.width, desc.height);
            printf("  Saved frame0.bmp: %s\n", saved ? "OK" : "FAILED");
        }
    }

    consumer.Destroy();
    producer.Destroy();

    printf("[Demo 1] Done\n");
}

// -----------------------------------------------------------------------
// Demo 2：多线程模式（Producer 线程 + Consumer 线程并发）
// -----------------------------------------------------------------------
static void RunMultiThreadDemo() {
    printf("\n========== [Demo 2: Multi-Thread Mode] ==========\n");

    SharedTextureDesc desc;
    desc.width  = 1280;
    desc.height = 720;

    ANGLEProducer producer;
    D3D11Consumer consumer;

    producer.Init(desc);
    consumer.Init();
    consumer.OpenSharedTexture(producer.GetShareHandle(), desc);

    constexpr int TOTAL_FRAMES = 30;
    std::atomic<int>  producedFrames{ 0 };
    std::atomic<int>  consumedFrames{ 0 };
    std::atomic<bool> running{ true };

    // Producer 线程：持续渲染
    std::thread producerThread([&]() {
        int frame = 0;
        while (running.load() && frame < TOTAL_FRAMES) {
            float hue = static_cast<float>(frame % 60) / 60.0f;
            producer.RenderFrame(hue);
            producedFrames.fetch_add(1);
            ++frame;
        }
        running.store(false);
        printf("[ProducerThread] Done, produced %d frames\n", frame);
    });

    // Consumer 线程：持续消费
    std::thread consumerThread([&]() {
        while (running.load() || producedFrames.load() > consumedFrames.load()) {
            if (producedFrames.load() <= consumedFrames.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            auto pixels = consumer.ConsumeFrame();
            int cf = consumedFrames.fetch_add(1) + 1;
            if (cf % 10 == 0) {
                printf("[ConsumerThread] Consumed frame %d\n", cf);
                VerifyPixels(pixels, desc.width, desc.height);
                bool saved = SaveBMP("frame0.bmp", pixels.data(), desc.width, desc.height);
                printf("  Saved frame0.bmp: %s\n", saved ? "OK" : "FAILED");
            }

        }
        printf("[ConsumerThread] Done, consumed %d frames\n",
               consumedFrames.load());
    });

    producerThread.join();
    consumerThread.join();

    printf("[Demo 2] Produced=%d  Consumed=%d\n",
           producedFrames.load(), consumedFrames.load());

    consumer.Destroy();
    producer.Destroy();

    printf("[Demo 2] Done\n");
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main() {
    printf("ANGLE Shared Texture Demo\n");
    printf("=========================\n");

    try {
       // RunSequentialDemo();
        RunMultiThreadDemo();
    } catch (const std::exception& e) {
        fprintf(stderr, "\n[FATAL] %s\n", e.what());
        return 1;
    }

    printf("\nAll demos completed successfully.\n");
    return 0;
}
