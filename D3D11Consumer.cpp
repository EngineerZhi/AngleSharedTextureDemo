/**
 * D3D11Consumer.cpp
 *
 * D3D11 原生设备侧实现：
 *   1. 创建独立的 D3D11 设备（与 ANGLE 内部设备不同）
 *   2. 通过 Legacy Share Handle 打开共享纹理（同进程跨设备）
 *   3. 通过 IDXGIKeyedMutex 等待 ANGLE 写完后读取
 *   4. 支持 GPU 端（SRV）和 CPU 端（Staging回读）两种消费方式
 */

#include "SharedTextureDemo.h"
#include <cstdio>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

// -----------------------------------------------------------------------
// D3D11Consumer 实现
// -----------------------------------------------------------------------
void D3D11Consumer::Init() {
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL actualLevel     = {};

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // 使用默认适配器
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels, 1,
        D3D11_SDK_VERSION,
        &m_device,
        &actualLevel,
        &m_context);
    HR_CHECK(hr, "D3D11CreateDevice (consumer)");

    printf("[D3D11Consumer] Device created, feature level=0x%04X\n",
           static_cast<unsigned>(actualLevel));
}

void D3D11Consumer::OpenSharedTexture(HANDLE shareHandle,
                                       const SharedTextureDesc& desc) {
    if (!m_device) {
        throw std::runtime_error("D3D11Consumer not initialized");
    }
    m_desc = desc;

    // 同进程：直接 OpenSharedResource（Legacy Handle）
    HRESULT hr = m_device->OpenSharedResource(
        shareHandle,
        IID_PPV_ARGS(&m_importedTex));
    HR_CHECK(hr, "OpenSharedResource");

    printf("[D3D11Consumer] Shared texture opened (%ux%u)\n",
           desc.width, desc.height);

    // 获取 KeyedMutex 接口
    hr = m_importedTex->QueryInterface(IID_PPV_ARGS(&m_keyedMutex));
    HR_CHECK(hr, "QueryInterface IDXGIKeyedMutex (consumer)");

    // 创建 ShaderResourceView（GPU 端使用）
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format              = desc.format;
    srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(m_importedTex, &srvDesc, &m_srv);
    HR_CHECK(hr, "CreateShaderResourceView");

    // 创建 Staging 纹理（CPU 回读用）
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width              = desc.width;
    stagingDesc.Height             = desc.height;
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = desc.format;
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags          = 0;
    stagingDesc.MiscFlags          = 0;  // Staging 纹理不能有 SHARED 标志

    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTex);
    HR_CHECK(hr, "CreateTexture2D (staging)");

    printf("[D3D11Consumer] SRV and Staging texture created\n");
}

std::vector<uint8_t> D3D11Consumer::ConsumeFrame() {
    // 等待 ANGLE 写完（key=1 表示 Producer 已 Release）
    HRESULT hr = m_keyedMutex->AcquireSync(1, INFINITE);
    HR_CHECK(hr, "KeyedMutex AcquireSync(1) [consumer]");

    // 从 GPU 拷贝到 Staging
    m_context->CopyResource(m_stagingTex, m_importedTex);

    // Map 读取像素
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_context->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
    HR_CHECK(hr, "Map staging texture");

    const UINT pixelBytes = 4;  // BGRA8
    const UINT rowBytes   = m_desc.width * pixelBytes;
    std::vector<uint8_t> pixels(rowBytes * m_desc.height);

    // 处理 RowPitch 可能有 padding 的情况
    const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = pixels.data();
    for (UINT row = 0; row < m_desc.height; ++row) {
        memcpy(dst + row * rowBytes, src + row * mapped.RowPitch, rowBytes);
    }

    m_context->Unmap(m_stagingTex, 0);

    // 归还 KeyedMutex（key=0，通知 Producer 可以写下一帧）
    hr = m_keyedMutex->ReleaseSync(0);
    HR_CHECK(hr, "KeyedMutex ReleaseSync(0) [consumer]");

    return pixels;
}

void D3D11Consumer::BindSRV(UINT slot) {
    if (m_srv && m_context) {
        m_context->PSSetShaderResources(slot, 1, &m_srv);
    }
}

void D3D11Consumer::SaveToPNG(const char* path) {
    auto pixels = ConsumeFrame();
    
    int result = stbi_write_png(path, m_desc.width, m_desc.height, 4, 
                                pixels.data(), m_desc.width * 4);
    if (!result) {
        throw std::runtime_error(std::string("Failed to save PNG: ") + path);
    }
    printf("[D3D11Consumer] Saved PNG: %s (%ux%u, %zu bytes)\n", 
           path, m_desc.width, m_desc.height, pixels.size());
}

void D3D11Consumer::Destroy() {
    if (m_keyedMutex) { m_keyedMutex->Release(); m_keyedMutex  = nullptr; }
    if (m_srv)        { m_srv->Release();         m_srv         = nullptr; }
    if (m_stagingTex) { m_stagingTex->Release();  m_stagingTex  = nullptr; }
    if (m_importedTex){ m_importedTex->Release();  m_importedTex = nullptr; }
    if (m_context)    { m_context->Release();     m_context     = nullptr; }
    if (m_device)     { m_device->Release();      m_device      = nullptr; }
}
