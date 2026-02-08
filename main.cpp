
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <cassert>
#include <string>
#include "hde64.h"

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")


#include "Zydis.h"

using namespace DirectX;

struct FSRConstants
{
    XMUINT4 Const0;
    XMUINT4 Const1;
    XMUINT4 Const2;
    XMUINT4 Const3;
    XMUINT4 Sample;
};

struct DetourHook
{
    void* target;
    void* hook;
    void* trampoline;
    BYTE original[100];
};

bool CreateDetour(DetourHook& h, SIZE_T HOOK_SIZE = 14)
{
    SIZE_T actualSize = HOOK_SIZE;

    memcpy(h.original, h.target, actualSize);

    h.trampoline = VirtualAlloc(nullptr, 1024, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!h.trampoline) return false;

    BYTE* tramp = (BYTE*)h.trampoline;

    memcpy(tramp, h.original, actualSize);

    BYTE* returnAddr = (BYTE*)h.target + actualSize;

    tramp += actualSize;
    tramp[0] = 0xFF;  // jmp [rip+0]
    tramp[1] = 0x25;
    *(DWORD*)(tramp + 2) = 0;
    *(void**)(tramp + 6) = returnAddr;

    DWORD old;
    VirtualProtect(h.target, actualSize, PAGE_EXECUTE_READWRITE, &old);

    BYTE* target = (BYTE*)h.target;
    target[0] = 0xFF;  // jmp [rip+0]
    target[1] = 0x25;
    *(DWORD*)(target + 2) = 0;
    *(void**)(target + 6) = h.hook;

    for (SIZE_T i = 14; i < actualSize; i++)
        target[i] = 0x90;

    VirtualProtect(h.target, actualSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), h.target, actualSize);

    return true;
}

using VideoProcessorBlt_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11VideoContext* This,
    ID3D11VideoProcessor* pVideoProcessor,
    ID3D11VideoProcessorOutputView* pView,
    UINT OutputFrame,
    UINT StreamCount,
    const D3D11_VIDEO_PROCESSOR_STREAM* pStreams);

DetourHook g_VPBltHook{};
VideoProcessorBlt_t oVideoProcessorBlt = nullptr;

#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"

static bool enable = false;
static ID3D11Texture2D* pFSRInputTex = nullptr;
static ID3D11Texture2D* pFSROutputTex = nullptr;
static ID3D11Texture2D* pRCASOutputTex = nullptr;
static ID3D11ShaderResourceView* pSRV = nullptr;
static ID3D11ShaderResourceView* pRCASSRV = nullptr;
static ID3D11UnorderedAccessView* pUAV = nullptr;
static ID3D11UnorderedAccessView* pRCASUAV = nullptr;
static ID3D11ComputeShader* pCS = nullptr;
static ID3D11ComputeShader* pRCASCS = nullptr;
static ID3D11Buffer* pCB = nullptr;
static ID3D11Buffer* pRCASCB = nullptr;
static ID3D11SamplerState* pSampler = nullptr;
static UINT lastInputWidth = 0;
static UINT lastInputHeight = 0;
static UINT lastOutputWidth = 0;
static UINT lastOutputHeight = 0;

HRESULT STDMETHODCALLTYPE hkVideoProcessorBlt(
    ID3D11VideoContext* pVideoContext,
    ID3D11VideoProcessor* pVideoProcessor,
    ID3D11VideoProcessorOutputView* pView,
    UINT OutputFrame,
    UINT StreamCount,
    const D3D11_VIDEO_PROCESSOR_STREAM* pStreams)
{
    // if (!pStreams || StreamCount == 0)

    if (GetAsyncKeyState(VK_F10) & 1)
        enable = !enable;

    if (enable)
        return oVideoProcessorBlt(pVideoContext, pVideoProcessor, pView, OutputFrame, StreamCount, pStreams);

    ID3D11Device* pDevice = nullptr;
    pVideoContext->GetDevice(&pDevice);
    if (!pDevice)
        return oVideoProcessorBlt(pVideoContext, pVideoProcessor, pView, OutputFrame, StreamCount, pStreams);

    ID3D11DeviceContext* pContext = nullptr;
    pDevice->GetImmediateContext(&pContext);
    if (!pContext)
    {
        pDevice->Release();
        return oVideoProcessorBlt(pVideoContext, pVideoProcessor, pView, OutputFrame, StreamCount, pStreams);
    }

    ID3D11VideoDevice* pVideoDevice = nullptr;
    pDevice->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&pVideoDevice);
    if (!pVideoDevice)
    {
        pDevice->Release();
        return oVideoProcessorBlt(pVideoContext, pVideoProcessor, pView, OutputFrame, StreamCount, pStreams);
    }

    ID3D11Resource* pInputRes = nullptr;
    if (pStreams[0].pInputSurface)
        pStreams[0].pInputSurface->GetResource(&pInputRes);

    ID3D11Resource* pOutputRes = nullptr;
    pView->GetResource(&pOutputRes);

    ID3D11Texture2D* pInputTex = (ID3D11Texture2D*)pInputRes;
    ID3D11Texture2D* pOutputTex = (ID3D11Texture2D*)pOutputRes;

    if (pInputTex && pOutputTex)
    {
        D3D11_TEXTURE2D_DESC inDesc, outDesc;
        pInputTex->GetDesc(&inDesc);
        pOutputTex->GetDesc(&outDesc);

        // if (inDesc.Width < outDesc.Width || inDesc.Height < outDesc.Height)
        {
            bool needsReinit = (!pCS ||
                lastInputWidth != inDesc.Width ||
                lastInputHeight != inDesc.Height ||
                lastOutputWidth != outDesc.Width ||
                lastOutputHeight != outDesc.Height);

            if (needsReinit)
            {
                if (pFSRInputTex) { pFSRInputTex->Release();   pFSRInputTex = nullptr; }
                if (pFSROutputTex) { pFSROutputTex->Release();  pFSROutputTex = nullptr; }
                if (pRCASOutputTex) { pRCASOutputTex->Release(); pRCASOutputTex = nullptr; }

                if (pSRV) { pSRV->Release();       pSRV = nullptr; }
                if (pRCASSRV) { pRCASSRV->Release();   pRCASSRV = nullptr; }

                if (pUAV) { pUAV->Release();       pUAV = nullptr; }
                if (pRCASUAV) { pRCASUAV->Release();   pRCASUAV = nullptr; }

                if (pCS) { pCS->Release();        pCS = nullptr; }
                if (pRCASCS) { pRCASCS->Release();    pRCASCS = nullptr; }

                if (pCB) { pCB->Release();        pCB = nullptr; }
                if (pRCASCB) { pRCASCB->Release();    pRCASCB = nullptr; }

                if (pSampler) { pSampler->Release();   pSampler = nullptr; }

                D3D11_TEXTURE2D_DESC inputTexDesc{};
                inputTexDesc.Width = inDesc.Width;
                inputTexDesc.Height = inDesc.Height;
                inputTexDesc.MipLevels = 1;
                inputTexDesc.ArraySize = 1;
                inputTexDesc.Format = outDesc.Format;
                inputTexDesc.SampleDesc.Count = 1;
                inputTexDesc.Usage = D3D11_USAGE_DEFAULT;
                inputTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                if (FAILED(pDevice->CreateTexture2D(&inputTexDesc, nullptr, &pFSRInputTex)))
                    goto cleanup;

                D3D11_TEXTURE2D_DESC outputTexDesc{};
                outputTexDesc.Width = outDesc.Width;
                outputTexDesc.Height = outDesc.Height;
                outputTexDesc.MipLevels = 1;
                outputTexDesc.ArraySize = 1;
                outputTexDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                outputTexDesc.SampleDesc.Count = 1;
                outputTexDesc.SampleDesc.Quality = 0;
                outputTexDesc.Usage = D3D11_USAGE_DEFAULT;
                outputTexDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
                outputTexDesc.CPUAccessFlags = 0;
                outputTexDesc.MiscFlags = 0;

                if (FAILED(pDevice->CreateTexture2D(&outputTexDesc, nullptr, &pFSROutputTex)))
                    goto cleanup;

                D3D11_TEXTURE2D_DESC rcasOutputDesc{};
                rcasOutputDesc.Width = outDesc.Width;
                rcasOutputDesc.Height = outDesc.Height;
                rcasOutputDesc.MipLevels = 1;
                rcasOutputDesc.ArraySize = 1;
                rcasOutputDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                rcasOutputDesc.SampleDesc.Count = 1;
                rcasOutputDesc.SampleDesc.Quality = 0;
                rcasOutputDesc.Usage = D3D11_USAGE_DEFAULT;
                rcasOutputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                rcasOutputDesc.CPUAccessFlags = 0;
                rcasOutputDesc.MiscFlags = 0;

                if (FAILED(pDevice->CreateTexture2D(&rcasOutputDesc, nullptr, &pRCASOutputTex)))
                    goto cleanup;

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                srvDesc.Texture2D.MostDetailedMip = 0;
                if (FAILED(pDevice->CreateShaderResourceView(pFSRInputTex, &srvDesc, &pSRV)))
                    goto cleanup;

                if (FAILED(pDevice->CreateShaderResourceView(pFSROutputTex, &srvDesc, &pRCASSRV)))
                    goto cleanup;

                D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = 0;
                if (FAILED(pDevice->CreateUnorderedAccessView(pFSROutputTex, &uavDesc, &pUAV)))
                    goto cleanup;

                if (FAILED(pDevice->CreateUnorderedAccessView(pRCASOutputTex, &uavDesc, &pRCASUAV)))
                    goto cleanup;

                D3D11_SAMPLER_DESC sampDesc{};
                sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
                sampDesc.MinLOD = 0;
                sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
                sampDesc.MaxAnisotropy = 1;
                if (FAILED(pDevice->CreateSamplerState(&sampDesc, &pSampler)))
                    goto cleanup;

                ID3DBlob* blob = nullptr;
                ID3DBlob* error = nullptr;
                HRESULT hr = D3DCompileFromFile(L"fsr1.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                    "CSMain", "cs_5_0", 0, 0, &blob, &error);
                if (FAILED(hr))
                {
                    if (error)
                        error->Release();
                    goto cleanup;
                }

                if (FAILED(pDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &pCS)))
                {
                    blob->Release();
                    goto cleanup;
                }
                blob->Release();

                hr = D3DCompileFromFile(L"rcas.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                    "CSMain", "cs_5_0", 0, 0, &blob, &error);
                if (FAILED(hr))
                {
                    if (error)
                        error->Release();
                    goto cleanup;
                }

                if (FAILED(pDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &pRCASCS)))
                {
                    blob->Release();
                    goto cleanup;
                }
                blob->Release();

                D3D11_BUFFER_DESC cbd{};
                cbd.Usage = D3D11_USAGE_DEFAULT;
                cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                cbd.ByteWidth = sizeof(FSRConstants);
                if (FAILED(pDevice->CreateBuffer(&cbd, nullptr, &pCB)))
                    goto cleanup;

                if (FAILED(pDevice->CreateBuffer(&cbd, nullptr, &pRCASCB)))
                    goto cleanup;

                lastInputWidth = inDesc.Width;
                lastInputHeight = inDesc.Height;
                lastOutputWidth = outDesc.Width;
                lastOutputHeight = outDesc.Height;
            }

            if (pCS && pRCASCS && pSRV && pRCASSRV && pUAV && pRCASUAV && pCB && pRCASCB && pSampler && pFSRInputTex && pFSROutputTex && pRCASOutputTex)
            {
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vpDesc = {};
                vpDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                vpDesc.Texture2D.MipSlice = 0;

                ID3D11VideoProcessorEnumerator* pEnumerator = nullptr;
                D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
                contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
                contentDesc.InputFrameRate.Numerator = 30;
                contentDesc.InputFrameRate.Denominator = 1;
                contentDesc.InputWidth = inDesc.Width;
                contentDesc.InputHeight = inDesc.Height;
                contentDesc.OutputWidth = inDesc.Width;
                contentDesc.OutputHeight = inDesc.Height;
                contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

                pVideoDevice->CreateVideoProcessorEnumerator(&contentDesc, &pEnumerator);

                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = inDesc.Width;
                desc.Height = inDesc.Height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = outDesc.Format;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                ID3D11Texture2D* pOutputTexture = nullptr;
                HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &pOutputTexture);

                ID3D11VideoProcessor* xxID3D11VideoProcessor = nullptr;
                pVideoDevice->CreateVideoProcessor(pEnumerator, 0, &xxID3D11VideoProcessor);

                ID3D11VideoProcessorOutputView* pVideoProcessorOutputView = nullptr;
                hr = pVideoDevice->CreateVideoProcessorOutputView(
                    pOutputTexture,
                    pEnumerator,
                    &vpDesc,
                    &pVideoProcessorOutputView
                );

                oVideoProcessorBlt(
                    pVideoContext,
                    xxID3D11VideoProcessor,
                    pVideoProcessorOutputView,
                    OutputFrame,
                    StreamCount,
                    pStreams
                );

                pVideoProcessorOutputView->Release();
                xxID3D11VideoProcessor->Release();
                pEnumerator->Release();

                pContext->CopyResource(pFSRInputTex, pOutputTexture);
                pOutputTexture->Release();

                pContext->CSSetShader(pCS, nullptr, 0);
                pContext->CSSetShaderResources(0, 1, &pSRV);
                pContext->CSSetUnorderedAccessViews(0, 1, &pUAV, nullptr);
                pContext->CSSetSamplers(0, 1, &pSampler);

                FSRConstants fsr{};
                float inputW = (float)lastInputWidth;
                float inputH = (float)lastInputHeight;

                FsrEasuCon(reinterpret_cast<AU1*>(&fsr.Const0),
                    reinterpret_cast<AU1*>(&fsr.Const1),
                    reinterpret_cast<AU1*>(&fsr.Const2),
                    reinterpret_cast<AU1*>(&fsr.Const3),
                    static_cast<AF1>(inputW),
                    static_cast<AF1>(inputH),
                    static_cast<AF1>(inputW),
                    static_cast<AF1>(inputH),
                    (AF1)lastOutputWidth,
                    (AF1)lastOutputHeight);
                fsr.Sample.x = 0;

                pContext->UpdateSubresource(pCB, 0, nullptr, &fsr, 0, 0);
                pContext->CSSetConstantBuffers(0, 1, &pCB);

                UINT dispatchX = (lastOutputWidth + 15) / 16;
                UINT dispatchY = (lastOutputHeight + 15) / 16;
                pContext->Dispatch(dispatchX, dispatchY, 1);

                ID3D11ShaderResourceView* nullSRV = nullptr;
                ID3D11UnorderedAccessView* nullUAV = nullptr;
                pContext->CSSetShaderResources(0, 1, &nullSRV);
                pContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

                pContext->CSSetShader(pRCASCS, nullptr, 0);
                pContext->CSSetShaderResources(0, 1, &pRCASSRV);
                pContext->CSSetUnorderedAccessViews(0, 1, &pRCASUAV, nullptr);
                pContext->CSSetSamplers(0, 1, &pSampler);

                FSRConstants rcas{};
                float sharpness = 0.0f;
                FsrRcasCon(reinterpret_cast<AU1*>(&rcas.Const0), sharpness);

                pContext->UpdateSubresource(pRCASCB, 0, nullptr, &rcas, 0, 0);
                pContext->CSSetConstantBuffers(0, 1, &pRCASCB);

                pContext->Dispatch(dispatchX, dispatchY, 1);

                pContext->CSSetShaderResources(0, 1, &nullSRV);
                pContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
                pContext->CSSetShader(nullptr, nullptr, 0);

                pContext->CopyResource(pOutputTex, pRCASOutputTex);

                if (pInputRes) pInputRes->Release();
                if (pOutputRes) pOutputRes->Release();
                pContext->Release();
                pDevice->Release();
                pVideoDevice->Release();

                return S_OK;
            }
        }
    }

cleanup:
    if (pInputRes) pInputRes->Release();
    if (pOutputRes) pOutputRes->Release();
    if (pContext) pContext->Release();
    if (pDevice) pDevice->Release();
    if (pVideoDevice) pVideoDevice->Release();

    return oVideoProcessorBlt(pVideoContext, pVideoProcessor, pView, OutputFrame, StreamCount, pStreams);
}

SIZE_T GetHookSize(void* target, SIZE_T minSize = 14)
{
    hde64s hs;
    SIZE_T totalSize = 0;
    BYTE* code = (BYTE*)target;

    while (totalSize < minSize)
    {
        unsigned int len = hde64_disasm(code + totalSize, &hs);
        if (hs.flags & F_ERROR)
            return minSize;

        totalSize += len;
    }

    return totalSize;
}

DWORD WINAPI InstallHook(LPVOID)
{
    if (!GetModuleHandleA("d3d11"))
        return 0;
    if (!GetModuleHandleA("DXCore"))
        return 0;
    if (!GetModuleHandleA("mf"))
        return 0;

    pFSRInputTex = nullptr;
    pFSROutputTex = nullptr;
    pRCASOutputTex = nullptr;
    pSRV = nullptr;
    pRCASSRV = nullptr;
    pUAV = nullptr;
    pRCASUAV = nullptr;
    pCS = nullptr;
    pRCASCS = nullptr;
    pCB = nullptr;
    pRCASCB = nullptr;
    pSampler = nullptr;
    lastInputWidth = 0;
    lastInputHeight = 0;
    lastOutputWidth = 0;
    lastOutputHeight = 0;

    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        &featureLevel,
        1,
        D3D11_SDK_VERSION,
        &pDevice,
        nullptr,
        &pContext
    );

    if (FAILED(hr))
        return 0;

    ID3D11VideoDevice* pVideoDevice = nullptr;
    pDevice->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&pVideoDevice);

    if (!pVideoDevice)
    {
        pContext->Release();
        pDevice->Release();
        return 0;
    }

    ID3D11VideoContext* pVideoContext = nullptr;
    pContext->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&pVideoContext);

    if (!pVideoContext)
    {
        pVideoDevice->Release();
        pContext->Release();
        pDevice->Release();
        return 0;
    }

    void** vTable = *(void***)pVideoContext;

    g_VPBltHook.target = vTable[53];
    g_VPBltHook.hook = (void*)hkVideoProcessorBlt;

    if (CreateDetour(g_VPBltHook, GetHookSize(g_VPBltHook.target)))
        oVideoProcessorBlt = (VideoProcessorBlt_t)g_VPBltHook.trampoline;

    pVideoContext->Release();
    pVideoDevice->Release();
    pContext->Release();
    pDevice->Release();

    return 0;
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InstallHook, nullptr, 0, nullptr);
    }
    return TRUE;
}
