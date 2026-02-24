/**
 * main.cpp
 * A线程：加载图片并渲染圆角效果
 * B线程：保存为PNG
 */

#include "SharedTextureDemo.h"
#include <cstdio>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

int main() {
    printf("ANGLE Shared Texture - Rounded Corner Demo\n");
    printf("===========================================\n");

    try {
        SharedTextureDesc desc;
        desc.width  = 800;
        desc.height = 600;

        ANGLEProducer producer;
        D3D11Consumer consumer;

        producer.Init(desc);
        consumer.Init();
        consumer.OpenSharedTexture(producer.GetShareHandle(), desc);

        printf("\n[Main] Loading image bg2.jpg...\n");
        producer.LoadImageFromFile("bg2.jpg");
        
        printf("[Main] Rendering with rounded corners (radius=50)...\n");
        producer.RenderWithRoundedCorners(50.0f);
        
        printf("\n[Main] Saving to output_rounded.png...\n");
        consumer.SaveToPNG("output_rounded.png");
        printf("[Main] Saved successfully!\n");

        consumer.Destroy();
        producer.Destroy();

        printf("\n========================================\n");
        printf("SUCCESS! Check output_rounded.png\n");
        printf("========================================\n");

    } catch (const std::exception& e) {
        fprintf(stderr, "\n[FATAL] %s\n", e.what());
        return 1;
    }

    return 0;
}
