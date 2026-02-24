# ANGLE Shared Texture Demo

基于 ANGLE（OpenGL ES on D3D11）的共享纹理跨线程渲染示例项目。

## 项目简介

本项目展示如何使用 ANGLE 在一个线程中渲染图像，然后通过 D3D11 共享纹理（Shared Texture）在另一个线程中读取并保存为 PNG 文件。支持单线程和双线程流水线两种架构。

### 核心特性

- ✅ **ANGLE OpenGL ES 渲染**：使用 OpenGL ES 2.0 API 在 Windows 上渲染
- ✅ **D3D11 共享纹理**：跨设备共享纹理数据（零拷贝）
- ✅ **KeyedMutex 同步**：GPU 级别的跨线程同步机制
- ✅ **圆角图像处理**：Shader 实现可配置圆角半径的图像裁剪
- ✅ **双线程流水线**：Producer-Consumer 模式，支持高效批量处理

## 技术架构

### 架构 1：单线程版本 (main.cpp)

```
┌─────────────────────────────────────────────┐
│              Main Thread                     │
│                                              │
│  ┌──────────────┐      ┌──────────────┐    │
│  │ ANGLE Device │─────▶│ D3D11 Device │    │
│  │  (Producer)  │shared│  (Consumer)  │    │
│  └──────────────┘texture└──────────────┘    │
│         │                       │            │
│      Render                  SavePNG         │
└─────────────────────────────────────────────┘
```

### 架构 2：双线程流水线版本 (main_pipeline.cpp)

```
┌─────────────────────┐    ┌─────────────────────┐
│  Producer Thread    │    │  Consumer Thread    │
│                     │    │                     │
│  ┌──────────────┐  │    │  ┌──────────────┐  │
│  │ANGLE Renderer│  │    │  │ D3D11 Reader │  │
│  └──────┬───────┘  │    │  └──────┬───────┘  │
│         │          │    │         │          │
│    ReleaseSync(1)  │    │    AcquireSync(1)  │
│         │          │    │         │          │
│         ▼          │    │         ▼          │
│  ┌──────────────┐  │    │  ┌──────────────┐  │
│  │Shared Texture│◀─┼────┼─▶│Shared Texture│  │
│  │(KeyedMutex)  │  │    │  │(KeyedMutex)  │  │
│  └──────────────┘  │    │  └──────────────┘  │
│         ▲          │    │         │          │
│    AcquireSync(0)  │    │    ReleaseSync(0)  │
│         │          │    │         │          │
└─────────┼──────────┘    └─────────┼──────────┘
          │                         │
          └─────────────┬───────────┘
                   KeyedMutex
                  Synchronization
```

## 项目结构

```
AngleSharedTextureDemo/
├── main.cpp                    # 单线程示例
├── main_pipeline.cpp           # 双线程流水线示例
├── ANGLEProducer.cpp/h        # ANGLE 渲染器（生产者）
├── D3D11Consumer.cpp/h        # D3D11 消费者（保存 PNG）
├── SharedTextureDemo.h         # 公共头文件
├── CMakeLists.txt              # CMake 构建配置
├── TECHNICAL_GUIDE.md          # 技术文档（架构、原理、时序图）
└── third_party/
    ├── angle/                  # ANGLE 库（预编译）
    └── stb/                    # STB 图像库（header-only）
```

## 快速开始

### 环境要求

- **操作系统**：Windows 10/11
- **编译器**：Visual Studio 2019/2022（支持 C++17）
- **CMake**：≥ 3.16
- **GPU**：支持 D3D11 的显卡

### 编译步骤

1. **克隆项目**
   ```bash
   git clone <repository-url>
   cd AngleSharedTextureDemo
   ```

2. **生成构建文件**
   ```bash
   cmake -B build -G "Visual Studio 17 2022" -A x64
   ```

3. **编译**
   ```bash
   # 编译单线程版本
   cmake --build build --config Release --target ANGLESharedTextureDemo
   
   # 编译双线程版本
   cmake --build build --config Release --target PipelineDemo
   ```

### 运行示例

**单线程版本**：
```bash
cd build/Release
ANGLESharedTextureDemo.exe
```
输出：`output_rounded.png`（800x600，圆角半径50）

**双线程流水线版本**：
```bash
cd build/Release
PipelineDemo.exe
```
输出：`pipeline_output_01.png` ~ `pipeline_output_05.png`（不同圆角半径）

## 核心技术

### 1. ANGLE (Almost Native Graphics Layer Engine)

ANGLE 是 Google 开发的图形引擎层，将 OpenGL ES API 翻译为平台原生 API（Windows 上为 D3D11）。

**优势**：
- 跨平台 API（OpenGL ES）+ 原生性能（D3D11）
- 避免 Windows 上 OpenGL 驱动质量不稳定问题
- 支持 WebGL 在桌面浏览器的实现

### 2. D3D11 共享纹理 (Shared Texture)

使用 `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` 创建的纹理可以在不同 D3D11 设备间共享。

**核心机制**：
- **Legacy Share Handle**：通过 `IDXGIResource::GetSharedHandle()` 获取句柄
- **跨设备打开**：通过 `ID3D11Device::OpenSharedResource()` 在另一个设备打开
- **零拷贝**：纹理数据保存在 GPU 显存，多个设备直接访问

### 3. KeyedMutex 同步

`IDXGIKeyedMutex` 提供 GPU 级别的互斥锁，用于跨设备/线程同步共享资源。

**API**：
- `AcquireSync(key, timeout)`: 获取指定 key 的锁
- `ReleaseSync(key)`: 释放到指定 key

**同步模式**（本项目采用）：
```
Producer                Consumer
---------              ---------
AcquireSync(0)         (wait)
  Render
ReleaseSync(1)    ->   AcquireSync(1)
  (wait)                 Read
AcquireSync(0)    <-   ReleaseSync(0)
```

### 4. 圆角 Shader 实现

Fragment Shader 通过 `distance()` 和 `smoothstep()` 实现圆角裁剪：

```glsl
void main() {
    vec2 pixelCoord = vUV * uResolution;
    float radius = uCornerRadius;
    float alpha = 1.0;
    
    // 检测四个角
    if (pixelCoord.x < radius && pixelCoord.y < radius) {
        float dist = distance(pixelCoord, vec2(radius, radius));
        alpha = 1.0 - smoothstep(radius - 1.0, radius, dist);
    }
    // ... 其他三个角
    
    vec4 texColor = texture2D(uTexture, vUV);
    gl_FragColor = vec4(texColor.rgb, texColor.a * alpha);
}
```

## 性能测试

**测试环境**：
- CPU: Intel Core i7-12700
- GPU: NVIDIA RTX 3060
- 图像: 1332x850 → 800x600

**单线程版本**：
- 单帧耗时：~100ms
- 瓶颈：串行执行（渲染 + IO）

**双线程流水线版本**：
- 5帧总耗时：~450ms
- 平均单帧：~90ms
- 提升：流水线并行，渲染与 IO 重叠

## 已知问题与解决

### ✅ 问题1：多次渲染时纹理数据丢失

**现象**：第1张图片正常，第2-5张全透明（18KB vs 652KB）

**原因**：`LoadImageFromFile()` 未解绑 EGL 上下文，导致 ANGLE 内部状态管理机制清空纹理数据

**解决**：在 `LoadImageFromFile()` 末尾添加：
```cpp
eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
```

详见 commit `e2c3495`

### ✅ 问题2：KeyedMutex 超时

**现象**：高负载时 `AcquireSync` 超时返回错误

**解决**：将超时参数从 `5000ms` 改为 `INFINITE`

详见 commit `57bf73a`

## 技术文档

详细的技术架构、实现原理和时序图请参考：

📖 [TECHNICAL_GUIDE.md](./TECHNICAL_GUIDE.md)

## 许可协议

MIT License

## 参考资料

- [ANGLE 官方文档](https://chromium.googlesource.com/angle/angle/)
- [D3D11 Shared Resources](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-sharing)
- [IDXGIKeyedMutex](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nn-dxgi-idxgikeyedmutex)
