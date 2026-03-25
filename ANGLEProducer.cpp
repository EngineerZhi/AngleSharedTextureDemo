/**
 * ANGLEProducer.cpp
 * ANGLE 渲染到共享纹理
 */

#include "SharedTextureDemo.h"
#include "PerfettoTracing.h"
#include <cmath>
#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"

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
    TRACE_EVENT("angle", "AngleEGLContext::Init");

    // 使用 D3D11 后端
    const EGLint displayAttribs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,     EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, EGL_DONT_CARE,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, EGL_DONT_CARE,
        EGL_NONE
    };

    {
        TRACE_EVENT("angle", "eglGetPlatformDisplayEXT");
        display = eglGetPlatformDisplayEXT(
            EGL_PLATFORM_ANGLE_ANGLE,
            EGL_DEFAULT_DISPLAY,
            displayAttribs);
    }
    EGL_CHECK(display != EGL_NO_DISPLAY, "eglGetPlatformDisplayEXT");

    EGLint major = 0, minor = 0;
    {
        TRACE_EVENT("angle", "eglInitialize");
        EGL_CHECK(eglInitialize(display, &major, &minor), "eglInitialize");
    }

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
    {
        TRACE_EVENT("angle", "eglChooseConfig");
        EGL_CHECK(eglChooseConfig(display, configAttribs, &config, 1, &numConfigs)
                  && numConfigs > 0, "eglChooseConfig");
    }

    // 创建 GLES2 Context
    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    {
        TRACE_EVENT("angle", "eglCreateContext");
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
    }
    EGL_CHECK(context != EGL_NO_CONTEXT, "eglCreateContext");
}

void AngleEGLContext::Destroy() {
    TRACE_EVENT("angle", "AngleEGLContext::Destroy");

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
    TRACE_EVENT("angle", "AngleEGLContext::QueryInternalD3DDevice");

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
varying vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* s_fsGradient = R"(
precision mediump float;
varying vec2 vUV;
void main() {
    gl_FragColor = vec4(vUV.x, vUV.y, 0.5, 1.0);
}
)";

static const char* s_fsRoundedCorner = R"(
precision mediump float;
varying vec2 vUV;
uniform sampler2D uTexture;
uniform float uCornerRadius;
uniform vec2 uResolution;

void main() {
    vec2 pixelCoord = vUV * uResolution;
    float radius = uCornerRadius;
    
    float alpha = 1.0;
    
    vec2 topLeft = vec2(radius, radius);
    vec2 topRight = vec2(uResolution.x - radius, radius);
    vec2 bottomLeft = vec2(radius, uResolution.y - radius);
    vec2 bottomRight = vec2(uResolution.x - radius, uResolution.y - radius);
    
    if (pixelCoord.x < radius && pixelCoord.y < radius) {
        float dist = distance(pixelCoord, topLeft);
        alpha = 1.0 - smoothstep(radius - 1.0, radius, dist);
    }
    else if (pixelCoord.x > uResolution.x - radius && pixelCoord.y < radius) {
        float dist = distance(pixelCoord, topRight);
        alpha = 1.0 - smoothstep(radius - 1.0, radius, dist);
    }
    else if (pixelCoord.x < radius && pixelCoord.y > uResolution.y - radius) {
        float dist = distance(pixelCoord, bottomLeft);
        alpha = 1.0 - smoothstep(radius - 1.0, radius, dist);
    }
    else if (pixelCoord.x > uResolution.x - radius && pixelCoord.y > uResolution.y - radius) {
        float dist = distance(pixelCoord, bottomRight);
        alpha = 1.0 - smoothstep(radius - 1.0, radius, dist);
    }
    
    vec4 texColor = texture2D(uTexture, vUV);
    gl_FragColor = vec4(texColor.rgb, texColor.a * alpha);
}
)";

static GLuint CompileShader(GLenum type, const char* src) {
    TRACE_EVENT("angle", "CompileShader", "shader_type", static_cast<int>(type));

    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        printf("[ANGLE] Shader compile failed (type=%d):\n%s\n", type, log);
        printf("[ANGLE] Shader source:\n%s\n", src);
        glDeleteShader(s);
        throw std::runtime_error(std::string("Shader compile error: ") + log);
    }
    return s;
}

// -----------------------------------------------------------------------
// ANGLEProducer 实现
// -----------------------------------------------------------------------
void ANGLEProducer::Init(const SharedTextureDesc& desc) {
    TRACE_EVENT("angle", "ANGLEProducer::Init",
                "width", static_cast<int>(desc.width),
                "height", static_cast<int>(desc.height));

    m_desc = desc;

    m_egl.Init();

    m_angleDevice = m_egl.QueryInternalD3DDevice();
    if (!m_angleDevice) {
        throw std::runtime_error("Failed to get ANGLE internal D3D11 device");
    }
    printf("[ANGLE] Internal D3D11 device: %p\n", (void*)m_angleDevice);

    _CreateSharedTexture();
    _CreatePBufferSurface();

    eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);
    EGL_CHECK(eglGetError() == EGL_SUCCESS, "eglMakeCurrent");

    _CompileShaders();
    _CreateQuadVBO();

    printf("[ANGLE] ANGLEProducer initialized (%ux%u)\n", desc.width, desc.height);
}

void ANGLEProducer::_CreateSharedTexture() {
    TRACE_EVENT("angle", "ANGLEProducer::_CreateSharedTexture",
                "width", static_cast<int>(m_desc.width),
                "height", static_cast<int>(m_desc.height));

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = m_desc.width;
    td.Height    = m_desc.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = m_desc.format;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = S_OK;
    {
        TRACE_EVENT("angle", "D3D11::CreateTexture2D");
        hr = m_angleDevice->CreateTexture2D(&td, nullptr, &m_sharedTex);
    }
    HR_CHECK(hr, "CreateTexture2D (shared)");

    {
        TRACE_EVENT("angle", "QueryInterface::IDXGIKeyedMutex");
        hr = m_sharedTex->QueryInterface(IID_PPV_ARGS(&m_keyedMutex));
    }
    HR_CHECK(hr, "QueryInterface IDXGIKeyedMutex");

    IDXGIResource* dxgiRes = nullptr;
    {
        TRACE_EVENT("angle", "QueryInterface::IDXGIResource");
        hr = m_sharedTex->QueryInterface(IID_PPV_ARGS(&dxgiRes));
    }
    HR_CHECK(hr, "QueryInterface IDXGIResource");

    {
        TRACE_EVENT("angle", "IDXGIResource::GetSharedHandle");
        hr = dxgiRes->GetSharedHandle(&m_shareHandle);
    }
    dxgiRes->Release();
    HR_CHECK(hr, "GetSharedHandle");
    
    hr = m_keyedMutex->AcquireSync(0, 0);
    if (SUCCEEDED(hr)) {
        m_keyedMutex->ReleaseSync(0);
        printf("[ANGLE] KeyedMutex initialized to key=0\n");
    } else {
        printf("[ANGLE] KeyedMutex initial state unknown\n");
    }

    printf("[ANGLE] Shared texture created, handle=%p\n", m_shareHandle);
}

void ANGLEProducer::_CreatePBufferSurface() {
    TRACE_EVENT("angle", "ANGLEProducer::_CreatePBufferSurface");

    const char* exts = eglQueryString(m_egl.display, EGL_EXTENSIONS);
    if (!strstr(exts, "EGL_ANGLE_d3d_texture_client_buffer")) {
        throw std::runtime_error("Missing EGL_ANGLE_d3d_texture_client_buffer");
    }

    const EGLint pbAttribs[] = {
        EGL_WIDTH,          static_cast<EGLint>(m_desc.width),
        EGL_HEIGHT,         static_cast<EGLint>(m_desc.height),
        EGL_NONE
    };

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
    TRACE_EVENT("angle", "ANGLEProducer::_CompileShaders");

    GLuint vs = CompileShader(GL_VERTEX_SHADER, s_vsSource);
    GLuint fsGrad = CompileShader(GL_FRAGMENT_SHADER, s_fsGradient);
    GLuint fsRounded = CompileShader(GL_FRAGMENT_SHADER, s_fsRoundedCorner);

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fsGrad);
    glBindAttribLocation(m_program, 0, "aPos");
    glLinkProgram(m_program);

    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        printf("[ANGLE] Gradient program link error: %s\n", log);
        throw std::runtime_error(std::string("Program link error: ") + log);
    }
    
    m_roundedProgram = glCreateProgram();
    glAttachShader(m_roundedProgram, vs);
    glAttachShader(m_roundedProgram, fsRounded);
    glBindAttribLocation(m_roundedProgram, 0, "aPos");
    glLinkProgram(m_roundedProgram);
    
    glGetProgramiv(m_roundedProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_roundedProgram, sizeof(log), nullptr, log);
        printf("[ANGLE] Rounded program link error: %s\n", log);
        throw std::runtime_error(std::string("Rounded program link error: ") + log);
    }
    
    printf("[ANGLE] Shaders compiled: gradient=%u, rounded=%u\n", m_program, m_roundedProgram);
    
    glDeleteShader(vs);
    glDeleteShader(fsGrad);
    glDeleteShader(fsRounded);
}

void ANGLEProducer::_CreateQuadVBO() {
    TRACE_EVENT("angle", "ANGLEProducer::_CreateQuadVBO");

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

void ANGLEProducer::LoadImageFromFile(const char* path) {
    TRACE_EVENT("io", "ANGLEProducer::LoadImageFromFile", "path", path);

    int width, height, channels;
    unsigned char* data = nullptr;
    {
        TRACE_EVENT("io", "stbi_load", "path", path);
        data = stbi_load(path, &width, &height, &channels, 4);
    }
    if (!data) {
        throw std::runtime_error(std::string("Failed to load image: ") + path);
    }
    
    EGLBoolean result = EGL_FALSE;
    {
        TRACE_EVENT("angle", "eglMakeCurrent::LoadImage");
        result = eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);
    }
    if (!result) {
        stbi_image_free(data);
        throw std::runtime_error("eglMakeCurrent failed in LoadImageFromFile");
    }
    
    if (m_imageTexture != 0) {
        glDeleteTextures(1, &m_imageTexture);
        m_imageTexture = 0;
    }
    
    glGenTextures(1, &m_imageTexture);
    GLenum err = glGetError();
    
    if (m_imageTexture == 0 || err != GL_NO_ERROR) {
        stbi_image_free(data);
        char msg[256];
        snprintf(msg, sizeof(msg), "glGenTextures failed: ID=%u, error=0x%04X", m_imageTexture, err);
        throw std::runtime_error(msg);
    }
    
    {
        TRACE_EVENT("angle", "UploadImageTexture",
                    "image_width", width,
                    "image_height", height);
        glBindTexture(GL_TEXTURE_2D, m_imageTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    
    {
        TRACE_EVENT("angle", "glFinish::LoadImage");
        glFinish();
    }
    
    // 解绑EGL上下文，避免纹理状态在下次MakeCurrent时丢失
    eglMakeCurrent(m_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    
    stbi_image_free(data);
    
    printf("[ANGLE] Image loaded: %s (%dx%d → texture ID=%u)\n", path, width, height, m_imageTexture);
}

void ANGLEProducer::RenderWithRoundedCorners(float cornerRadius) {
    TRACE_EVENT("angle", "ANGLEProducer::RenderWithRoundedCorners",
                "corner_radius", cornerRadius,
                "width", static_cast<int>(m_desc.width),
                "height", static_cast<int>(m_desc.height));

    if (m_imageTexture == 0) {
        throw std::runtime_error("No image loaded! Call LoadImageFromFile() first.");
    }
    
    float maxRadius = std::min(m_desc.width, m_desc.height) / 2.0f;
    if (cornerRadius > maxRadius) {
        printf("[ANGLE WARNING] cornerRadius=%.1f exceeds max=%.1f, clamping\n", 
               cornerRadius, maxRadius);
        cornerRadius = maxRadius;
    }
    
    {
        TRACE_EVENT("angle", "eglMakeCurrent::Render");
        eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);
    }
    
    TRACE_COUNTER("angle", "CornerRadius", cornerRadius);

    HRESULT hr = S_OK;
    {
        TRACE_EVENT("sync", "KeyedMutex::AcquireSync[producer]");
        hr = m_keyedMutex->AcquireSync(0, INFINITE);
    }
    HR_CHECK(hr, "KeyedMutex AcquireSync(0)");

    {
        TRACE_EVENT("angle", "DrawRoundedCorners");
        glViewport(0, 0, m_desc.width, m_desc.height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(m_roundedProgram);
        
        GLint texLoc = glGetUniformLocation(m_roundedProgram, "uTexture");
        GLint radiusLoc = glGetUniformLocation(m_roundedProgram, "uCornerRadius");
        GLint resLoc = glGetUniformLocation(m_roundedProgram, "uResolution");
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_imageTexture);
        glUniform1i(texLoc, 0);
        glUniform1f(radiusLoc, cornerRadius);
        glUniform2f(resLoc, static_cast<float>(m_desc.width), static_cast<float>(m_desc.height));
        
        printf("[ANGLE] Rendering: resolution=(%u,%u), cornerRadius=%.1f\n",
               m_desc.width, m_desc.height, cornerRadius);

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    {
        TRACE_EVENT("angle", "glFinish::Render");
        glFinish();
    }
    GL_CHECK("RenderWithRoundedCorners");

    {
        TRACE_EVENT("sync", "KeyedMutex::ReleaseSync[producer]");
        hr = m_keyedMutex->ReleaseSync(1);
    }
    HR_CHECK(hr, "KeyedMutex ReleaseSync(1)");
    
    printf("[ANGLE] Rendered with rounded corners (radius=%.1f)\n", cornerRadius);
}

void ANGLEProducer::RenderGradient() {
    TRACE_EVENT("angle", "ANGLEProducer::RenderGradient");

    HRESULT hr = S_OK;
    {
        TRACE_EVENT("sync", "KeyedMutex::AcquireSync[producer]");
        hr = m_keyedMutex->AcquireSync(0, INFINITE);
    }
    HR_CHECK(hr, "KeyedMutex AcquireSync(0) [producer]");

    {
        TRACE_EVENT("angle", "eglMakeCurrent::Gradient");
        eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);
    }

    {
        TRACE_EVENT("angle", "DrawGradient");
        glViewport(0, 0, m_desc.width, m_desc.height);
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_program);

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    {
        TRACE_EVENT("angle", "glFinish::Gradient");
        glFinish();
    }
    GL_CHECK("RenderGradient");

    {
        TRACE_EVENT("sync", "KeyedMutex::ReleaseSync[producer]");
        hr = m_keyedMutex->ReleaseSync(1);
    }
    HR_CHECK(hr, "KeyedMutex ReleaseSync(1) [producer]");
    
    printf("[ANGLE] Gradient rendered\n");
}

void ANGLEProducer::Destroy() {
    TRACE_EVENT("angle", "ANGLEProducer::Destroy");

    if (m_egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_egl.display, m_pbuffer, m_pbuffer, m_egl.context);

        if (m_imageTexture)   { glDeleteTextures(1, &m_imageTexture); m_imageTexture = 0; }
        if (m_vbo)            { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
        if (m_program)        { glDeleteProgram(m_program);  m_program = 0; }
        if (m_roundedProgram) { glDeleteProgram(m_roundedProgram); m_roundedProgram = 0; }

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
