#include "PerfettoTracing.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

PERFETTO_TRACK_EVENT_STATIC_STORAGE();
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(
    perfetto::internal::TrackEventDataSource,
    perfetto::internal::TrackEventDataSourceTraits);

namespace demo::tracing {
namespace {

constexpr const char* kEnabledCategories[] = {
    "app",
    "pipeline",
    "angle",
    "d3d11",
    "io",
    "sync",
};

std::string MakeTimestampString() {
    std::time_t now = std::time(nullptr);
    std::tm localTime = {};
#if defined(_WIN32)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &localTime);
    return buffer;
}

std::string MakeTracePath(const std::string& fileStem) {
    namespace fs = std::filesystem;

    const fs::path traceDir = fs::current_path() / "traces";
    fs::create_directories(traceDir);

    const std::string stem = fileStem.empty() ? "trace" : fileStem;
    return (traceDir / (stem + "_" + MakeTimestampString() + ".pftrace")).string();
}

perfetto::TraceConfig BuildTraceConfig(uint32_t bufferSizeKb) {
    perfetto::TraceConfig cfg;

    auto* bufferCfg = cfg.add_buffers();
    bufferCfg->set_size_kb(bufferSizeKb == 0 ? 16384u : bufferSizeKb);

    auto* dataSourceCfg = cfg.add_data_sources()->mutable_config();
    dataSourceCfg->set_name("track_event");
    dataSourceCfg->set_target_buffer(0);

    perfetto::protos::gen::TrackEventConfig trackEventCfg;
    for (const char* category : kEnabledCategories) {
        trackEventCfg.add_enabled_categories(category);
    }
    dataSourceCfg->set_track_event_config_raw(trackEventCfg.SerializeAsString());

    return cfg;
}

}  // namespace

void InitializePerfetto() {
    static std::once_flag once;
    std::call_once(once, [] {
        perfetto::TracingInitArgs args;
        args.backends = perfetto::kInProcessBackend;
        args.shmem_size_hint_kb = 4096;
        args.shmem_page_size_hint_kb = 16;
        args.disallow_merging_with_system_tracks = true;
        args.log_message_callback = [](perfetto::LogMessageCallbackArgs cb) {
            if (cb.level < perfetto::base::kLogError) {
                return;
            }
            std::fprintf(stderr, "[Perfetto] %s:%d %s\n", cb.filename, cb.line, cb.message);
        };

        perfetto::Tracing::Initialize(args);
        perfetto::TrackEvent::Register();
    });
}

void SetProcessName(const std::string& name) {
    if (name.empty()) {
        return;
    }

    InitializePerfetto();

    auto desc = perfetto::ProcessTrack::Current().Serialize();
    desc.mutable_process()->set_process_name(name);
    perfetto::TrackEvent::SetTrackDescriptor(perfetto::ProcessTrack::Current(), desc);
}

void SetCurrentThreadName(const std::string& name) {
    if (name.empty()) {
        return;
    }

    InitializePerfetto();

    auto desc = perfetto::ThreadTrack::Current().Serialize();
    desc.mutable_thread()->set_thread_name(name);
    perfetto::TrackEvent::SetTrackDescriptor(perfetto::ThreadTrack::Current(), desc);
}

TraceSession::TraceSession(TraceSessionOptions options)
    : output_path_(MakeTracePath(options.file_stem)) {
    InitializePerfetto();

    session_ = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
    if (!session_) {
        throw std::runtime_error("Failed to create Perfetto tracing session");
    }

    session_->Setup(BuildTraceConfig(options.buffer_size_kb));
    session_->StartBlocking();

    if (!options.process_name.empty()) {
        SetProcessName(options.process_name);
    }

    std::printf("[Perfetto] Recording trace to %s\n", output_path_.c_str());
}

TraceSession::~TraceSession() {
    Finalize();
}

bool TraceSession::Finalize() {
    if (finalized_) {
        return true;
    }
    finalized_ = true;

    if (!session_) {
        return false;
    }

    (void)session_->FlushBlocking(5000);
    session_->StopBlocking();

    const std::vector<char> traceData = session_->ReadTraceBlocking();
    std::ofstream output(output_path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "[Perfetto] Failed to open trace file: %s\n", output_path_.c_str());
        return false;
    }

    if (!traceData.empty()) {
        output.write(traceData.data(), static_cast<std::streamsize>(traceData.size()));
    }
    output.close();

    if (!output) {
        std::fprintf(stderr, "[Perfetto] Failed to write trace file: %s\n", output_path_.c_str());
        return false;
    }

    std::printf("[Perfetto] Trace saved: %s (%zu bytes)\n",
                output_path_.c_str(),
                traceData.size());
    return true;
}

}  // namespace demo::tracing
