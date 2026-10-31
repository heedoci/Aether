// iOS Artemis provider backed by the publicly available Art3m1s Rust engine.
#include "engine_runtime_provider.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>

extern "C" {
void* aether_artemis_create(uint32_t, uint32_t, const char*);
void aether_artemis_destroy(void*);
int32_t aether_artemis_open(void*, const char*);
int32_t aether_artemis_tick(void*, uint32_t);
uint32_t aether_artemis_width(void*);
uint32_t aether_artemis_height(void*);
size_t aether_artemis_frame_bytes(void*, uint8_t*, size_t);
int32_t aether_artemis_has_frame(void*);
void aether_artemis_mouse(void*, int32_t, int32_t);
void aether_artemis_button(void*, uint32_t, int32_t);
void aether_artemis_key(void*, uint32_t, int32_t);
void aether_artemis_lifecycle(void*, int32_t);
const char* aether_artemis_error(void*);
}
namespace {
constexpr char kName[] = "artemis";
struct Session {
    engine_runtime_host_v1_t host{};
    void* runtime = nullptr;
    std::string error;
    uint64_t serial = 0;
    bool opened = false;
    bool rendered = false;
    bool paused = false;
};
Session* Cast(void* p) { return static_cast<Session*>(p); }
engine_result_t Copy(const std::string& value, char* buffer, uint32_t cap,
                     uint32_t* written = nullptr) {
    if (written) *written = 0;
    if (!buffer || cap == 0) return ENGINE_RESULT_INVALID_ARGUMENT;
    const size_t count = std::min<size_t>(value.size(), cap - 1u);
    std::memcpy(buffer, value.data(), count);
    buffer[count] = '\0';
    if (written) *written = static_cast<uint32_t>(count);
    return count == value.size() ? ENGINE_RESULT_OK : ENGINE_RESULT_INVALID_ARGUMENT;
}
void Log(Session* s, uint32_t level, const char* message) {
    if (s && s->host.log)
        s->host.log(s->host.user_data, level, kName, message);
}
int32_t Probe(void*, const char* path) {
    if (!path || !*path) return 0;
    try {
        std::filesystem::path root(path);
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec))
            return root.extension() == ".pfs" ? 95 : 0;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::string n = it->path().filename().string();
            std::transform(n.begin(), n.end(), n.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (n == "root.pfs" || n == "system.ini") return 95;
        }
    } catch (...) {}
    return 0;
}
engine_result_t Create(void*, const engine_runtime_host_v1_t* host,
                       const engine_create_desc_t* desc, void** result) {
    if (!host || !desc || !result ||
        host->api_version != ENGINE_RUNTIME_PROVIDER_API_VERSION)
        return ENGINE_RESULT_INVALID_ARGUMENT;
    *result = nullptr;
    auto s = std::unique_ptr<Session>(new (std::nothrow) Session());
    if (!s) return ENGINE_RESULT_INTERNAL_ERROR;
    s->host = *host;
    s->runtime = aether_artemis_create(1280, 720, desc->writable_path_utf8);
    if (!s->runtime) {
        Log(s.get(), ENGINE_RUNTIME_LOG_ERROR,
            "Art3m1s runtime creation failed (Metal device unavailable?)");
        return ENGINE_RESULT_INTERNAL_ERROR;
    }
    *result = s.release();
    return ENGINE_RESULT_OK;
}
void Destroy(void* p) {
    std::unique_ptr<Session> s(Cast(p));
    if (s && s->runtime) aether_artemis_destroy(s->runtime);
}
engine_result_t Open(void* p, const char* path, const char*) {
    Session* s = Cast(p);
    if (!s || !s->runtime || !path) return ENGINE_RESULT_INVALID_ARGUMENT;
    if (aether_artemis_open(s->runtime, path) != 0) {
        s->error = aether_artemis_error(s->runtime);
        Log(s, ENGINE_RUNTIME_LOG_ERROR, s->error.c_str());
        return ENGINE_RESULT_IO_ERROR;
    }
    s->opened = true;
    s->serial = 0;
    s->rendered = false;
    Log(s, ENGINE_RUNTIME_LOG_INFO, "Artemis game opened using public Art3m1s engine");
    return ENGINE_RESULT_OK;
}
engine_result_t Tick(void* p, uint32_t ms) {
    Session* s = Cast(p);
    if (!s || !s->runtime || !s->opened) return ENGINE_RESULT_INVALID_STATE;
    if (s->paused) return ENGINE_RESULT_OK;
    int32_t status = aether_artemis_tick(s->runtime, ms);
    if (status == -2) {
        s->error = "runtime requested termination";
        return ENGINE_RESULT_INVALID_STATE;
    }
    if (status != 0) {
        s->error = aether_artemis_error(s->runtime);
        Log(s, ENGINE_RUNTIME_LOG_ERROR, s->error.c_str());
        return ENGINE_RESULT_INTERNAL_ERROR;
    }
    s->rendered = aether_artemis_has_frame(s->runtime) != 0;
    ++s->serial;
    return ENGINE_RESULT_OK;
}
engine_result_t Pause(void* p) {
    Session* s = Cast(p);
    if (!s || !s->runtime) return ENGINE_RESULT_INVALID_ARGUMENT;
    s->paused = true;
    aether_artemis_lifecycle(s->runtime, 1);
    return ENGINE_RESULT_OK;
}
engine_result_t Resume(void* p) {
    Session* s = Cast(p);
    if (!s || !s->runtime) return ENGINE_RESULT_INVALID_ARGUMENT;
    s->paused = false;
    aether_artemis_lifecycle(s->runtime, 2);
    return ENGINE_RESULT_OK;
}
engine_result_t Option(void* p, const engine_option_t* option) {
    return p && option ? ENGINE_RESULT_OK : ENGINE_RESULT_INVALID_ARGUMENT;
}
engine_result_t Resize(void* p, uint32_t w, uint32_t h) {
    return p && w && h ? ENGINE_RESULT_OK : ENGINE_RESULT_INVALID_ARGUMENT;
}
engine_result_t FrameDesc(void* p, engine_frame_desc_t* desc) {
    Session* s = Cast(p);
    if (!s || !s->runtime || !desc || desc->struct_size < sizeof(*desc))
        return ENGINE_RESULT_INVALID_ARGUMENT;
    const uint32_t width = aether_artemis_width(s->runtime);
    const uint32_t height = aether_artemis_height(s->runtime);
    if (!width || !height) return ENGINE_RESULT_INVALID_STATE;
    uint32_t struct_size = desc->struct_size;
    *desc = {};
    desc->struct_size = struct_size;
    desc->width = width;
    desc->height = height;
    desc->stride_bytes = width * 4u;
    desc->pixel_format = ENGINE_PIXEL_FORMAT_RGBA8888;
    desc->frame_serial = s->serial;
    return ENGINE_RESULT_OK;
}
engine_result_t ReadFrame(void* p, void* pixels, size_t cap) {
    Session* s = Cast(p);
    if (!s || !s->runtime || !pixels || !s->rendered)
        return ENGINE_RESULT_INVALID_ARGUMENT;
    size_t expected = size_t(aether_artemis_width(s->runtime)) *
                      aether_artemis_height(s->runtime) * 4u;
    if (!expected || cap < expected) return ENGINE_RESULT_INVALID_ARGUMENT;
    return aether_artemis_frame_bytes(s->runtime, static_cast<uint8_t*>(pixels), cap)
           == expected ? ENGINE_RESULT_OK : ENGINE_RESULT_INTERNAL_ERROR;
}
engine_result_t Input(void* p, const engine_input_event_t* e) {
    Session* s = Cast(p);
    if (!s || !s->runtime || !e || e->struct_size < sizeof(*e))
        return ENGINE_RESULT_INVALID_ARGUMENT;
    switch (e->type) {
        case ENGINE_INPUT_EVENT_POINTER_DOWN:
        case ENGINE_INPUT_EVENT_POINTER_UP:
            aether_artemis_mouse(s->runtime, static_cast<int32_t>(e->x),
                                static_cast<int32_t>(e->y));
            aether_artemis_button(s->runtime, e->button == 2 ? 2u : 1u,
                e->type == ENGINE_INPUT_EVENT_POINTER_DOWN ? 1 : 0);
            break;
        case ENGINE_INPUT_EVENT_POINTER_MOVE:
            aether_artemis_mouse(s->runtime, static_cast<int32_t>(e->x),
                                static_cast<int32_t>(e->y));
            break;
        case ENGINE_INPUT_EVENT_KEY_DOWN:
        case ENGINE_INPUT_EVENT_KEY_UP:
            if (e->key_code >= 0 && e->key_code < 256)
                aether_artemis_key(s->runtime, static_cast<uint32_t>(e->key_code),
                    e->type == ENGINE_INPUT_EVENT_KEY_DOWN ? 1 : 0);
            break;
        case ENGINE_INPUT_EVENT_BACK:
            aether_artemis_key(s->runtime, 27u, 1);
            aether_artemis_key(s->runtime, 27u, 0);
            break;
        case ENGINE_INPUT_EVENT_TEXT_INPUT:
        case ENGINE_INPUT_EVENT_POINTER_SCROLL: break;
        default: return ENGINE_RESULT_INVALID_ARGUMENT;
    }
    return ENGINE_RESULT_OK;
}
engine_result_t WasRendered(void* p, uint32_t* result) {
    Session* s = Cast(p);
    if (!s || !result) return ENGINE_RESULT_INVALID_ARGUMENT;
    *result = s->rendered ? 1u : 0u;
    return ENGINE_RESULT_OK;
}
engine_result_t RendererInfo(void* p, char* buffer, uint32_t cap) {
    if (!p) return ENGINE_RESULT_INVALID_ARGUMENT;
    return Copy("runtime=artemis renderer=art3m1s public_gpu=metal", buffer, cap);
}
engine_result_t DebugInfo(void* p, char* buffer, uint32_t cap, uint32_t* written) {
    Session* s = Cast(p);
    if (!s) return ENGINE_RESULT_INVALID_ARGUMENT;
    return Copy("runtime=artemis frames=" + std::to_string(s->serial) +
        " host_media=not_yet_wired", buffer, cap, written);
}
const char* LastError(void* p) {
    Session* s = Cast(p);
    if (!s) return "Artemis session not available";
    if (!s->error.empty()) return s->error.c_str();
    return aether_artemis_error(s->runtime);
}
engine_runtime_provider_v1_t MakeProvider() {
    engine_runtime_provider_v1_t p{};
    p.struct_size = sizeof(p);
    p.api_version = ENGINE_RUNTIME_PROVIDER_API_VERSION;
    p.runtime_id_utf8 = kName;
    p.display_name_utf8 = "Public Artemis / Art3m1s";
    p.priority = 91;
    p.probe = Probe;
    p.create = Create;
    p.destroy = Destroy;
    p.open_game = Open;
    p.tick = Tick;
    p.pause = Pause;
    p.resume = Resume;
    p.set_option = Option;
    p.set_surface_size = Resize;
    p.get_frame_desc = FrameDesc;
    p.read_frame_rgba = ReadFrame;
    p.send_input = Input;
    p.get_frame_rendered_flag = WasRendered;
    p.get_renderer_info = RendererInfo;
    p.get_plugin_debug_info = DebugInfo;
    p.get_last_error = LastError;
    return p;
}
} // namespace
extern "C" engine_result_t aetherkiri_artemis_register_runtime_provider() {
    static engine_runtime_provider_v1_t provider = MakeProvider();
    return engine_register_runtime_provider(&provider);
}
