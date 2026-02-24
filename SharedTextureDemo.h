#pragma once

/**
 * SharedTextureDemo.h
 * ANGLE EGL -> D3D11 共享纹理 Demo
 *
 * 演示：ANGLE (OpenGL ES) 渲染写入共享纹理，D3D11 原生设备读取该纹理
 *
 * 流程：
 *   1. ANGLEProducer::Init()
 *      - 初始化 EGL Display/Context
 *      - 查询 ANGLE 内部 D3D11 设备
 *      - 用 ANGLE 设备创建带 SHARED_KEYEDMUTEX 标志的 D3D11 共享纹理
 *      - 用纹理指针创建 EGL PBuffer Surface 作为 GL 渲染目标
 *   2. D3D11Consumer::Init() + OpenSharedTexture(handle)
 *      - 创建独立 D3D11 设备
 *      - OpenSharedResource 导入共享纹理
 *   3. 每帧循环：
 *      - Producer  AcquireSync(key=0) → GL渲染 → glFinish → ReleaseSync(key=1)
 *      - Consumer  AcquireSync(key=1) → 使用纹理 → ReleaseSync(key=0)
 *
 * 依赖：
 *   - ANGLE (libEGL.dll / libEGL.lib, libGLESv2.dll / libGLESv2.lib)
 *   - DirectX 11 SDK (d3d11.lib, dxgi.lib)
 *   - Windows SDK
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// 启用 EGL 扩展函数的原型声明（例如 eglGetPlatformDisplayEXT）
#ifndef EGL_EGLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

// ANGLE EGL / GLES headers (chromium/angle 仓库的 include 目录)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// -----------------------------------------------------------------------
// 辅助宏
// -----------------------------------------------------------------------
#define HR_CHECK(hr, msg)                                                         \
    do {                                                                          \
        if (FAILED(hr)) {                                                         \
            char _buf[256];                                                       \
            snprintf(_buf, sizeof(_buf), "[D3D] %s  HRESULT=0x%08X",             \
                     (msg), static_cast<unsigned>(hr));                           \
            throw std::runtime_error(_buf);                                       \
        }                                                                         \
    } while (0)

#define EGL_CHECK(cond, msg)                                                      \
    do {                                                                          \
        if (!(cond)) {                                                            \
            char _buf[256];                                                       \
            snprintf(_buf, sizeof(_buf), "[EGL] %s  error=0x%04X",               \
                     (msg), static_cast<unsigned>(eglGetError()));                \
            throw std::runtime_error(_buf);                                       \
        }                                                                         \
    } while (0)

#define GL_CHECK(msg)                                                             \
    do {                                                                          \
        GLenum _err = glGetError();                                               \
        if (_err != GL_NO_ERROR) {                                                \
            char _buf[256];                                                       \
            snprintf(_buf, sizeof(_buf), "[GL] %s  error=0x%04X", (msg),         \
                     static_cast<unsigned>(_err));                                \
            throw std::runtime_error(_buf);                                       \
        }                                                                         \
    } while (0)

// -----------------------------------------------------------------------
// 共享纹理描述
// -----------------------------------------------------------------------
struct SharedTextureDesc {
    UINT        width  = 1280;
    UINT        height = 720;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
};

// -----------------------------------------------------------------------
// ANGLE EGL 环境封装
// -----------------------------------------------------------------------
class AngleEGLContext {
public:
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig  config  = nullptr;

    /// 初始化 EGL Display / Context
    void Init();
    void Destroy();

    /// 通过 EGL_ANGLE_device_d3d 扩展查询 ANGLE 内部 D3D11 设备（不 AddRef）
    ID3D11Device* QueryInternalD3DDevice() const;
};

// -----------------------------------------------------------------------
// ANGLEProducer：OpenGL ES 渲染，输出到共享纹理
// -----------------------------------------------------------------------
class ANGLEProducer {
public:
    ANGLEProducer()  = default;
    ~ANGLEProducer() { Destroy(); }

    void Init(const SharedTextureDesc& desc);
    void Destroy();

    void LoadImageFromFile(const char* path);
    void RenderWithRoundedCorners(float cornerRadius);
    void RenderGradient();

    HANDLE GetShareHandle() const { return m_shareHandle; }

private:
    void _CreateSharedTexture();
    void _CreatePBufferSurface();
    void _CompileShaders();
    void _CreateQuadVBO();

    AngleEGLContext  m_egl;
    ID3D11Device*    m_angleDevice = nullptr;  // 借用，不持有引用
    ID3D11Texture2D* m_sharedTex   = nullptr;
    IDXGIKeyedMutex* m_keyedMutex  = nullptr;
    HANDLE           m_shareHandle = nullptr;
    EGLSurface       m_pbuffer     = EGL_NO_SURFACE;

    GLuint m_vbo     = 0;
    GLuint m_program = 0;
    GLuint m_roundedProgram = 0;
    GLuint m_imageTexture = 0;

    SharedTextureDesc m_desc;
};

// -----------------------------------------------------------------------
// D3D11Consumer：导入并读取共享纹理
// -----------------------------------------------------------------------
class D3D11Consumer {
public:
    D3D11Consumer()  = default;
    ~D3D11Consumer() { Destroy(); }

    void Init();
    void Destroy();

    void OpenSharedTexture(HANDLE shareHandle, const SharedTextureDesc& desc);

    std::vector<uint8_t> ConsumeFrame();

    void SaveToPNG(const char* path);

    void BindSRV(UINT slot = 0);

    ID3D11Device*        GetDevice()  const { return m_device;  }
    ID3D11DeviceContext* GetContext() const { return m_context; }

private:
    ID3D11Device*             m_device      = nullptr;
    ID3D11DeviceContext*      m_context     = nullptr;
    ID3D11Texture2D*          m_importedTex = nullptr;
    ID3D11Texture2D*          m_stagingTex  = nullptr;
    ID3D11ShaderResourceView* m_srv         = nullptr;
    IDXGIKeyedMutex*          m_keyedMutex  = nullptr;
    SharedTextureDesc         m_desc;
};
