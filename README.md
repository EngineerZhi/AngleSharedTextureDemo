# ANGLE EGL 共享纹理：OpenGL ES 渲染 → D3D11 读取

> **适用场景**：同进程、跨线程、跨 D3D 设备，ANGLE 作为渲染生产方，原生 D3D11 作为消费方。

---

## 目录

1. [架构总览](#1-架构总览)
2. [核心原理](#2-核心原理)
3. [环境依赖](#3-环境依赖)
4. [完整流程详解](#4-完整流程详解)
   - 4.1 [初始化 ANGLE EGL 环境](#41-初始化-angle-egl-环境)
   - 4.2 [查询 ANGLE 内部 D3D11 设备](#42-查询-angle-内部-d3d11-设备)
   - 4.3 [创建共享纹理](#43-创建共享纹理)
   - 4.4 [创建 EGL PBuffer 渲染目标](#44-创建-egl-pbuffer-渲染目标)
   - 4.5 [D3D 消费方导入纹理](#45-d3d-消费方导入纹理)
   - 4.6 [每帧同步渲染与读取](#46-每帧同步渲染与读取)
5. [KeyedMutex 同步机制](#5-keyedmutex-同步机制)
6. [代码结构说明](#6-代码结构说明)
7. [构建方法](#7-构建方法)
8. [常见问题排查](#8-常见问题排查)
9. [扩展与进阶](#9-扩展与进阶)
10. [API 速查表](#10-api-速查表)

---

## 1. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                         同一进程                             │
│                                                             │
│  ┌──────────────────────┐      ┌───────────────────────┐   │
│  │   ANGLEProducer      │      │   D3D11Consumer       │   │
│  │                      │      │                       │   │
│  │  ┌───────────────┐   │      │  ┌─────────────────┐  │   │
│  │  │ ANGLE 内部    │   │      │  │ 消费方 D3D11    │  │   │
│  │  │ D3D11 Device  │   │      │  │ Device          │  │   │
│  │  └───────┬───────┘   │      │  └────────┬────────┘  │   │
│  │          │ 创建       │      │           │ 打开       │   │
│  │  ┌───────▼───────┐   │      │  ┌────────▼────────┐  │   │
│  │  │  共享纹理      │◄─────────►  │  同一纹理        │  │   │
│  │  │ (D3D Texture) │   │HANDLE│  │ (另一接口指针)   │  │   │
│  │  └───────┬───────┘   │      │  └────────┬────────┘  │   │
│  │          │            │      │           │            │   │
│  │  ┌───────▼───────┐   │      │  ┌────────▼────────┐  │   │
│  │  │  EGL PBuffer  │   │      │  │  SRV / Staging  │  │   │
│  │  │  (GL渲染目标) │   │      │  │  (GPU/CPU读取)  │  │   │
│  │  └───────────────┘   │      │  └─────────────────┘  │   │
│  │                      │      │                       │   │
│  │  KeyedMutex          │      │  KeyedMutex           │   │
│  │  AcquireSync(0)      │      │  AcquireSync(1)       │   │
│  │  GL渲染 + glFinish   │      │  读取像素             │   │
│  │  ReleaseSync(1) ─────┼──────┼► ReleaseSync(0)      │   │
│  └──────────────────────┘      └───────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 核心原理

### 为什么必须用 ANGLE 内部设备创建纹理？

ANGLE 渲染时，内部维护一个 D3D11 设备和对应的 D3D11 渲染目标视图（RTV）。  
若纹理由 **外部设备** 创建，ANGLE 无法为其创建 RTV，`eglCreatePbufferFromClientBuffer` 会返回 `EGL_BAD_PARAMETER`。

**正确做法**：
1. 通过 `EGL_ANGLE_device_d3d` 扩展拿到 ANGLE 内部 D3D11 设备指针
2. 用该设备创建共享纹理
3. 将纹理指针传给 `eglCreatePbufferFromClientBuffer`

### 为什么要用 KeyedMutex？

跨 D3D 设备访问同一纹理时，GPU 命令在不同设备的命令队列中互相不可见。  
`IDXGIKeyedMutex` 是 DXGI 提供的 GPU 级同步原语，确保：

- **Producer 释放锁前**：GPU 渲染命令 100% 提交完毕（`glFinish()` 保障）
- **Consumer 获取锁后**：纹理数据已完全可见

---

## 3. 环境依赖

| 依赖 | 版本要求 | 说明 |
|------|---------|------|
| ANGLE | 任意现代版本（推荐 Chromium 维护版） | 需 D3D11 后端 + `EGL_ANGLE_d3d_texture_client_buffer` 扩展 |
| Windows SDK | 10.0+ | D3D11 / DXGI 头文件 |
| MSVC | VS 2019+ | C++17 支持 |
| CMake | 3.16+ | 构建系统 |

### 必须的 EGL 扩展

```
EGL_ANGLE_platform_angle_d3d          -- 使用 D3D 后端
EGL_ANGLE_device_d3d                  -- 查询内部 D3D 设备
EGL_ANGLE_d3d_texture_client_buffer   -- D3D 纹理 → EGL PBuffer
EGL_EXT_device_query                  -- eglQueryDeviceAttribEXT
```

运行时验证：

```cpp
const char* exts = eglQueryString(display, EGL_EXTENSIONS);
bool supported = (strstr(exts, "EGL_ANGLE_d3d_texture_client_buffer") != nullptr);
```

---

## 4. 完整流程详解

### 4.1 初始化 ANGLE EGL 环境

指定使用 D3D11 后端，通过 `eglGetPlatformDisplayEXT` 创建 Display：

```cpp
const EGLint displayAttribs[] = {
    EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
    EGL_NONE
};
EGLDisplay display = eglGetPlatformDisplayEXT(
    EGL_PLATFORM_ANGLE_ANGLE,
    EGL_DEFAULT_DISPLAY,
    displayAttribs);
eglInitialize(display, &major, &minor);
```

选择支持 BGRA8 + PBUFFER 的 Config：

```cpp
const EGLint configAttribs[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
};
```

### 4.2 查询 ANGLE 内部 D3D11 设备

```cpp
// 需先获取扩展函数指针
PFNEGLQUERYDISPLAYATTRIBEXTPROC eglQueryDisplayAttribEXT =
    (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
PFNEGLQUERYDEVICEATTRIBEXTPROC eglQueryDeviceAttribEXT =
    (PFNEGLQUERYDEVICEATTRIBEXTPROC)eglGetProcAddress("eglQueryDeviceAttribEXT");

// 查询 EGLDevice
EGLAttrib deviceAttrib = 0;
eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &deviceAttrib);
EGLDeviceEXT eglDevice = (EGLDeviceEXT)deviceAttrib;

// 查询 D3D11 设备指针
EGLAttrib d3dAttrib = 0;
eglQueryDeviceAttribEXT(eglDevice, EGL_D3D11_DEVICE_ANGLE, &d3dAttrib);
ID3D11Device* angleDevice = (ID3D11Device*)d3dAttrib;
// ⚠️ 不要 AddRef / Release 这个指针，它由 ANGLE 管理
```

### 4.3 创建共享纹理

**必须**用 ANGLE 设备创建，**必须**加 `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` 标志：

```cpp
D3D11_TEXTURE2D_DESC desc = {};
desc.Width     = 1280;
desc.Height    = 720;
desc.MipLevels = 1;
desc.ArraySize = 1;
desc.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;  // ✅ BGRA，ANGLE 优先支持
desc.SampleDesc.Count = 1;
desc.Usage     = D3D11_USAGE_DEFAULT;
desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;  // ✅ 跨设备+同步

ID3D11Texture2D* sharedTex = nullptr;
angleDevice->CreateTexture2D(&desc, nullptr, &sharedTex);

// 获取 KeyedMutex
IDXGIKeyedMutex* keyedMutex = nullptr;
sharedTex->QueryInterface(IID_PPV_ARGS(&keyedMutex));

// 获取 Share Handle（同进程直接传值）
IDXGIResource* dxgiRes = nullptr;
sharedTex->QueryInterface(IID_PPV_ARGS(&dxgiRes));
HANDLE shareHandle = nullptr;
dxgiRes->GetSharedHandle(&shareHandle);
dxgiRes->Release();
```

### 4.4 创建 EGL PBuffer 渲染目标

将 D3D11 纹理指针包装为 EGL Surface，作为 GL 默认帧缓冲：

```cpp
const EGLint pbAttribs[] = {
    EGL_WIDTH,          1280,
    EGL_HEIGHT,         720,
    EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
    EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
    EGL_NONE
};

EGLSurface pbuffer = eglCreatePbufferFromClientBuffer(
    display,
    EGL_D3D_TEXTURE_ANGLE,                       // ← 关键 target 类型
    reinterpret_cast<EGLClientBuffer>(sharedTex), // ← D3D11 纹理指针
    config,
    pbAttribs);

// 绑定为当前渲染目标
eglMakeCurrent(display, pbuffer, pbuffer, context);
```

### 4.5 D3D 消费方导入纹理

```cpp
// 消费方用自己的 D3D11 设备打开共享纹理
ID3D11Texture2D* importedTex = nullptr;
consumerDevice->OpenSharedResource(
    shareHandle,           // ← 从 Producer 传来的 Handle
    IID_PPV_ARGS(&importedTex));

// 同样获取 KeyedMutex（是同一个 mutex，不同设备侧的接口）
IDXGIKeyedMutex* consumerMutex = nullptr;
importedTex->QueryInterface(IID_PPV_ARGS(&consumerMutex));

// 创建 SRV（GPU 端采样用）
ID3D11ShaderResourceView* srv = nullptr;
consumerDevice->CreateShaderResourceView(importedTex, nullptr, &srv);

// 创建 Staging 纹理（CPU 回读用）
D3D11_TEXTURE2D_DESC stagingDesc = {};
stagingDesc.Width          = 1280;
stagingDesc.Height         = 720;
stagingDesc.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
stagingDesc.MipLevels = stagingDesc.ArraySize = stagingDesc.SampleDesc.Count = 1;
stagingDesc.Usage          = D3D11_USAGE_STAGING;
stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
// ⚠️ Staging 纹理不能有 MiscFlags（无法共享）

ID3D11Texture2D* stagingTex = nullptr;
consumerDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
```

### 4.6 每帧同步渲染与读取

```
KeyedMutex 状态机：
  key=0  →  Producer 可以写（初始状态）
  key=1  →  Consumer 可以读
```

**ANGLE Producer（渲染线程）**：

```cpp
// 等待消费方用完（或初始状态）
producerMutex->AcquireSync(0, INFINITE);

// GL 渲染
eglMakeCurrent(display, pbuffer, pbuffer, context);
glViewport(0, 0, 1280, 720);
// ... 渲染命令 ...

// ⚠️ 必须 glFinish，确保 GPU 命令全部完成
glFinish();

// 交给消费方
producerMutex->ReleaseSync(1);
```

**D3D11 Consumer（读取线程）**：

```cpp
// 等待 ANGLE 渲染完成
consumerMutex->AcquireSync(1, INFINITE);

// GPU 端使用：绑定 SRV
context->PSSetShaderResources(0, 1, &srv);

// 或 CPU 端回读
context->CopyResource(stagingTex, importedTex);
D3D11_MAPPED_SUBRESOURCE mapped;
context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
// ... 读取 mapped.pData ...
context->Unmap(stagingTex, 0);

// 归还给 Producer
consumerMutex->ReleaseSync(0);
```

---

## 5. KeyedMutex 同步机制

### 状态转移图

```
         Producer               Consumer
           │                      │
    ┌──────▼──────┐         ┌─────▼──────┐
    │AcquireSync(0)│         │            │
    │  等待key=0   │         │            │
    └──────┬──────┘         │            │
           │                │            │
    ┌──────▼──────┐         │            │
    │  GL渲染      │         │            │
    │  glFinish() │         │            │
    └──────┬──────┘         │            │
           │                │            │
    ┌──────▼──────┐         │            │
    │ReleaseSync(1)│──key=1─►│AcquireSync(1)│
    └─────────────┘         └─────┬──────┘
                                  │
                            ┌─────▼──────┐
                            │ 读取/使用   │
                            │ 纹理数据    │
                            └─────┬──────┘
                                  │
    ┌─────────────┐         ┌─────▼──────┐
    │AcquireSync(0)│◄─key=0──│ReleaseSync(0)│
    └─────────────┘         └────────────┘
```

### 超时处理

实际项目中应处理超时情况：

```cpp
HRESULT hr = mutex->AcquireSync(key, 100 /*ms*/);
if (hr == WAIT_TIMEOUT) {
    // 对方可能卡死，跳过本帧或重试
    return false;
}
if (FAILED(hr)) {
    // 设备丢失等错误
    HandleDeviceLost();
}
```

---

## 6. 代码结构说明

```
angle_shared_texture_demo/
├── SharedTextureDemo.h    # 头文件：类声明 + 辅助宏
├── ANGLEProducer.cpp      # ANGLE 侧实现（EGL + GL ES 渲染）
├── D3D11Consumer.cpp      # D3D11 侧实现（导入 + 读取）
├── main.cpp               # 入口：顺序模式 + 多线程模式演示
└── CMakeLists.txt         # CMake 构建文件
```

### 类职责

| 类 | 职责 |
|----|------|
| `AngleEGLContext` | 管理 EGLDisplay / EGLContext 的生命周期，提供查询内部 D3D 设备的接口 |
| `ANGLEProducer` | 创建共享纹理，包装为 PBuffer，执行 GL ES 渲染，通过 KeyedMutex 发布 |
| `D3D11Consumer` | 打开共享纹理，提供 SRV 绑定和 CPU 回读，通过 KeyedMutex 消费 |

---

## 7. 构建方法

### 前提：获取 ANGLE

**方法 A：使用 Chromium 预编译版**

```bash
# 从 Chromium 构建机或 Google Storage 下载
# 或者从 Qt 等包含预编译 ANGLE 的 SDK 中提取
```

**方法 B：自行编译 ANGLE**

```bash
git clone https://chromium.googlesource.com/angle/angle
cd angle
python scripts/bootstrap.py
gclient sync
gn gen out/Release --args="is_debug=false angle_enable_d3d11=true"
ninja -C out/Release libEGL libGLESv2
```

### CMake 构建

```bash
mkdir build && cd build
cmake .. -DANGLE_DIR="C:/path/to/angle/out/Release" -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

构建产物在 `build/Release/`：
- `ANGLESharedTextureDemo.exe`
- `libEGL.dll`（自动拷贝）
- `libGLESv2.dll`（自动拷贝）

### 运行

```bash
cd build/Release
ANGLESharedTextureDemo.exe
# 成功输出：
# [ANGLE] EGL version: 1.5
# [ANGLE] Internal D3D11 device: 0x000001A2B3C4D5E6
# [Frame 0] hue=0.00
#   Center pixel BGRA = (xxx, yyy, zzz, 255)
#   Pixel verify: PASS
#   Saved frame0.bmp: OK
# ...
# All demos completed successfully.
```

---

## 8. 常见问题排查

### `eglCreatePbufferFromClientBuffer` 返回 `EGL_NO_SURFACE`

| 原因 | 解决 |
|------|------|
| 纹理不是由 ANGLE 内部设备创建 | 确认用 `QueryInternalD3DDevice()` 返回的设备创建纹理 |
| 纹理格式不支持 | 改用 `DXGI_FORMAT_B8G8R8A8_UNORM` |
| 扩展不支持 | 检查 `EGL_ANGLE_d3d_texture_client_buffer` 是否在扩展列表中 |
| `BindFlags` 缺少 `BIND_RENDER_TARGET` | 必须包含，否则 ANGLE 无法创建 RTV |

### `OpenSharedResource` 失败 (0x80070005 Access Denied)

- 跨进程场景需要 NT Handle（`CreateSharedHandle` + `OpenSharedResource1`）
- 同进程用 Legacy Handle（`GetSharedHandle` + `OpenSharedResource`）即可

### `KeyedMutex::AcquireSync` 一直超时

- 确认 Producer `ReleaseSync(1)` 已调用
- 确认 `glFinish()` 在 `ReleaseSync` 之前
- 检查是否有异常导致 Mutex 未释放（建议 RAII 封装）

### 像素全黑 / 全零

- 检查 `eglMakeCurrent` 是否成功（每帧渲染前调用）
- 确认 `glViewport` 覆盖整个纹理尺寸
- 检查 GL 渲染命令是否有错误（`glGetError()`）

### 画面撕裂 / 数据不完整

- Consumer 侧少了 `AcquireSync` 就读取了数据
- Producer 侧少了 `glFinish()` 导致 GPU 命令未提交

---

## 9. 扩展与进阶

### 使用 EGLImage 替代 PBuffer（作为采样纹理）

当需要将 D3D 纹理作为 **GL 采样纹理**（而非渲染目标）时：

```cpp
EGLImageKHR image = eglCreateImageKHR(
    display, context,
    EGL_D3D11_SHARE_HANDLE_ANGLE,
    reinterpret_cast<EGLClientBuffer>(shareHandle),
    nullptr);

GLuint texId;
glGenTextures(1, &texId);
glBindTexture(GL_TEXTURE_2D, texId);
glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
```

### 多缓冲（Double/Triple Buffering）

避免 Producer 等 Consumer，提高吞吐：

```cpp
// 创建 2 个共享纹理，轮流使用
SharedTexture buffers[2];
int writeIdx = 0, readIdx = 1;

// Producer：写 buffers[writeIdx]
// Consumer：读 buffers[readIdx]
// 每帧交换：std::swap(writeIdx, readIdx)
```

### 传递给外部进程

跨进程时改用 NT Handle：

```cpp
// 创建时
desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

IDXGIResource1* res1 = nullptr;
tex->QueryInterface(IID_PPV_ARGS(&res1));

HANDLE ntHandle = nullptr;
res1->CreateSharedHandle(nullptr,
    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
    nullptr, &ntHandle);

// 跨进程传递：DuplicateHandle 后通过 IPC 发送
// 消费方：device1->OpenSharedResource1(receivedHandle, IID_PPV_ARGS(&tex))
```

---

## 10. API 速查表

| API | 所属 | 作用 |
|-----|------|------|
| `eglGetPlatformDisplayEXT` | EGL | 创建 D3D11 后端的 EGL Display |
| `eglQueryDisplayAttribEXT` | EGL_EXT | 查询 Display 关联的 EGLDevice |
| `eglQueryDeviceAttribEXT` | EGL_EXT | 从 EGLDevice 查询 D3D11 设备指针 |
| `eglCreatePbufferFromClientBuffer` | EGL | 将 D3D11 纹理包装为 EGL PBuffer |
| `eglMakeCurrent` | EGL | 绑定 Context + Surface |
| `glFinish` | OpenGL ES | **等待所有 GPU 命令完成（释放 mutex 前必须调用）** |
| `ID3D11Device::CreateTexture2D` | D3D11 | 创建共享纹理（需 `SHARED_KEYEDMUTEX` 标志） |
| `IDXGIResource::GetSharedHandle` | DXGI | 获取 Legacy Share Handle |
| `ID3D11Device::OpenSharedResource` | D3D11 | 同进程跨设备打开共享纹理 |
| `IDXGIKeyedMutex::AcquireSync` | DXGI | 获取 GPU 级互斥锁 |
| `IDXGIKeyedMutex::ReleaseSync` | DXGI | 释放 GPU 级互斥锁并交接 Key |
| `ID3D11DeviceContext::CopyResource` | D3D11 | GPU 内纹理拷贝（共享→Staging） |
| `ID3D11DeviceContext::Map` | D3D11 | Staging 纹理映射到 CPU 可读内存 |

---

*文档版本：1.0 | 适用 ANGLE D3D11 后端 | Windows 平台*
