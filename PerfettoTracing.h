#pragma once

#include "perfetto-cpp-sdk-src/perfetto.h"

#include <cstdint>
#include <string>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("app").SetDescription("Top-level application flow"),
    perfetto::Category("pipeline").SetDescription("Producer-consumer pipeline flow"),
    perfetto::Category("angle").SetDescription("ANGLE / OpenGL ES work"),
    perfetto::Category("d3d11").SetDescription("Direct3D11 interop and readback"),
    perfetto::Category("io").SetDescription("Image decode and file encode"),
    perfetto::Category("sync").SetDescription("Thread and keyed-mutex waits"));

namespace demo::tracing {

struct TraceSessionOptions {
    std::string file_stem;
    std::string process_name;
    uint32_t buffer_size_kb = 16384;
};

void InitializePerfetto();
void SetProcessName(const std::string& name);
void SetCurrentThreadName(const std::string& name);

class TraceSession {
public:
    explicit TraceSession(TraceSessionOptions options);
    ~TraceSession();

    TraceSession(const TraceSession&) = delete;
    TraceSession& operator=(const TraceSession&) = delete;

    TraceSession(TraceSession&&) = delete;
    TraceSession& operator=(TraceSession&&) = delete;

    bool Finalize();

    const std::string& output_path() const { return output_path_; }

private:
    std::unique_ptr<perfetto::TracingSession> session_;
    std::string output_path_;
    bool finalized_ = false;
};

}  // namespace demo::tracing
