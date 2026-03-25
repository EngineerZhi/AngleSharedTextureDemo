/**
 * main_pipeline.cpp
 * 流水线架构：Producer线程 + Consumer线程
 * 
 * 架构：
 * - Thread A (Producer): ANGLE渲染 → 写入共享纹理
 * - Thread B (Consumer): 从共享纹理读取 → 保存PNG
 * - 同步：KeyedMutex + 条件变量
 */

#include "SharedTextureDemo.h"
#include "PerfettoTracing.h"
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <chrono>

struct RenderTask {
    std::string inputPath;
    std::string outputPath;
    float cornerRadius;
    int taskId;
};

class PipelineProcessor {
public:
    PipelineProcessor() = default;
    ~PipelineProcessor() { Stop(); }

    void Init(const SharedTextureDesc& desc) {
        TRACE_EVENT("pipeline", "PipelineProcessor::Init",
                    "width", static_cast<int>(desc.width),
                    "height", static_cast<int>(desc.height));
        m_desc = desc;
        m_stopFlag = false;
        m_frameReady = false;
        m_currentTaskId = -1;
    }

    void Start() {
        TRACE_EVENT("pipeline", "PipelineProcessor::Start");

        // 启动生产者线程（ANGLE 渲染）
        m_producerThread = std::thread(&PipelineProcessor::ProducerThreadFunc, this);
        
        // 启动消费者线程（D3D11 保存）
        m_consumerThread = std::thread(&PipelineProcessor::ConsumerThreadFunc, this);
        
        printf("[Pipeline] Producer and Consumer threads started\n");
    }

    void AddTask(const std::string& inputPath, const std::string& outputPath, float cornerRadius) {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        RenderTask task;
        task.inputPath = inputPath;
        task.outputPath = outputPath;
        task.cornerRadius = cornerRadius;
        task.taskId = m_totalTasks++;
        m_taskQueue.push(task);
        const int queueDepth = static_cast<int>(m_taskQueue.size());
        m_taskCV.notify_one();

        TRACE_EVENT("pipeline", "PipelineProcessor::AddTask",
                    "task_id", task.taskId,
                    "corner_radius", cornerRadius,
                    "queue_depth", queueDepth,
                    "input", inputPath,
                    "output", outputPath);
        TRACE_COUNTER("pipeline", "TaskQueueDepth", queueDepth);
        
        printf("[Pipeline] Task #%d queued: %s -> %s\n", task.taskId, inputPath.c_str(), outputPath.c_str());
    }

    void Stop() {
        TRACE_EVENT("pipeline", "PipelineProcessor::Stop");
        m_stopFlag = true;
        m_taskCV.notify_all();
        m_frameCV.notify_all();

        if (m_producerThread.joinable()) m_producerThread.join();
        if (m_consumerThread.joinable()) m_consumerThread.join();
    }

    void WaitForCompletion() {
        TRACE_EVENT("pipeline", "PipelineProcessor::WaitForCompletion",
                    "total_tasks", m_totalTasks);
        // 等待所有任务完成
        while (m_completedCount < m_totalTasks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Stop();
    }

    int GetCompletedCount() const { return m_completedCount.load(); }
    int GetTotalTasks() const { return m_totalTasks; }

private:
    void ProducerThreadFunc() {
        demo::tracing::SetCurrentThreadName("Producer Thread");
        TRACE_EVENT("pipeline", "ProducerThreadFunc");
        printf("[Producer] Thread started\n");

        try {
            // 在生产者线程初始化 ANGLEProducer（EGL 上下文要求）
            ANGLEProducer producer;
            producer.Init(m_desc);
            
            // 保存共享句柄供消费者使用
            {
                std::lock_guard<std::mutex> lock(m_shareMutex);
                m_shareHandle = producer.GetShareHandle();
                m_shareHandleReady = true;
            }
            m_shareCV.notify_one();

            printf("[Producer] ANGLEProducer initialized\n");

            while (!m_stopFlag) {
                RenderTask task;

                // 获取任务
                {
                    std::unique_lock<std::mutex> lock(m_taskMutex);
                    {
                        TRACE_EVENT("sync", "ProducerWaitForTask");
                        m_taskCV.wait(lock, [this] { 
                            return m_stopFlag || !m_taskQueue.empty(); 
                        });
                    }

                    if (m_stopFlag && m_taskQueue.empty()) break;
                    if (m_taskQueue.empty()) continue;

                    task = m_taskQueue.front();
                    m_taskQueue.pop();
                    TRACE_COUNTER("pipeline", "TaskQueueDepth", static_cast<int>(m_taskQueue.size()));
                }

                printf("[Producer] Processing task #%d: %s\n", task.taskId, task.inputPath.c_str());

                {
                    TRACE_EVENT("pipeline", "ProducerProcessTask",
                                "task_id", task.taskId,
                                "corner_radius", task.cornerRadius,
                                "input", task.inputPath);

                    // 加载图像
                    producer.LoadImageFromFile(task.inputPath.c_str());

                    // 渲染圆角效果
                    producer.RenderWithRoundedCorners(task.cornerRadius);

                    // 通知消费者新帧已就绪
                    {
                        std::lock_guard<std::mutex> lock(m_frameMutex);
                        m_frameReady = true;
                        m_currentTaskId = task.taskId;
                        m_currentOutputPath = task.outputPath;
                    }
                    TRACE_COUNTER("pipeline", "FrameReadyFlag", 1);
                    m_frameCV.notify_one();

                    printf("[Producer] Task #%d rendered, waiting for consumer...\n", task.taskId);

                    // 等待消费者完成（确保KeyedMutex已归还到key=0）
                    {
                        std::unique_lock<std::mutex> lock(m_frameMutex);
                        {
                            TRACE_EVENT("sync", "ProducerWaitForConsumer");
                            m_frameCV.wait(lock, [this] { return !m_frameReady || m_stopFlag; });
                        }
                    }
                }
                if (m_stopFlag) break;
            }

            producer.Destroy();
            printf("[Producer] Thread finished\n");

        } catch (const std::exception& e) {
            fprintf(stderr, "[Producer FATAL] %s\n", e.what());
            m_stopFlag = true;
        }
    }

    void ConsumerThreadFunc() {
        demo::tracing::SetCurrentThreadName("Consumer Thread");
        TRACE_EVENT("pipeline", "ConsumerThreadFunc");
        printf("[Consumer] Thread started\n");

        try {
            // 等待共享句柄就绪
            HANDLE shareHandle;
            {
                std::unique_lock<std::mutex> lock(m_shareMutex);
                {
                    TRACE_EVENT("sync", "ConsumerWaitForShareHandle");
                    m_shareCV.wait(lock, [this] { return m_shareHandleReady; });
                }
                shareHandle = m_shareHandle;
            }

            // 在消费者线程初始化 D3D11Consumer
            D3D11Consumer consumer;
            consumer.Init();
            consumer.OpenSharedTexture(shareHandle, m_desc);

            printf("[Consumer] D3D11Consumer initialized\n");

            while (!m_stopFlag) {
                int taskId;
                std::string outputPath;

                // 等待新帧
                {
                    std::unique_lock<std::mutex> lock(m_frameMutex);
                    {
                        TRACE_EVENT("sync", "ConsumerWaitForFrame");
                        m_frameCV.wait(lock, [this] { 
                            return m_stopFlag || m_frameReady; 
                        });
                    }

                    if (m_stopFlag && !m_frameReady) break;
                    if (!m_frameReady) continue;

                    taskId = m_currentTaskId;
                    outputPath = m_currentOutputPath;
                }

                printf("[Consumer] Saving task #%d: %s\n", taskId, outputPath.c_str());

                {
                    TRACE_EVENT("pipeline", "ConsumerProcessTask",
                                "task_id", taskId,
                                "output", outputPath);

                    // 保存 PNG
                    consumer.SaveToPNG(outputPath.c_str());
                }

                m_completedCount++;
                TRACE_COUNTER("pipeline", "CompletedTasks", m_completedCount.load());
                printf("[Consumer] Task #%d saved (%d/%d completed)\n", 
                       taskId, m_completedCount.load(), m_totalTasks);

                // 通知生产者可以继续
                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_frameReady = false;
                }
                TRACE_COUNTER("pipeline", "FrameReadyFlag", 0);
                m_frameCV.notify_one();
            }

            consumer.Destroy();
            printf("[Consumer] Thread finished\n");

        } catch (const std::exception& e) {
            fprintf(stderr, "[Consumer FATAL] %s\n", e.what());
            m_stopFlag = true;
        }
    }

    SharedTextureDesc m_desc;

    // 任务队列
    std::queue<RenderTask> m_taskQueue;
    std::mutex m_taskMutex;
    std::condition_variable m_taskCV;
    int m_totalTasks = 0;

    // 共享句柄同步
    HANDLE m_shareHandle = nullptr;
    bool m_shareHandleReady = false;
    std::mutex m_shareMutex;
    std::condition_variable m_shareCV;

    // 帧同步（生产者→消费者）
    std::mutex m_frameMutex;
    std::condition_variable m_frameCV;
    bool m_frameReady = false;
    int m_currentTaskId = -1;
    std::string m_currentOutputPath;

    // 线程控制
    std::thread m_producerThread;
    std::thread m_consumerThread;
    std::atomic<bool> m_stopFlag{false};
    std::atomic<int> m_completedCount{0};
};

int main() {
    printf("=========================================\n");
    printf("Pipeline Architecture Demo\n");
    printf("Producer Thread (ANGLE) + Consumer Thread (D3D11)\n");
    printf("=========================================\n\n");

    std::string tracePath;

    try {
        demo::tracing::TraceSession trace({
            "pipeline_demo",
            "PipelineDemo",
            32768,
        });
        tracePath = trace.output_path();
        demo::tracing::SetCurrentThreadName("Main Thread");

        TRACE_EVENT("app", "PipelineDemoMain");

        SharedTextureDesc desc;
        desc.width = 800;
        desc.height = 600;

        PipelineProcessor pipeline;
        pipeline.Init(desc);
        
        // 添加任务（圆角半径：30, 50, 70, 90, 120）
        pipeline.AddTask("bg2.jpg", "pipeline_output_01.png", 30.0f);
        pipeline.AddTask("bg2.jpg", "pipeline_output_02.png", 50.0f);
        pipeline.AddTask("bg2.jpg", "pipeline_output_03.png", 70.0f);
        pipeline.AddTask("bg2.jpg", "pipeline_output_04.png", 90.0f);
        pipeline.AddTask("bg2.jpg", "pipeline_output_05.png", 120.0f);

        auto startTime = std::chrono::high_resolution_clock::now();

        // 启动流水线
        pipeline.Start();

        // 等待完成
        printf("\n--- Pipeline running ---\n\n");
        pipeline.WaitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        printf("\n=========================================\n");
        printf("SUCCESS!\n");
        printf("Tasks completed: %d/%d\n", pipeline.GetCompletedCount(), pipeline.GetTotalTasks());
        printf("Total time: %lld ms\n", duration.count());
        printf("Average: %.2f ms/image\n", (float)duration.count() / pipeline.GetTotalTasks());
        printf("=========================================\n");

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
