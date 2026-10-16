#pragma once
#include "SysUtils.h"

#include <hooks/Hook_Utils.h>

#include <dxgi1_6.h>
#include <d3d12.h>

// Game-facing swapchain for dual GPU mode (see DualGpu.h).
//
// The game gets its back buffers from a swapchain on its own GPU that is never presented (it sits on a hidden window).
// On Present the back buffer is carried to the second GPU and written into the back buffer of FSR frame generation's
// swapchain there, which presents to the game's window. Modelled on Dx11wDx12SC.
class DECLSPEC_UUID("6c1f4d2a-9b7e-4f0c-a3d8-2e5b7c9f1a46") Dx12CrossAdapterSC final : public IDXGISwapChain4
{
  public:
    using PFN_CreateSwapChainForHwnd = rewrite_signature<decltype(&IDXGIFactory2::CreateSwapChainForHwnd)>::type;

    // Creates the dual GPU swapchain. factory creates the plain swapchain on the game's GPU, through originalCreate
    // when the factory is hooked (else directly); fgFactory is what FGHooks gets for frame generation's swapchain.
    // The caller holds its ScopedSkipFGSCCreation. Fails, with the reason logged, when dual GPU mode can't run.
    static HRESULT CreateForHwnd(IDXGIFactory* factory, PFN_CreateSwapChainForHwnd originalCreate,
                                 IDXGIFactory* fgFactory, ID3D12CommandQueue* gameQueue, HWND hWnd,
                                 const DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                 IDXGIOutput* restrictToOutput, IDXGISwapChain1** ppSwapChain);

    // The same for IDXGIFactory::CreateSwapChain
    static HRESULT CreateForDesc(IDXGIFactory* factory, PFN_CreateSwapChainForHwnd originalCreate,
                                 IDXGIFactory* fgFactory, ID3D12CommandQueue* gameQueue,
                                 const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppSwapChain);

    Dx12CrossAdapterSC(IDXGISwapChain1* gameSwapChain, IDXGISwapChain4* presentSwapChain, HWND hWnd);
    virtual ~Dx12CrossAdapterSC();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                            UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override;

    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override;

  private:
    IDXGISwapChain1* _gameSwapChain = nullptr; // On the game's GPU, never presented
    IDXGISwapChain2* _gameSwapChain2 = nullptr;
    IDXGISwapChain3* _gameSwapChain3 = nullptr;
    IDXGISwapChain4* _presentSwapChain = nullptr; // FSR frame generation's swapchain on the second GPU

    HWND _handle = nullptr;
    LONG _refcount = 1;
};
