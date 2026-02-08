
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <memory>
#include <array>
#include <optional>
#include <span>
#include <format>

#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"


#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace Config
{
    constexpr UINT ThreadGroupSize = 16;
    constexpr float DefaultSharpness = 0.0f;
    constexpr D3D_FEATURE_LEVEL MinFeatureLevel = D3D_FEATURE_LEVEL_11_0;
}

struct FSRConstants
{
    XMUINT4 Const0;
    XMUINT4 Const1;
    XMUINT4 Const2;
    XMUINT4 Const3;
    XMUINT4 Sample;
};

struct TextureDimensions
{
    UINT width;
    UINT height;

    constexpr bool operator==(const TextureDimensions&) const = default;
    constexpr bool operator!=(const TextureDimensions&) const = default;
};

using VideoProcessorBlt_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11VideoContext*,
    ID3D11VideoProcessor*,
    ID3D11VideoProcessorOutputView*,
    UINT,
    UINT,
    const D3D11_VIDEO_PROCESSOR_STREAM*);

VideoProcessorBlt_t oVideoProcessorBlt = nullptr;

class FSRPipeline
{
public:
    FSRPipeline() = default;
    ~FSRPipeline() = default;

    FSRPipeline(const FSRPipeline&) = delete;
    FSRPipeline& operator=(const FSRPipeline&) = delete;
    FSRPipeline(FSRPipeline&&) = default;
    FSRPipeline& operator=(FSRPipeline&&) = default;

    [[nodiscard]] bool Initialize(
        ID3D11Device* device,
        TextureDimensions inputDims,
        TextureDimensions outputDims)
    {
        m_inputDimensions = inputDims;
        m_outputDimensions = outputDims;

        if (!CreateTextures(device)) return false;
        if (!CreateViews(device)) return false;
        if (!CreateSampler(device)) return false;
        if (!CompileShaders(device)) return false;
        if (!CreateConstantBuffers(device)) return false;

        return true;
    }

    void Release()
    {
        m_inputTexture.Reset();
        m_easuOutputTexture.Reset();
        m_rcasOutputTexture.Reset();
        m_inputSRV.Reset();
        m_easuOutputSRV.Reset();
        m_easuUAV.Reset();
        m_rcasUAV.Reset();
        m_sampler.Reset();
        m_easuShader.Reset();
        m_rcasShader.Reset();
        m_easuConstantBuffer.Reset();
        m_rcasConstantBuffer.Reset();
    }

    [[nodiscard]] bool IsValid() const
    {
        return m_easuShader && m_rcasShader && m_inputSRV &&
            m_easuOutputSRV && m_easuUAV && m_rcasUAV &&
            m_easuConstantBuffer && m_rcasConstantBuffer && m_sampler;
    }

    void ExecuteEASU(ID3D11DeviceContext* context)
    {
        context->CSSetShader(m_easuShader.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[] = { m_inputSRV.Get() };
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);

        ID3D11UnorderedAccessView* uavs[] = { m_easuUAV.Get() };
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(uavs)), uavs, nullptr);

        ID3D11SamplerState* samplers[] = { m_sampler.Get() };
        context->CSSetSamplers(0, static_cast<UINT>(std::size(samplers)), samplers);

        FSRConstants constants = CalculateEASUConstants();
        context->UpdateSubresource(m_easuConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);

        ID3D11Buffer* constantBuffers[] = { m_easuConstantBuffer.Get() };
        context->CSSetConstantBuffers(0, static_cast<UINT>(std::size(constantBuffers)), constantBuffers);

        const auto [dispatchX, dispatchY] = CalculateDispatchDimensions(m_outputDimensions);
        context->Dispatch(dispatchX, dispatchY, 1);

        UnbindComputeResources(context);
    }

    void ExecuteRCAS(ID3D11DeviceContext* context, float sharpness = Config::DefaultSharpness)
    {
        context->CSSetShader(m_rcasShader.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[] = { m_easuOutputSRV.Get() };
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);

        ID3D11UnorderedAccessView* uavs[] = { m_rcasUAV.Get() };
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(uavs)), uavs, nullptr);

        ID3D11SamplerState* samplers[] = { m_sampler.Get() };
        context->CSSetSamplers(0, static_cast<UINT>(std::size(samplers)), samplers);

        FSRConstants constants = CalculateRCASConstants(sharpness);
        context->UpdateSubresource(m_rcasConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);

        ID3D11Buffer* constantBuffers[] = { m_rcasConstantBuffer.Get() };
        context->CSSetConstantBuffers(0, static_cast<UINT>(std::size(constantBuffers)), constantBuffers);

        const auto [dispatchX, dispatchY] = CalculateDispatchDimensions(m_outputDimensions);
        context->Dispatch(dispatchX, dispatchY, 1);

        UnbindComputeResources(context);
    }

    [[nodiscard]] ID3D11Texture2D* GetInputTexture() const { return m_inputTexture.Get(); }
    [[nodiscard]] ID3D11Texture2D* GetOutputTexture() const { return m_rcasOutputTexture.Get(); }
    [[nodiscard]] TextureDimensions GetInputDimensions() const { return m_inputDimensions; }
    [[nodiscard]] TextureDimensions GetOutputDimensions() const { return m_outputDimensions; }

private:
    [[nodiscard]] bool CreateTextures(ID3D11Device* device)
    {
        // Input Texture (BGRA)
        D3D11_TEXTURE2D_DESC inputDesc = {
            .Width = m_inputDimensions.width,
            .Height = m_inputDimensions.height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = {.Count = 1, .Quality = 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        if (FAILED(device->CreateTexture2D(&inputDesc, nullptr, m_inputTexture.GetAddressOf())))
            return false;

        // EASU Output Texture
        D3D11_TEXTURE2D_DESC easuOutputDesc = {
            .Width = m_outputDimensions.width,
            .Height = m_outputDimensions.height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = {.Count = 1, .Quality = 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        if (FAILED(device->CreateTexture2D(&easuOutputDesc, nullptr, m_easuOutputTexture.GetAddressOf())))
            return false;

        // RCAS Output Texture
        D3D11_TEXTURE2D_DESC rcasOutputDesc = {
            .Width = m_outputDimensions.width,
            .Height = m_outputDimensions.height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = {.Count = 1, .Quality = 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        if (FAILED(device->CreateTexture2D(&rcasOutputDesc, nullptr, m_rcasOutputTexture.GetAddressOf())))
            return false;

        return true;
    }

    [[nodiscard]] bool CreateViews(ID3D11Device* device)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1 }
        };

        if (FAILED(device->CreateShaderResourceView(m_inputTexture.Get(), &srvDesc, m_inputSRV.GetAddressOf())))
            return false;

        if (FAILED(device->CreateShaderResourceView(m_easuOutputTexture.Get(), &srvDesc, m_easuOutputSRV.GetAddressOf())))
            return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = {.MipSlice = 0 }
        };

        if (FAILED(device->CreateUnorderedAccessView(m_easuOutputTexture.Get(), &uavDesc, m_easuUAV.GetAddressOf())))
            return false;

        if (FAILED(device->CreateUnorderedAccessView(m_rcasOutputTexture.Get(), &uavDesc, m_rcasUAV.GetAddressOf())))
            return false;

        return true;
    }

    [[nodiscard]] bool CreateSampler(ID3D11Device* device)
    {
        D3D11_SAMPLER_DESC samplerDesc = {
            .Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
            .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
            .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
            .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
            .MipLODBias = 0.0f,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D11_COMPARISON_NEVER,
            .BorderColor = { 0.0f, 0.0f, 0.0f, 0.0f },
            .MinLOD = 0.0f,
            .MaxLOD = D3D11_FLOAT32_MAX
        };

        return SUCCEEDED(device->CreateSamplerState(&samplerDesc, m_sampler.GetAddressOf()));
    }

    [[nodiscard]] bool CompileShaders(ID3D11Device* device)
    {
        // EASU Shader
        ComPtr<ID3DBlob> easuBlob;
        ComPtr<ID3DBlob> errorBlob;

        HRESULT hr = D3DCompileFromFile(
            L"fsr1.hlsl",
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "CSMain",
            "cs_5_0",
            0, 0,
            easuBlob.GetAddressOf(),
            errorBlob.GetAddressOf()
        );

        if (FAILED(hr))
            return false;

        if (FAILED(device->CreateComputeShader(
            easuBlob->GetBufferPointer(),
            easuBlob->GetBufferSize(),
            nullptr,
            m_easuShader.GetAddressOf())))
        {
            return false;
        }

        // RCAS Shader
        ComPtr<ID3DBlob> rcasBlob;

        hr = D3DCompileFromFile(
            L"rcas.hlsl",
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "CSMain",
            "cs_5_0",
            0, 0,
            rcasBlob.GetAddressOf(),
            errorBlob.GetAddressOf()
        );

        if (FAILED(hr))
            return false;

        if (FAILED(device->CreateComputeShader(
            rcasBlob->GetBufferPointer(),
            rcasBlob->GetBufferSize(),
            nullptr,
            m_rcasShader.GetAddressOf())))
        {
            return false;
        }

        return true;
    }

    [[nodiscard]] bool CreateConstantBuffers(ID3D11Device* device)
    {
        D3D11_BUFFER_DESC cbDesc = {
            .ByteWidth = sizeof(FSRConstants),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
            .CPUAccessFlags = 0,
            .MiscFlags = 0,
            .StructureByteStride = 0
        };

        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, m_easuConstantBuffer.GetAddressOf())))
            return false;

        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, m_rcasConstantBuffer.GetAddressOf())))
            return false;

        return true;
    }

    [[nodiscard]] FSRConstants CalculateEASUConstants() const
    {
        FSRConstants constants{};

        FsrEasuCon(
            reinterpret_cast<AU1*>(&constants.Const0),
            reinterpret_cast<AU1*>(&constants.Const1),
            reinterpret_cast<AU1*>(&constants.Const2),
            reinterpret_cast<AU1*>(&constants.Const3),
            static_cast<AF1>(m_inputDimensions.width),
            static_cast<AF1>(m_inputDimensions.height),
            static_cast<AF1>(m_inputDimensions.width),
            static_cast<AF1>(m_inputDimensions.height),
            static_cast<AF1>(m_outputDimensions.width),
            static_cast<AF1>(m_outputDimensions.height)
        );

        constants.Sample.x = 0;
        return constants;
    }

    [[nodiscard]] FSRConstants CalculateRCASConstants(float sharpness) const
    {
        FSRConstants constants{};
        FsrRcasCon(reinterpret_cast<AU1*>(&constants.Const0), sharpness);
        return constants;
    }

    [[nodiscard]] static constexpr std::pair<UINT, UINT> CalculateDispatchDimensions(TextureDimensions dims)
    {
        return {
            (dims.width + Config::ThreadGroupSize - 1) / Config::ThreadGroupSize,
            (dims.height + Config::ThreadGroupSize - 1) / Config::ThreadGroupSize
        };
    }

    static void UnbindComputeResources(ID3D11DeviceContext* context)
    {
        ID3D11ShaderResourceView* nullSRVs[] = { nullptr };
        ID3D11UnorderedAccessView* nullUAVs[] = { nullptr };

        context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(nullUAVs)), nullUAVs, nullptr);
    }

private:
    TextureDimensions m_inputDimensions{};
    TextureDimensions m_outputDimensions{};

    ComPtr<ID3D11Texture2D> m_inputTexture;
    ComPtr<ID3D11Texture2D> m_easuOutputTexture;
    ComPtr<ID3D11Texture2D> m_rcasOutputTexture;

    ComPtr<ID3D11ShaderResourceView> m_inputSRV;
    ComPtr<ID3D11ShaderResourceView> m_easuOutputSRV;

    ComPtr<ID3D11UnorderedAccessView> m_easuUAV;
    ComPtr<ID3D11UnorderedAccessView> m_rcasUAV;

    ComPtr<ID3D11SamplerState> m_sampler;

    ComPtr<ID3D11ComputeShader> m_easuShader;
    ComPtr<ID3D11ComputeShader> m_rcasShader;

    ComPtr<ID3D11Buffer> m_easuConstantBuffer;
    ComPtr<ID3D11Buffer> m_rcasConstantBuffer;
};

class D3D11Context
{
public:
    [[nodiscard]] bool Initialize(ID3D11VideoContext* videoContext)
    {
        videoContext->GetDevice(m_device.GetAddressOf());
        if (!m_device)
            return false;

        m_device->GetImmediateContext(m_context.GetAddressOf());
        if (!m_context)
            return false;

        if (FAILED(m_device.As(&m_videoDevice)))
            return false;

        return true;
    }

    [[nodiscard]] ID3D11Device* GetDevice() const { return m_device.Get(); }
    [[nodiscard]] ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    [[nodiscard]] ID3D11VideoDevice* GetVideoDevice() const { return m_videoDevice.Get(); }

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11VideoDevice> m_videoDevice;
};

class VideoProcessor
{
public:
    [[nodiscard]] bool ConvertToFormat(
        D3D11Context& d3dContext,
        ID3D11VideoContext* videoContext,
        ID3D11Texture2D* sourceTexture,
        ID3D11Texture2D* destTexture,
        const D3D11_TEXTURE2D_DESC& sourceDesc,
        const D3D11_TEXTURE2D_DESC& destDesc,
        UINT outputFrame,
        std::optional<const D3D11_VIDEO_PROCESSOR_STREAM*> inputStream = std::nullopt)
    {
        ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        ComPtr<ID3D11VideoProcessor> processor;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {
            .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
            .InputFrameRate = {.Numerator = 30, .Denominator = 1 },
            .InputWidth = sourceDesc.Width,
            .InputHeight = sourceDesc.Height,
            .OutputFrameRate = {.Numerator = 30, .Denominator = 1 },
            .OutputWidth = destDesc.Width,
            .OutputHeight = destDesc.Height,
            .Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL
        };

        if (FAILED(d3dContext.GetVideoDevice()->CreateVideoProcessorEnumerator(
            &contentDesc, enumerator.GetAddressOf())))
        {
            return false;
        }

        if (FAILED(d3dContext.GetVideoDevice()->CreateVideoProcessor(
            enumerator.Get(), 0, processor.GetAddressOf())))
        {
            return false;
        }

        ComPtr<ID3D11VideoProcessorOutputView> outputView;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {
            .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
            .Texture2D = {.MipSlice = 0 }
        };

        if (FAILED(d3dContext.GetVideoDevice()->CreateVideoProcessorOutputView(
            destTexture, enumerator.Get(), &outputViewDesc, outputView.GetAddressOf())))
        {
            return false;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};

        if (inputStream.has_value())
        {
            return SUCCEEDED(oVideoProcessorBlt(
                videoContext,
                processor.Get(),
                outputView.Get(),
                outputFrame,
                1,
                inputStream.value()
            ));
        }
        else
        {
            ComPtr<ID3D11VideoProcessorInputView> inputView;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {
                .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
                .Texture2D = {.MipSlice = 0, .ArraySlice = 0 }
            };

            if (FAILED(d3dContext.GetVideoDevice()->CreateVideoProcessorInputView(
                sourceTexture, enumerator.Get(), &inputViewDesc, inputView.GetAddressOf())))
            {
                return false;
            }

            stream.Enable = TRUE;
            stream.OutputIndex = 0;
            stream.InputFrameOrField = 0;
            stream.PastFrames = 0;
            stream.FutureFrames = 0;
            stream.pInputSurface = inputView.Get();

            return SUCCEEDED(oVideoProcessorBlt(
                videoContext,
                processor.Get(),
                outputView.Get(),
                outputFrame,
                1,
                &stream
            ));
        }
    }
};

class FSRManager
{
public:
    static FSRManager& Get()
    {
        static FSRManager instance;
        return instance;
    }

    void ToggleEnabled()
    {
        m_enabled = !m_enabled;
    }

    [[nodiscard]] bool IsEnabled() const { return m_enabled; }

    [[nodiscard]] bool ReinitializeIfNeeded(
        ID3D11Device* device,
        TextureDimensions inputDims,
        TextureDimensions outputDims)
    {
        if (m_pipeline.IsValid() &&
            m_pipeline.GetInputDimensions() == inputDims &&
            m_pipeline.GetOutputDimensions() == outputDims)
        {
            return true;
        }

        m_pipeline.Release();
        return m_pipeline.Initialize(device, inputDims, outputDims);
    }

    FSRPipeline& GetPipeline() { return m_pipeline; }

private:
    FSRManager() = default;

    bool m_enabled = false;
    FSRPipeline m_pipeline;
};

HRESULT STDMETHODCALLTYPE hkVideoProcessorBlt(
    ID3D11VideoContext* videoContext,
    ID3D11VideoProcessor* videoProcessor,
    ID3D11VideoProcessorOutputView* outputView,
    UINT outputFrame,
    UINT streamCount,
    const D3D11_VIDEO_PROCESSOR_STREAM* streams)
{
    if (!streams || streamCount == 0)
    {
        return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
            outputFrame, streamCount, streams);
    }

    if (GetAsyncKeyState(VK_F10) & 1)
    {
        FSRManager::Get().ToggleEnabled();
    }

    if (FSRManager::Get().IsEnabled())
    {
        return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
            outputFrame, streamCount, streams);
    }

    D3D11Context d3dContext;
    if (!d3dContext.Initialize(videoContext))
    {
        return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
            outputFrame, streamCount, streams);
    }

    ComPtr<ID3D11Resource> inputResource;
    ComPtr<ID3D11Resource> outputResource;

    if (streams[0].pInputSurface)
        streams[0].pInputSurface->GetResource(inputResource.GetAddressOf());

    outputView->GetResource(outputResource.GetAddressOf());

    ComPtr<ID3D11Texture2D> inputTexture;
    ComPtr<ID3D11Texture2D> outputTexture;

    if (FAILED(inputResource.As(&inputTexture)) || FAILED(outputResource.As(&outputTexture)))
    {
        return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
            outputFrame, streamCount, streams);
    }

    D3D11_TEXTURE2D_DESC inputDesc{};
    D3D11_TEXTURE2D_DESC outputDesc{};
    inputTexture->GetDesc(&inputDesc);
    outputTexture->GetDesc(&outputDesc);

    TextureDimensions inputDims = { inputDesc.Width, inputDesc.Height };
    TextureDimensions outputDims = { outputDesc.Width, outputDesc.Height };

    if (!FSRManager::Get().ReinitializeIfNeeded(d3dContext.GetDevice(), inputDims, outputDims))
    {
        return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
            outputFrame, streamCount, streams);
    }

    auto& pipeline = FSRManager::Get().GetPipeline();

    ComPtr<ID3D11Texture2D> tempInputTexture;
    D3D11_TEXTURE2D_DESC tempInputDesc = {
        .Width = inputDesc.Width,
        .Height = inputDesc.Height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = {.Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = 0,
        .MiscFlags = 0
    };

    if (FAILED(d3dContext.GetDevice()->CreateTexture2D(&tempInputDesc, nullptr, tempInputTexture.GetAddressOf())))
    {
        return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
            outputFrame, streamCount, streams);
    }

    VideoProcessor videoProc;
    if (!videoProc.ConvertToFormat(
        d3dContext,
        videoContext,
        inputTexture.Get(),
        tempInputTexture.Get(),
        inputDesc,
        tempInputDesc,
        outputFrame,
        &streams[0]))
    {
        return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
            outputFrame, streamCount, streams);
    }

    d3dContext.GetContext()->CopyResource(pipeline.GetInputTexture(), tempInputTexture.Get());
    pipeline.ExecuteEASU(d3dContext.GetContext());
    pipeline.ExecuteRCAS(d3dContext.GetContext(), Config::DefaultSharpness);
    if (outputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        d3dContext.GetContext()->CopyResource(outputTexture.Get(), pipeline.GetOutputTexture());
    }
    else
    {
        if (!videoProc.ConvertToFormat(
            d3dContext,
            videoContext,
            pipeline.GetOutputTexture(),
            outputTexture.Get(),
            outputDesc,
            outputDesc,
            outputFrame))
        {
            return oVideoProcessorBlt(videoContext, videoProcessor, outputView,
                outputFrame, streamCount, streams);
        }
    }

    return S_OK;
}

struct DetourHook
{
    void* target;
    void* hook;
    void* trampoline;
    std::array<BYTE, 100> original;
};

DetourHook g_VPBltHook{};

#include "hde64.h"

SIZE_T GetHookSize(void* target, SIZE_T minSize = 14)
{
    hde64s hs;
    SIZE_T totalSize = 0;
    auto* code = static_cast<BYTE*>(target);

    while (totalSize < minSize)
    {
        unsigned int len = hde64_disasm(code + totalSize, &hs);
        if (hs.flags & F_ERROR)
            return minSize;

        totalSize += len;
    }

    return totalSize;
}

bool CreateDetour(DetourHook& hook, SIZE_T hookSize = 14)
{
    SIZE_T actualSize = hookSize;

    std::memcpy(hook.original.data(), hook.target, actualSize);

    hook.trampoline = VirtualAlloc(nullptr, 1024, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!hook.trampoline)
        return false;

    auto* tramp = static_cast<BYTE*>(hook.trampoline);
    std::memcpy(tramp, hook.original.data(), actualSize);

    auto* returnAddr = static_cast<BYTE*>(hook.target) + actualSize;

    tramp += actualSize;
    tramp[0] = 0xFF;  // jmp [rip+0]
    tramp[1] = 0x25;
    *reinterpret_cast<DWORD*>(tramp + 2) = 0;
    *reinterpret_cast<void**>(tramp + 6) = returnAddr;

    DWORD oldProtect;
    VirtualProtect(hook.target, actualSize, PAGE_EXECUTE_READWRITE, &oldProtect);

    auto* target = static_cast<BYTE*>(hook.target);
    target[0] = 0xFF;  // jmp [rip+0]
    target[1] = 0x25;
    *reinterpret_cast<DWORD*>(target + 2) = 0;
    *reinterpret_cast<void**>(target + 6) = hook.hook;

    for (SIZE_T i = 14; i < actualSize; i++)
        target[i] = 0x90;

    VirtualProtect(hook.target, actualSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), hook.target, actualSize);

    return true;
}

DWORD WINAPI InstallHook(LPVOID)
{
    if (!GetModuleHandleA("d3d11"))
        return 0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    D3D_FEATURE_LEVEL featureLevel = Config::MinFeatureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        &featureLevel,
        1,
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        nullptr,
        context.GetAddressOf()
    );

    if (FAILED(hr))
        return 0;

    ComPtr<ID3D11VideoDevice> videoDevice;
    if (FAILED(device.As(&videoDevice)))
        return 0;

    ComPtr<ID3D11VideoContext> videoContext;
    if (FAILED(context.As(&videoContext)))
        return 0;

    void** vTable = *reinterpret_cast<void***>(videoContext.Get());

    g_VPBltHook.target = vTable[53];
    g_VPBltHook.hook = reinterpret_cast<void*>(hkVideoProcessorBlt);

    if (CreateDetour(g_VPBltHook, GetHookSize(g_VPBltHook.target)))
    {
        oVideoProcessorBlt = reinterpret_cast<VideoProcessorBlt_t>(g_VPBltHook.trampoline);
    }

    return 0;
}

void CleanupFSRResources()
{
    FSRManager::Get().GetPipeline().Release();

    if (g_VPBltHook.trampoline)
    {
        VirtualFree(g_VPBltHook.trampoline, 0, MEM_RELEASE);
        g_VPBltHook.trampoline = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InstallHook, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        CleanupFSRResources();
    }
    return TRUE;
}
