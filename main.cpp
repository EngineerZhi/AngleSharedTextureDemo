/**
 * main.cpp
 * A线程：加载图片并渲染圆角效果
 * B线程：保存为PNG
 */

#include "SharedTextureDemo.h"
#include "PerfettoTracing.h"
#include <cstdio>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

int main() {
    printf("ANGLE Shared Texture - Rounded Corner Demo\n");
    printf("===========================================\n");

    std::string tracePath;

    try {
        demo::tracing::TraceSession trace({
            "angle_shared_texture_demo",
            "ANGLESharedTextureDemo",
            16384,
        });
        tracePath = trace.output_path();
        demo::tracing::SetCurrentThreadName("Main Thread");

        TRACE_EVENT("app", "SingleThreadDemo");

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

        trace.Finalize();

    } catch (const std::exception& e) {
        fprintf(stderr, "\n[FATAL] %s\n", e.what());
        if (!tracePath.empty()) {
            fprintf(stderr, "[Perfetto] Trace output path: %s\n", tracePath.c_str());
        }
        return 1;
    }

    return 0;
}
