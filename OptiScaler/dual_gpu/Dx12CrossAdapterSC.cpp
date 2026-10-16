#include "pch.h"
#include "Dx12CrossAdapterSC.h"
#include "DualGpu.h"

#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <misc/HiddenWindow.h>

#include <Config.h>
#include <State.h>

namespace
{
template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}
} // namespace

HRESULT Dx12CrossAdapterSC::CreateForHwnd(IDXGIFactory* factory, PFN_CreateSwapChainForHwnd originalCreate,
                                          IDXGIFactory* fgFactory, ID3D12CommandQueue* gameQueue, HWND hWnd,
                                          const DXGI_SWAP_CHAIN_DESC1* desc,
                                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                          IDXGIOutput* restrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (factory == nullptr || gameQueue == nullptr || desc == nullptr || ppSwapChain == nullptr)
        return E_INVALIDARG;

    if (desc->SampleDesc.Count > 1)
    {
        LOG_WARN("DualGpu: MSAA swapchains are not supported");
        return E_NOTIMPL;
    }

    ID3D12Device* gameDevice = nullptr;
    if (FAILED(gameQueue->GetDevice(IID_PPV_ARGS(&gameDevice))) || gameDevice == nullptr)
        return E_FAIL;

    // The game's device outlives its swapchain
    gameDevice->Release();

    auto& dual = DualGpu::Instance();
    if (!dual.Init(gameDevice, gameQueue, hWnd))
        return E_FAIL;

    HWND hiddenHwnd = nullptr;
    try
    {
        hiddenHwnd = CreateHiddenSwapchainWindow();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("DualGpu: hidden window creation failed: {}", e.what());
        return E_FAIL;
    }

    // The game's back buffers: a plain swapchain on its own GPU, never presented
    IDXGIFactory2* factory2 = nullptr;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
        return E_NOINTERFACE;

    DXGI_SWAP_CHAIN_DESC1 gameDesc = *desc;
    if (gameDesc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && gameDesc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        gameDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    if (gameDesc.BufferCount < 2)
        gameDesc.BufferCount = 2;

    IDXGISwapChain1* gameSwapChain = nullptr;
    HRESULT result;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        if (originalCreate != nullptr)
            result = originalCreate(factory2, gameQueue, hiddenHwnd, &gameDesc, nullptr, nullptr, &gameSwapChain);
        else
            result =
                factory2->CreateSwapChainForHwnd(gameQueue, hiddenHwnd, &gameDesc, nullptr, nullptr, &gameSwapChain);
    }

    factory2->Release();

    if (FAILED(result) || gameSwapChain == nullptr)
    {
        LOG_ERROR("DualGpu: the game GPU's swapchain failed: {:X}", (UINT) result);
        return FAILED(result) ? result : E_FAIL;
    }

    // FSR frame generation's swapchain on the second GPU, presenting to the game's window
    auto& state = State::Instance();
    DXGI_SWAP_CHAIN_DESC1 presentDesc = *desc;
    IDXGISwapChain1* presentSwapChain1 = nullptr;

    dual.SetActive(true);
    result = FGHooks::CreateSwapChainForHwnd(fgFactory, dual.SecondQueue(), hWnd, &presentDesc, fullscreenDesc,
                                             restrictToOutput, &presentSwapChain1);

    IDXGISwapChain4* presentSwapChain = nullptr;
    if (SUCCEEDED(result) && presentSwapChain1 != nullptr)
        presentSwapChain1->QueryInterface(IID_PPV_ARGS(&presentSwapChain));

    SafeRelease(presentSwapChain1);

    if (presentSwapChain == nullptr)
    {
        LOG_ERROR("DualGpu: frame generation's swapchain on the second GPU failed: {:X}", (UINT) result);
        dual.SetActive(false);
        gameSwapChain->Release();
        return FAILED(result) ? result : E_FAIL;
    }

    // Frame generation's swapchain registered itself as the current one; the game talks to this wrapper
    auto wrapper = new Dx12CrossAdapterSC(gameSwapChain, presentSwapChain, hWnd);

    presentSwapChain->GetDesc(&state.currentSwapchainDesc);
    state.currentSwapchainDesc.OutputWindow = hWnd;
    state.currentCommandQueue = gameQueue;
    state.swapchainInteropApi = SwapchainInteropApi::Dx12CrossAdapter;
    state.currentSwapchain = wrapper;

    *ppSwapChain = wrapper;

    LOG_INFO("DualGpu: created the dual GPU swapchain, {}x{}, {} buffers, presenting on {}", desc->Width, desc->Height,
             gameDesc.BufferCount, dual.SecondGpuName());

    return S_OK;
}

HRESULT Dx12CrossAdapterSC::CreateForDesc(IDXGIFactory* factory, PFN_CreateSwapChainForHwnd originalCreate,
                                          IDXGIFactory* fgFactory, ID3D12CommandQueue* gameQueue,
                                          const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppSwapChain)
{
    if (desc == nullptr || ppSwapChain == nullptr)
        return E_INVALIDARG;

    DXGI_SWAP_CHAIN_DESC1 desc1 {};
    desc1.Width = desc->BufferDesc.Width;
    desc1.Height = desc->BufferDesc.Height;
    desc1.Format = desc->BufferDesc.Format;
    desc1.SampleDesc = desc->SampleDesc;
    desc1.BufferUsage = desc->BufferUsage;
    desc1.BufferCount = desc->BufferCount;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.SwapEffect = desc->SwapEffect;
    desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Flags = desc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc {};
    fullscreenDesc.RefreshRate = desc->BufferDesc.RefreshRate;
    fullscreenDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreenDesc.Scaling = desc->BufferDesc.Scaling;
    fullscreenDesc.Windowed = desc->Windowed;

    IDXGISwapChain1* swapChain1 = nullptr;
    auto result = CreateForHwnd(factory, originalCreate, fgFactory, gameQueue, desc->OutputWindow, &desc1,
                                &fullscreenDesc, nullptr, &swapChain1);

    if (SUCCEEDED(result))
        *ppSwapChain = swapChain1;

    return result;
}

Dx12CrossAdapterSC::Dx12CrossAdapterSC(IDXGISwapChain1* gameSwapChain, IDXGISwapChain4* presentSwapChain, HWND hWnd)
    : _gameSwapChain(gameSwapChain), _presentSwapChain(presentSwapChain), _handle(hWnd)
{
    // Takes over the creation references of both swapchains
    _gameSwapChain->QueryInterface(IID_PPV_ARGS(&_gameSwapChain2));
    _gameSwapChain->QueryInterface(IID_PPV_ARGS(&_gameSwapChain3));
}

Dx12CrossAdapterSC::~Dx12CrossAdapterSC()
{
    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    // Frame generation's swapchain is torn down by FGHooks when its last reference goes
    SafeRelease(_presentSwapChain);
    SafeRelease(_gameSwapChain3);
    SafeRelease(_gameSwapChain2);
    SafeRelease(_gameSwapChain);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4))
    {
        AddRef();
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    if (riid == __uuidof(Dx12CrossAdapterSC))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx12CrossAdapterSC::AddRef() { return InterlockedIncrement(&_refcount); }

ULONG STDMETHODCALLTYPE Dx12CrossAdapterSC::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    if (ret == 0)
    {
        LOG_INFO("DualGpu: releasing the dual GPU swapchain");

        auto& state = State::Instance();
        auto& dual = DualGpu::Instance();

        dual.ReleaseSurfaces();

        if (state.currentSwapchain == this)
            state.currentSwapchain = nullptr;

        state.swapchainInteropApi = SwapchainInteropApi::None;
        dual.SetActive(false);

        delete this;
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _gameSwapChain->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _gameSwapChain->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _gameSwapChain->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetParent(REFIID riid, void** ppParent)
{
    return _gameSwapChain->GetParent(riid, ppParent);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetDevice(REFIID riid, void** ppDevice)
{
    return _gameSwapChain->GetDevice(riid, ppDevice);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::Present(UINT SyncInterval, UINT Flags)
{
    if (_gameSwapChain == nullptr || _presentSwapChain == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _presentSwapChain->Present(SyncInterval, Flags);

    ID3D12Resource* backBuffer = nullptr;
    ID3D12Resource* target = nullptr;

    _gameSwapChain->GetBuffer(GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
    _presentSwapChain->GetBuffer(_presentSwapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target));

    if (!DualGpu::Instance().TransferFrame(backBuffer, target))
    {
        static UINT64 failures = 0;
        if ((failures++ % 300) == 0)
            LOG_WARN("DualGpu: frame transfer failed ({} times)", failures);
    }

    SafeRelease(backBuffer);
    SafeRelease(target);

    // Through FGHooks::FGPresent, which dispatches frame generation on the second GPU
    return _presentSwapChain->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return _gameSwapChain->GetBuffer(Buffer, riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    State::Instance().realExclusiveFullscreen = Fullscreen;
    return _presentSwapChain->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return _presentSwapChain->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    auto result = _gameSwapChain->GetDesc(pDesc);

    if (SUCCEEDED(result) && pDesc != nullptr)
    {
        pDesc->OutputWindow = _handle;

        BOOL fullscreen = FALSE;
        if (SUCCEEDED(_presentSwapChain->GetFullscreenState(&fullscreen, nullptr)))
            pDesc->Windowed = !fullscreen;
    }

    return result;
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    LOG_INFO("DualGpu: ResizeBuffers {} buffers, {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
             (UINT) NewFormat, SwapChainFlags);

    DualGpu::Instance().ReleaseSurfaces();
    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    auto gameResult = _gameSwapChain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    if (FAILED(gameResult))
    {
        LOG_ERROR("DualGpu: the game GPU's ResizeBuffers failed: {:X}", (UINT) gameResult);
        return gameResult;
    }

    auto result = _presentSwapChain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(result))
    {
        auto& state = State::Instance();

        if (Config::Instance()->IsFGEnabled())
        {
            state.fgResetCapturedResources = true;
            state.fgOnlyUseCapturedResources = false;
            state.fgChanged = true;
        }

        state.scChanged = true;
        state.SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

        if (state.currentFeature == nullptr)
        {
            state.screenWidth = static_cast<float>(Width);
            state.screenHeight = static_cast<float>(Height);
        }
    }
    else
    {
        LOG_ERROR("DualGpu: the second GPU's ResizeBuffers failed: {:X}", (UINT) result);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _presentSwapChain->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _presentSwapChain->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _presentSwapChain->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _presentSwapChain->GetLastPresentCount(pLastPresentCount);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _gameSwapChain->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return _presentSwapChain->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetHwnd(HWND* pHwnd)
{
    if (pHwnd == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    *pHwnd = _handle;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _gameSwapChain->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::Present1(UINT SyncInterval, UINT Flags,
                                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    UNREFERENCED_PARAMETER(pPresentParameters);
    return Present(SyncInterval, Flags);
}

BOOL STDMETHODCALLTYPE Dx12CrossAdapterSC::IsTemporaryMonoSupported(void) { return FALSE; }

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _presentSwapChain->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _presentSwapChain->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _presentSwapChain->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _presentSwapChain->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _presentSwapChain->GetRotation(pRotation);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetSourceSize(UINT Width, UINT Height)
{
    if (_gameSwapChain2 != nullptr)
        _gameSwapChain2->SetSourceSize(Width, Height);

    return _presentSwapChain->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _presentSwapChain->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetMaximumFrameLatency(UINT MaxLatency)
{
    return _presentSwapChain->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _presentSwapChain->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE Dx12CrossAdapterSC::GetFrameLatencyWaitableObject(void)
{
    return _presentSwapChain->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _presentSwapChain->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _presentSwapChain->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE Dx12CrossAdapterSC::GetCurrentBackBufferIndex(void)
{
    // The game GPU's swapchain is never presented, so its current buffer never changes, and the game always renders
    // into it. Only a swapchain's current buffer may be written: on hybrid laptops writing another one removes the
    // device with DXGI_ERROR_ACCESS_DENIED. The handoff copy is ordered on the game's queue before the next frame.
    return _gameSwapChain3 != nullptr ? _gameSwapChain3->GetCurrentBackBufferIndex() : 0;
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                     UINT* pColorSpaceSupport)
{
    return _presentSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    return _presentSwapChain->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                             DXGI_FORMAT Format, UINT SwapChainFlags,
                                                             const UINT* pCreationNodeMask,
                                                             IUnknown* const* ppPresentQueue)
{
    // The game's present queues belong to its own GPU; the second GPU keeps its queue
    UNREFERENCED_PARAMETER(pCreationNodeMask);
    UNREFERENCED_PARAMETER(ppPresentQueue);
    return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE Dx12CrossAdapterSC::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    return _presentSwapChain->SetHDRMetaData(Type, Size, pMetaData);
}
