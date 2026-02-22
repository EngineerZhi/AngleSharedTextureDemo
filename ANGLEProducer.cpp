/**
 * ANGLEProducer.cpp
 *
 * ANGLE (OpenGL ES) 侧实现：
 *   1. 初始化 EGL + 查询 ANGLE 内部 D3D11 设备
 *   2. 用 ANGLE 设备创建带 KeyedMutex 的共享纹理
 *   3. 通过 EGL_D3D_TEXTURE_ANGLE 将纹理包装为 PBuffer（GL 渲染目标）
 *   4. 每帧用 GLSL 渲染渐变色，通过 KeyedMutex 与消费方同步
 */

#include "SharedTextureDemo.h"
#include <cmath>
#include <cstdio>

// -----------------------------------------------------------------------
// EGL 扩展函数指针（需运行时获取）
// -----------------------------------------------------------------------
static PFNEGLQUERYDISPLAYATTRIBEXTPROC  s_eglQueryDisplayAttribEXT  = nullptr;
static PFNEGLQUERYDEVICEATTRIBEXTPROC   s_eglQueryDeviceAttribEXT   = nullptr;

static void LoadEGLExtensions(EGLDisplay dpy) {
    (void)dpy;
    s_eglQueryDisplayAttribEXT = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    s_eglQueryDeviceAttribEXT  = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDeviceAttribEXT"));
}

// -----------------------------------------------------------------------
// AngleEGLContext
// -----------------------------------------------------------------------
void AngleEGLContext::Init() {
    // 使用 D3D11 后端
    const EGLint displayAttribs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,     EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, EGL_DONT_CARE,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, EGL_DONT_CARE,
        EGL_NONE
    };

    display = eglGetPlatformDisplayEXT(
        EGL_PLATFORM_ANGLE_ANGLE,
        EGL_DEFAULT_DISPLAY,
        displayAttribs);
    EGL_CHECK(display != EGL_NO_DISPLAY, "eglGetPlatformDisplayEXT");

    EGLint major = 0, minor = 0;
    EGL_CHECK(eglInitialize(display, &major, &minor), "eglInitialize");

    printf("[ANGLE] EGL version: %d.%d\n", major, minor);

    // 加载扩展
    LoadEGLExtensions(display);

    // 选择 Config（BGRA8 渲染目标）
    const EGLint configAttribs[] = {
        EGL_RED_SIZE,     8,
        EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,    8,
        EGL_ALPHA_SIZE,   8,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLint numConfigs = 0;
    EGL_CHECK(eglChooseConfig(display, configAttribs, &config, 1, &numConfigs)
              && numConfigs > 0, "eglChooseConfig");

    // 创建 GLES2 Context
    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
    EGL_CHECK(context != EGL_NO_CONTEXT, "eglCreateContext");
}

void AngleEGLContext::Destroy() {
    if (display != EGL_NO_DISPLAY) {
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(display, context);
            context = EGL_NO_CONTEXT;
        }
        eglTerminate(display);
        display = EGL_NO_DISPLAY;
    }
}

ID3D11Device* AngleEGLContext::QueryInternalD3DDevice() const {
    if (!s_eglQueryDisplayAttribEXT || !s_eglQueryDeviceAttribEXT) {
        printf("[ANGLE] EGL_ANGLE_device_d3d extensions not available\n");
        return nullptr;
    }
    EGLAttrib deviceAttrib = 0;
    if (!s_eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &deviceAttrib)) {
        printf("[ANGLE] eglQueryDisplayAttribEXT failed\n");
        return nullptr;
    }
    EGLDeviceEXT eglDevice = reinterpret_cast<EGLDeviceEXT>(deviceAttrib);

    EGLAttrib d3dDeviceAttrib = 0;
    if (!s_eglQueryDeviceAttribEXT(eglDevice, EGL_D3D11_DEVICE_ANGLE, &d3dDeviceAttrib)) {
        printf("[ANGLE] eglQueryDeviceAttribEXT(EGL_D3D11_DEVICE_ANGLE) failed\n");
        return nullptr;
    }
    return reinterpret_cast<ID3D11Device*>(d3dDeviceAttrib);
}

// -----------------------------------------------------------------------
// GLSL 着色器（OpenGL ES 2.0）
// -----------------------------------------------------------------------
static const char* s_vsSource = R"(
attribute vec2 aPos;
varying   vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// 根据 UV 坐标 + 色相画出渐变
static const char* s_fsSource = R"(
precision mediump float;
varying vec2 vUV;
uniform float uHue;  // 0.0 ~ 1.0

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    float h = mod(uHue + vUV.x * 0.3 + vUV.y * 0.2, 1.0);
    vec3  rgb = hsv2rgb(vec3(h, 0.85, 0.9));
    gl_FragColor = vec4(rgb, 1.0);
}
)";

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        char msg[600];
        snprintf(msg, sizeof(msg), "Shader compile error: %s", log);
        glDeleteShader(s);
        throw std::runtime_error(msg);
    }
    return s;
}

// -----------------------------------------------------------------------
// ANGLEProducer 实现
// -----------------------------------------------------------------------
void ANGLEProducer::Init(const SharedTextureDesc& desc) {
    m_desc = desc;

    // 1. 初始化 EGL
    m_egl.Init();

    // 2. 获取 ANGLE 内部 D3D 设备
    m_angleDevice = m_egl.QueryInternalD3DDevice();
    if (!m_angleDevice) {
        throw std::runtime_error("Failed to get ANGLE internal D3D11 device");
    }
    printf("[ANGLE] Internal D3D11 device: %p\n", (void*)m_angleDevice);

    // 3. 用 ANGLE 设备创建共享纹理
    _CreateSharedTexture();

    // 4. 创建 EGL PBuffer Surface（将 D3D 纹理包装为 GL 渲染目标）
    _CreatePBufferSurface();

    // 5. 激活 Context，创建 GL 资源
    eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);
    EGL_CHECK(eglGetError() == EGL_SUCCESS, "eglMakeCurrent");

    _CompileShaders();
    _CreateQuadVBO();

    printf("[ANGLE] ANGLEProducer initialized (%ux%u)\n", desc.width, desc.height);
}

void ANGLEProducer::_CreateSharedTexture() {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = m_desc.width;
    td.Height    = m_desc.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = m_desc.format;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // SHARED_KEYEDMUTEX: 同进程跨设备共享 + 同步支持
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = m_angleDevice->CreateTexture2D(&td, nullptr, &m_sharedTex);
    HR_CHECK(hr, "CreateTexture2D (shared)");

    // 获取 KeyedMutex 接口
    hr = m_sharedTex->QueryInterface(IID_PPV_ARGS(&m_keyedMutex));
    HR_CHECK(hr, "QueryInterface IDXGIKeyedMutex");

    // 获取 Legacy Share Handle（同进程传值即可）
    IDXGIResource* dxgiRes = nullptr;
    hr = m_sharedTex->QueryInterface(IID_PPV_ARGS(&dxgiRes));
    HR_CHECK(hr, "QueryInterface IDXGIResource");

    hr = dxgiRes->GetSharedHandle(&m_shareHandle);
    dxgiRes->Release();
    HR_CHECK(hr, "GetSharedHandle");

    printf("[ANGLE] Shared texture created, handle=%p\n", m_shareHandle);
}

void ANGLEProducer::_CreatePBufferSurface() {
    // 验证 EGL_ANGLE_d3d_texture_client_buffer 扩展
    const char* exts = eglQueryString(m_egl.display, EGL_EXTENSIONS);
    if (!strstr(exts, "EGL_ANGLE_d3d_texture_client_buffer")) {
        throw std::runtime_error("Missing EGL_ANGLE_d3d_texture_client_buffer");
    }

    const EGLint pbAttribs[] = {
        EGL_WIDTH,          static_cast<EGLint>(m_desc.width),
        EGL_HEIGHT,         static_cast<EGLint>(m_desc.height),
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_NONE
    };

    // 直接用 D3D11 纹理指针作为 ClientBuffer
    m_pbuffer = eglCreatePbufferFromClientBuffer(
        m_egl.display,
        EGL_D3D_TEXTURE_ANGLE,
        reinterpret_cast<EGLClientBuffer>(m_sharedTex),
        m_egl.config,
        pbAttribs);

    EGL_CHECK(m_pbuffer != EGL_NO_SURFACE, "eglCreatePbufferFromClientBuffer");
    printf("[ANGLE] EGL PBuffer surface created\n");
}

void ANGLEProducer::_CompileShaders() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER,   s_vsSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, s_fsSource);

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glBindAttribLocation(m_program, 0, "aPos");
    glLinkProgram(m_program);

    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        glDeleteProgram(m_program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        throw std::runtime_error(std::string("Program link error: ") + log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
}

void ANGLEProducer::_CreateQuadVBO() {
    // 全屏三角形对（NDC 坐标）
    const float verts[] = {
        -1.f, -1.f,   1.f, -1.f,   -1.f,  1.f,
        -1.f,  1.f,   1.f, -1.f,    1.f,  1.f,
    };
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_CHECK("CreateQuadVBO");
}

void ANGLEProducer::RenderFrame(float hue) {
    // 获取 KeyedMutex（等待消费方用完，初始 key=0）
    HRESULT hr = m_keyedMutex->AcquireSync(0, 5000);
    HR_CHECK(hr, "KeyedMutex AcquireSync(0) [producer]");

    // --- GL 渲染 ---
    eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);

    glViewport(0, 0, static_cast<GLsizei>(m_desc.width),
                      static_cast<GLsizei>(m_desc.height));
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_program);
    GLint hueLoc = glGetUniformLocation(m_program, "uHue");
    glUniform1f(hueLoc, hue);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // 关键：确保 GPU 命令完全提交后再释放 Mutex
    glFinish();
    GL_CHECK("RenderFrame");

    // 释放 KeyedMutex，通知消费方可以读取（交接 key=1）
    hr = m_keyedMutex->ReleaseSync(1);
    HR_CHECK(hr, "KeyedMutex ReleaseSync(1) [producer]");
}

void ANGLEProducer::Destroy() {
    if (m_egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);

        if (m_vbo)     { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
        if (m_program) { glDeleteProgram(m_program);  m_program = 0; }

        if (m_pbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(m_egl.display, m_pbuffer);
            m_pbuffer = EGL_NO_SURFACE;
        }
    }
    if (m_keyedMutex) { m_keyedMutex->Release(); m_keyedMutex = nullptr; }
    if (m_sharedTex)  { m_sharedTex->Release();  m_sharedTex  = nullptr; }

    m_egl.Destroy();
    m_shareHandle = nullptr;
    m_angleDevice = nullptr;
}
