#include "pch.h"
#include "DualGpu.h"

#include <Config.h>
#include <State.h>

#include <misc/IdentifyGpu.h>

#include <magic_enum.hpp>

#include <algorithm>

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

void SafeCloseHandle(HANDLE& handle)
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    if (list == nullptr || resource == nullptr || before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

// Shares a heap or fence created on owner with other through an NT handle
template <typename T>
bool OpenOnOtherDevice(ID3D12Device* owner, ID3D12Device* other, ID3D12DeviceChild* object, T** opened)
{
    HANDLE handle = nullptr;
    auto result = owner->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(result))
    {
        LOG_ERROR("DualGpu: CreateSharedHandle failed: {:X}", (UINT) result);
        return false;
    }

    result = other->OpenSharedHandle(handle, IID_PPV_ARGS(opened));
    CloseHandle(handle);

    if (FAILED(result))
    {
        LOG_ERROR("DualGpu: OpenSharedHandle failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

UINT64 AlignUp(UINT64 value, UINT64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return value;
}

bool WaitFence(ID3D12Fence* fence, UINT64 value, HANDLE event, DWORD timeoutMs, const char* what)
{
    if (fence == nullptr || value == 0 || fence->GetCompletedValue() >= value)
        return true;

    if (FAILED(fence->SetEventOnCompletion(value, event)))
        return false;

    if (WaitForSingleObject(event, timeoutMs) != WAIT_OBJECT_0)
    {
        LOG_ERROR("DualGpu: timed out waiting for {}, fence value {}, completed {}", what, value,
                  fence->GetCompletedValue());
        return false;
    }

    return true;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}
} // namespace

DualGpu& DualGpu::Instance()
{
    static DualGpu instance;
    return instance;
}

bool DualGpu::Enabled()
{
    return Config::Instance()->DualGpuEnabled.value_or_default() && State::Instance().activeFgOutput == FGOutput::FSRFG;
}

bool DualGpu::IsSecondGpu(IUnknown* obj) const
{
    if (_secondDevice == nullptr || obj == nullptr)
        return false;

    ID3D12Device* device = nullptr;
    if (obj->QueryInterface(IID_PPV_ARGS(&device)) != S_OK)
    {
        ID3D12DeviceChild* child = nullptr;
        if (obj->QueryInterface(IID_PPV_ARGS(&child)) == S_OK)
        {
            child->GetDevice(IID_PPV_ARGS(&device));
            child->Release();
        }
    }

    if (device == nullptr)
        return false;

    // By adapter, so wrappers and the frame generation library's own queues count too
    auto luid = device->GetAdapterLuid();
    device->Release();

    return IsEqualLUID(luid, _secondLuid);
}

IDXGIAdapter1* DualGpu::PickAdapter(LUID gameLuid, HWND hwnd)
{
    ScopedSkipSpoofingGlobal skipSpoofing {};

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory == nullptr)
    {
        LOG_ERROR("DualGpu: CreateDXGIFactory1 failed");
        return nullptr;
    }

    const auto setting = ToLower(Config::Instance()->DualGpuPresentAdapter.value_or_default());
    const auto isAuto = setting.empty() || setting == "auto";
    const HMONITOR monitor = hwnd != nullptr ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;

    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* fallback = nullptr;

    for (UINT i = 0;; i++)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);

        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        const bool isGameAdapter = IsEqualLUID(desc.AdapterLuid, gameLuid);
        const auto name = wstring_to_string(desc.Description);

        // On hybrid laptops Windows reports the monitor on the GPU the game runs on, so this can be wrong there; the
        // fallback below still finds the other GPU
        bool ownsMonitor = false;
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++)
        {
            DXGI_OUTPUT_DESC outputDesc {};
            if (output->GetDesc(&outputDesc) == S_OK && outputDesc.Monitor == monitor)
                ownsMonitor = true;

            output->Release();
        }

        LOG_INFO("DualGpu: adapter {}: {}, luid {:X}:{:X}, game's GPU: {}, owns the game's monitor: {}", i, name,
                 (UINT) desc.AdapterLuid.HighPart, (UINT) desc.AdapterLuid.LowPart, isGameAdapter, ownsMonitor);

        if (software || isGameAdapter)
        {
            adapter->Release();
            continue;
        }

        bool match = false;
        if (setting.rfind("index:", 0) == 0)
        {
            match = std::to_string(i) == setting.substr(6);
        }
        else if (setting.rfind("luid:", 0) == 0)
        {
            auto luid = std::format("{:x}:{:x}", (UINT) desc.AdapterLuid.HighPart, (UINT) desc.AdapterLuid.LowPart);
            match = luid == setting.substr(5);
        }
        else if (!isAuto)
        {
            match = ToLower(name).find(setting) != std::string::npos;
        }
        else
        {
            match = ownsMonitor;
        }

        if (match && chosen == nullptr)
            chosen = adapter;
        else if (isAuto && fallback == nullptr)
            fallback = adapter;
        else
            adapter->Release();
    }

    if (chosen == nullptr)
    {
        chosen = fallback;
        fallback = nullptr;
    }

    SafeRelease(fallback);
    factory->Release();

    return chosen;
}

bool DualGpu::Init(ID3D12Device* gameDevice, ID3D12CommandQueue* gameQueue, HWND hwnd)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (gameDevice == nullptr || gameQueue == nullptr)
        return false;

    if (_secondDevice != nullptr)
    {
        if (_gameDevice == gameDevice && _gameQueue == gameQueue)
            return true;

        LOG_INFO("DualGpu: the game's device or queue changed, rebuilding its side");
        ReleaseGameSide();
    }
    else if (!CreateSecondGpu(gameDevice, hwnd))
    {
        return false;
    }

    _gameDevice = gameDevice;
    _gameQueue = gameQueue;

    if (!CreateGameSide())
    {
        ReleaseGameSide();
        return false;
    }

    return true;
}

bool DualGpu::CreateSecondGpu(ID3D12Device* gameDevice, HWND hwnd)
{
    auto adapter = PickAdapter(gameDevice->GetAdapterLuid(), hwnd);
    if (adapter == nullptr)
    {
        LOG_WARN("DualGpu: no second GPU found");
        return false;
    }

    DXGI_ADAPTER_DESC1 desc {};
    adapter->GetDesc1(&desc);

    // The create device hook must not adopt the second GPU's device as the game's
    auto& state = State::Instance();
    auto gameDeviceInState = state.currentD3D12Device;

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_secondDevice));
    }

    state.currentD3D12Device = gameDeviceInState;
    adapter->Release();

    if (FAILED(result) || _secondDevice == nullptr)
    {
        LOG_ERROR("DualGpu: D3D12CreateDevice on {} failed: {:X}", wstring_to_string(desc.Description), (UINT) result);
        _secondDevice = nullptr;
        return false;
    }

    _secondDevice->SetName(L"DualGpu second GPU device");

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    result = _secondDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_secondQueue));
    if (FAILED(result))
    {
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        result = _secondDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_secondQueue));
    }

    const D3D12_FENCE_FLAGS shared = D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER;

    if (FAILED(result) || FAILED(_secondDevice->CreateFence(0, shared, IID_PPV_ARGS(&_consumed))) ||
        !CreateRing(_secondDevice, _secondRing, L"DualGpu second GPU", D3D12_COMMAND_LIST_TYPE_DIRECT) ||
        (_consumedEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr)) == nullptr)
    {
        LOG_ERROR("DualGpu: creating the second GPU's queue, fence or command lists failed");
        ReleaseRing(_secondRing);
        SafeCloseHandle(_consumedEvent);
        SafeRelease(_consumed);
        SafeRelease(_secondQueue);
        SafeRelease(_secondDevice);
        return false;
    }

    _secondQueue->SetName(L"DualGpu second GPU queue");
    _secondLuid = desc.AdapterLuid;
    _secondName = wstring_to_string(desc.Description);

    LOG_INFO("DualGpu: second GPU is {}, luid {:X}:{:X}", _secondName, (UINT) _secondLuid.HighPart,
             (UINT) _secondLuid.LowPart);

    return true;
}

bool DualGpu::CreateGameSide()
{
    const D3D12_FENCE_FLAGS shared = D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER;

    if (FAILED(_gameDevice->CreateFence(_frameValue, shared, IID_PPV_ARGS(&_ready))) ||
        !OpenOnOtherDevice(_gameDevice, _secondDevice, _ready, &_readyOnSecond) ||
        !OpenOnOtherDevice(_secondDevice, _gameDevice, _consumed, &_consumedOnGame) ||
        !CreateRing(_gameDevice, _gameRing, L"DualGpu game GPU", D3D12_COMMAND_LIST_TYPE_DIRECT))
    {
        LOG_ERROR("DualGpu: shared fences or the game GPU's command lists could not be created");
        return false;
    }

    // Capability check: one small heap shared across the two GPUs, so a missing feature shows up here, not mid-frame
    {
        D3D12_HEAP_DESC heapDesc {};
        heapDesc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

        ID3D12Heap* probe = nullptr;
        ID3D12Heap* probeOnSecond = nullptr;
        auto result = _gameDevice->CreateHeap(&heapDesc, IID_PPV_ARGS(&probe));
        const bool opened = SUCCEEDED(result) && OpenOnOtherDevice(_gameDevice, _secondDevice, probe, &probeOnSecond);
        SafeRelease(probeOnSecond);
        SafeRelease(probe);

        if (!opened)
        {
            LOG_ERROR("DualGpu: these GPUs can't share memory across adapters: {:X}", (UINT) result);
            return false;
        }
    }

    _useCopyQueue = Config::Instance()->DualGpuCopyQueue.value_or_default();
    if (_useCopyQueue)
    {
        D3D12_COMMAND_QUEUE_DESC copyDesc {};
        copyDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;

        if (FAILED(_gameDevice->CreateCommandQueue(&copyDesc, IID_PPV_ARGS(&_copyQueue))) ||
            FAILED(_gameDevice->CreateFence(_frameValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_staged))) ||
            !CreateRing(_gameDevice, _copyRing, L"DualGpu game GPU copy queue", D3D12_COMMAND_LIST_TYPE_COPY))
        {
            LOG_WARN("DualGpu: the game GPU's copy queue is unavailable, copying on the game's queue");
            ReleaseRing(_copyRing);
            SafeRelease(_staged);
            SafeRelease(_copyQueue);
            _useCopyQueue = false;
        }
        else
        {
            _copyQueue->SetName(L"DualGpu game GPU copy queue");
        }
    }

    LOG_INFO("DualGpu: frames cross on the game GPU's {} queue", _useCopyQueue ? "copy" : "graphics");
    return true;
}

void DualGpu::ReleaseGameSide()
{
    WaitIdleLocked();
    ReleaseSurfacesLocked();

    ReleaseRing(_copyRing);
    SafeRelease(_staged);
    SafeRelease(_copyQueue);
    _useCopyQueue = false;

    ReleaseRing(_gameRing);
    SafeRelease(_consumedOnGame);
    SafeRelease(_readyOnSecond);
    SafeRelease(_ready);

    _gameDevice = nullptr;
    _gameQueue = nullptr;
}

bool DualGpu::CreateRing(ID3D12Device* device, CommandRing& ring, const wchar_t* name, D3D12_COMMAND_LIST_TYPE type)
{
    for (UINT i = 0; i < kSlots; i++)
    {
        if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&ring.allocator[i]))) ||
            FAILED(device->CreateCommandList(0, type, ring.allocator[i], nullptr, IID_PPV_ARGS(&ring.list[i]))))
        {
            return false;
        }

        ring.list[i]->SetName(std::format(L"{} copy list {}", name, i).c_str());
        ring.list[i]->Close();
        ring.fenceValue[i] = 0;
    }

    ring.lastValue = 0;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ring.fence))))
        return false;

    ring.event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return ring.event != nullptr;
}

void DualGpu::ReleaseRing(CommandRing& ring)
{
    for (UINT i = 0; i < kSlots; i++)
    {
        SafeRelease(ring.list[i]);
        SafeRelease(ring.allocator[i]);
        ring.fenceValue[i] = 0;
    }

    SafeRelease(ring.fence);
    SafeCloseHandle(ring.event);
    ring.lastValue = 0;
}

ID3D12GraphicsCommandList* DualGpu::BeginRing(CommandRing& ring, UINT slot)
{
    if (!WaitFence(ring.fence, ring.fenceValue[slot], ring.event, 2000, "a copy list"))
        return nullptr;

    if (FAILED(ring.allocator[slot]->Reset()) || FAILED(ring.list[slot]->Reset(ring.allocator[slot], nullptr)))
    {
        LOG_ERROR("DualGpu: copy list reset failed");
        return nullptr;
    }

    return ring.list[slot];
}

bool DualGpu::SubmitRing(CommandRing& ring, UINT slot, ID3D12CommandQueue* queue)
{
    auto list = ring.list[slot];
    if (FAILED(list->Close()))
    {
        LOG_ERROR("DualGpu: copy list close failed");
        return false;
    }

    queue->ExecuteCommandLists(1, (ID3D12CommandList**) &list);
    ring.fenceValue[slot] = ++ring.lastValue;
    queue->Signal(ring.fence, ring.fenceValue[slot]);
    return true;
}

DXGI_FORMAT DualGpu::TextureFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;

    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;

    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        return DXGI_FORMAT_R16G16_FLOAT;

    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
        return DXGI_FORMAT_R32G32_FLOAT;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;

    // 24 bit depth packed with stencil has no plain format to copy into
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return DXGI_FORMAT_UNKNOWN;

    default:
        return format;
    }
}

void DualGpu::ReleaseSurface(SharedSurface& surface)
{
    SafeRelease(surface.shared);
    SafeRelease(surface.sharedOnSecond);
    SafeRelease(surface.staging);
    SafeRelease(surface.heap);
    SafeRelease(surface.heapOnSecond);
    SafeRelease(surface.texture);
    surface.size = 0;
    surface.bytes = 0;
    surface.sourceDesc = {};
    surface.footprint = {};
    surface.textureState = D3D12_RESOURCE_STATE_COMMON;
}

bool DualGpu::EnsureSurface(SharedSurface& surface, ID3D12Resource* source, bool needsTexture)
{
    auto desc = source->GetDesc();

    const bool sameSource = surface.shared != nullptr && surface.sourceDesc.Width == desc.Width &&
                            surface.sourceDesc.Height == desc.Height && surface.sourceDesc.Format == desc.Format;

    if (sameSource && (!needsTexture || surface.texture != nullptr))
        return true;

    // Subresource 0 is the depth plane of depth-stencil formats
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    _gameDevice->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);

    if (bytes == 0 || bytes == UINT64_MAX)
    {
        LOG_ERROR("DualGpu: no copyable footprint for {}", magic_enum::enum_name(desc.Format));
        return false;
    }

    const auto textureFormat = needsTexture ? TextureFormat(footprint.Footprint.Format) : DXGI_FORMAT_UNKNOWN;
    if (needsTexture && textureFormat == DXGI_FORMAT_UNKNOWN)
    {
        LOG_ERROR("DualGpu: {} inputs are not supported", magic_enum::enum_name(desc.Format));
        return false;
    }

    // Replacing a surface the GPUs may still be using (the game changed resolution): let both finish first
    if (surface.shared != nullptr)
        WaitIdleLocked();

    if (surface.shared == nullptr || bytes > surface.size)
    {
        ReleaseSurface(surface);

        const UINT64 size = AlignUp(bytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

        D3D12_HEAP_DESC heapDesc {};
        heapDesc.SizeInBytes = size;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

        auto result = _gameDevice->CreateHeap(&heapDesc, IID_PPV_ARGS(&surface.heap));
        if (FAILED(result) || !OpenOnOtherDevice(_gameDevice, _secondDevice, surface.heap, &surface.heapOnSecond))
        {
            LOG_ERROR("DualGpu: shared heap of {} bytes failed: {:X}", size, (UINT) result);
            ReleaseSurface(surface);
            return false;
        }

        // The shared buffers stay in COMMON: copies promote them to COPY_DEST on the game GPU and COPY_SOURCE on the
        // second GPU, and they decay back after each submission
        auto sharedDesc = BufferDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        if (FAILED(_gameDevice->CreatePlacedResource(surface.heap, 0, &sharedDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&surface.shared))) ||
            FAILED(_secondDevice->CreatePlacedResource(surface.heapOnSecond, 0, &sharedDesc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&surface.sharedOnSecond))))
        {
            LOG_ERROR("DualGpu: shared buffer creation failed");
            ReleaseSurface(surface);
            return false;
        }

        if (_useCopyQueue)
        {
            D3D12_HEAP_PROPERTIES localHeap {};
            localHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            auto stagingDesc = BufferDesc(size, D3D12_RESOURCE_FLAG_NONE);

            if (FAILED(_gameDevice->CreateCommittedResource(&localHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&surface.staging))))
            {
                LOG_ERROR("DualGpu: staging buffer creation failed");
                ReleaseSurface(surface);
                return false;
            }
        }

        surface.size = size;
    }

    surface.bytes = bytes;
    surface.sourceDesc = desc;
    surface.footprint = footprint;

    if (needsTexture)
    {
        SafeRelease(surface.texture);

        D3D12_RESOURCE_DESC textureDesc {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = footprint.Footprint.Width;
        textureDesc.Height = footprint.Footprint.Height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = textureFormat;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES heapProps {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto result = _secondDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&surface.texture));
        if (FAILED(result))
        {
            LOG_ERROR("DualGpu: second GPU input texture creation failed: {:X}", (UINT) result);
            return false;
        }

        surface.textureState = D3D12_RESOURCE_STATE_COPY_DEST;

        LOG_INFO("DualGpu: input {}x{} {} crosses as {}", footprint.Footprint.Width, footprint.Footprint.Height,
                 magic_enum::enum_name(desc.Format), magic_enum::enum_name(textureFormat));
    }

    return true;
}

void DualGpu::RecordCopyOut(ID3D12GraphicsCommandList* list, SharedSurface& surface, ID3D12Resource* source,
                            D3D12_RESOURCE_STATES state)
{
    Barrier(list, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // Into the game GPU's own memory when its copy queue does the PCIe part, otherwise straight into shared memory
    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = surface.staging != nullptr ? surface.staging : surface.shared;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = surface.footprint;

    D3D12_TEXTURE_COPY_LOCATION from {};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;

    list->CopyTextureRegion(&destination, 0, 0, 0, &from, nullptr);

    Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

void DualGpu::RecordCopyIn(ID3D12GraphicsCommandList* list, SharedSurface& surface, ID3D12Resource* target)
{
    // Same bytes the game GPU wrote, named in the target's format
    D3D12_TEXTURE_COPY_LOCATION from {};
    from.pResource = surface.sharedOnSecond;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = surface.footprint;
    from.PlacedFootprint.Footprint.Format = target->GetDesc().Format;

    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = target;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    list->CopyTextureRegion(&destination, 0, 0, 0, &from, nullptr);
}

void DualGpu::WaitForSlot(UINT64 consumedValue)
{
    if (_consumed == nullptr || consumedValue == 0)
        return;

    WaitFence(_consumed, consumedValue, _consumedEvent, 2000, "the second GPU");
}

bool DualGpu::DevicesHealthy()
{
    auto gameReason = _gameDevice != nullptr ? _gameDevice->GetDeviceRemovedReason() : S_OK;
    auto secondReason = _secondDevice != nullptr ? _secondDevice->GetDeviceRemovedReason() : S_OK;

    if (gameReason == S_OK && secondReason == S_OK)
        return true;

    if (_removedLogs++ < 10)
    {
        LOG_ERROR("DualGpu: device removed, game GPU {:X}, second GPU {:X}", (UINT) gameReason, (UINT) secondReason);
    }

    return false;
}

ID3D12Resource* DualGpu::StageInput(FG_ResourceType type, int fIndex, ID3D12GraphicsCommandList* cmdList,
                                    ID3D12Resource* source, D3D12_RESOURCE_STATES state)
{
    if (_secondDevice == nullptr || source == nullptr || fIndex < 0)
        return nullptr;

    // The presented image itself crosses in TransferFrame
    if (type == FG_ResourceType::Color)
    {
        if (!_loggedUnsupported[type])
        {
            LOG_WARN("DualGpu: {} inputs are not carried to the second GPU", magic_enum::enum_name(type));
            _loggedUnsupported[type] = true;
        }

        return nullptr;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    const UINT slot = (UINT) fIndex % kSlots;

    // The second GPU must be done with this slot's previous frame before the game GPU overwrites it
    WaitForSlot(_inputConsumed[slot][type]);

    auto& surface = _inputs[slot][type];
    if (!EnsureSurface(surface, source, true))
        return nullptr;

    std::erase_if(_pending, [&](const PendingInput& p) { return p.type == type && p.fIndex == fIndex; });

    PendingInput pending {};
    pending.type = type;
    pending.fIndex = fIndex;
    pending.slot = slot;

    if (cmdList != nullptr)
    {
        RecordCopyOut(cmdList, surface, source, state);
    }
    else
    {
        source->AddRef();
        pending.deferredSource = source;
        pending.deferredState = state;
    }

    _pending.push_back(pending);
    return surface.texture;
}

bool DualGpu::TransferFrame(ID3D12Resource* backBuffer, ID3D12Resource* target)
{
    if (_secondDevice == nullptr || backBuffer == nullptr || target == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(_mutex);

    if (!DevicesHealthy())
        return false;

    const UINT slot = _presentSlot;
    _presentSlot = (_presentSlot + 1) % kSlots;

    WaitForSlot(_colorConsumed[slot]);

    auto& color = _color[slot];
    if (!EnsureSurface(color, backBuffer, false))
        return false;

    // Game GPU: the back buffer, and inputs that came without a command list, out of the game's resources
    auto gameList = BeginRing(_gameRing, slot);
    if (gameList == nullptr)
        return false;

    RecordCopyOut(gameList, color, backBuffer, D3D12_RESOURCE_STATE_PRESENT);

    for (auto& pending : _pending)
    {
        if (pending.deferredSource == nullptr)
            continue;

        RecordCopyOut(gameList, _inputs[pending.slot][pending.type], pending.deferredSource, pending.deferredState);
        SafeRelease(pending.deferredSource);
    }

    if (!SubmitRing(_gameRing, slot, _gameQueue))
        return false;

    const UINT64 value = ++_frameValue;
    ID3D12CommandQueue* readyQueue = _gameQueue;

    // Copy queue mode: the copy queue waits for the game's queue, then moves the frame across PCIe
    if (_useCopyQueue)
    {
        _gameQueue->Signal(_staged, value);

        auto copyList = BeginRing(_copyRing, slot);
        if (copyList == nullptr)
            return false;

        copyList->CopyBufferRegion(color.shared, 0, color.staging, 0, color.bytes);

        for (auto& pending : _pending)
        {
            auto& surface = _inputs[pending.slot][pending.type];
            copyList->CopyBufferRegion(surface.shared, 0, surface.staging, 0, surface.bytes);
        }

        _copyQueue->Wait(_staged, value);

        if (!SubmitRing(_copyRing, slot, _copyQueue))
            return false;

        readyQueue = _copyQueue;
    }

    readyQueue->Signal(_ready, value);

    // Second GPU: waits for the frame on the GPU, then copies it into its own resources
    _secondQueue->Wait(_readyOnSecond, value);

    auto secondList = BeginRing(_secondRing, slot);
    if (secondList == nullptr)
        return false;

    Barrier(secondList, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    RecordCopyIn(secondList, color, target);
    Barrier(secondList, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);

    UINT64 bytes = color.bytes;
    for (auto& pending : _pending)
    {
        auto& surface = _inputs[pending.slot][pending.type];
        Barrier(secondList, surface.texture, surface.textureState, D3D12_RESOURCE_STATE_COPY_DEST);
        RecordCopyIn(secondList, surface, surface.texture);
        Barrier(secondList, surface.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        surface.textureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bytes += surface.bytes;
    }

    if (!SubmitRing(_secondRing, slot, _secondQueue))
        return false;

    _secondQueue->Signal(_consumed, value);

    _colorConsumed[slot] = value;
    for (auto& pending : _pending)
        _inputConsumed[pending.slot][pending.type] = value;

    _pending.clear();
    _bytesPerFrame = bytes;
    _framesTransferred++;
    return true;
}

void DualGpu::WaitIdle()
{
    std::lock_guard<std::mutex> lock(_mutex);
    WaitIdleLocked();
}

void DualGpu::WaitIdleLocked()
{
    auto drain = [](ID3D12CommandQueue* queue, CommandRing& ring, const char* what)
    {
        if (queue == nullptr || ring.fence == nullptr)
            return;

        queue->Signal(ring.fence, ++ring.lastValue);
        WaitFence(ring.fence, ring.lastValue, ring.event, 5000, what);
    };

    drain(_gameQueue, _gameRing, "the game GPU");
    drain(_copyQueue, _copyRing, "the game GPU's copy queue");
    drain(_secondQueue, _secondRing, "the second GPU");
}

void DualGpu::ReleaseSurfaces()
{
    std::lock_guard<std::mutex> lock(_mutex);
    WaitIdleLocked();
    ReleaseSurfacesLocked();
}

void DualGpu::ReleaseSurfacesLocked()
{
    for (auto& pending : _pending)
        SafeRelease(pending.deferredSource);

    _pending.clear();

    for (UINT s = 0; s < kSlots; s++)
    {
        ReleaseSurface(_color[s]);
        for (auto& input : _inputs[s])
            ReleaseSurface(input);
    }
}
