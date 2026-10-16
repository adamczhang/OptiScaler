#pragma once
#include "SysUtils.h"

#include <framegen/IFGFeature.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Dual GPU mode ([DualGpu] Enabled)
//
// The game keeps rendering on its own GPU (the "game GPU"). FSR frame generation and presentation run on a second GPU,
// normally the one driving the monitor. Every frame the game's back buffer and the frame generation inputs (depth and
// motion vectors) are copied into heaps in system memory that both GPUs can access, ordered by shared fences, so no
// CPU thread waits for the other GPU.
//
// On the game GPU the copy first goes into the GPU's own memory on the game's queue, which is cheap. Its copy queue
// then moves the data across PCIe while the next frame renders ([DualGpu] CopyQueue). The second GPU waits for that on
// the GPU and copies everything into its own resources before frame generation runs there.
//
// Locking: StageInput is called with FSRFG_Dx12's resource lock held and takes _mutex; nothing here calls back into
// frame generation, so the order is always frame generation first, then _mutex.
class DualGpu
{
  public:
    static constexpr UINT kSlots = BUFFER_COUNT;

    static DualGpu& Instance();

    // Dual GPU mode is configured on, with FSR frame generation as the output
    static bool Enabled();

    // The second GPU is set up and a dual GPU swapchain is live
    bool Active() const { return _active; }
    void SetActive(bool active) { _active = active; }

    // Creates the second GPU's device and queue and the objects shared with the game GPU. Returns false, with the
    // reason logged, when dual GPU mode can't run, so the caller falls back to single GPU frame generation.
    bool Init(ID3D12Device* gameDevice, ID3D12CommandQueue* gameQueue, HWND hwnd);

    ID3D12Device* GameDevice() const { return _gameDevice; }
    ID3D12CommandQueue* GameQueue() const { return _gameQueue; }
    ID3D12Device* SecondDevice() const { return _secondDevice; }
    ID3D12CommandQueue* SecondQueue() const { return _secondQueue; }

    // True when obj (a device, or anything created from one) belongs to the second GPU
    bool IsSecondGpu(IUnknown* obj) const;
    static bool IsSecondGpuObject(IUnknown* obj) { return Instance().IsSecondGpu(obj); }

    // A frame generation input on the game GPU (depth, motion vectors, HUD-less colour, UI, distortion). Records its
    // copy on cmdList, or at present time when cmdList is null. Returns the second GPU texture the input will be in, in
    // NON_PIXEL_SHADER_RESOURCE state, once this frame's TransferFrame has run; frame generation reads it there on the
    // second GPU's queue.
    ID3D12Resource* StageInput(FG_ResourceType type, int fIndex, ID3D12GraphicsCommandList* cmdList,
                               ID3D12Resource* source, D3D12_RESOURCE_STATES state);

    // At the game's present: copies the back buffer and this frame's inputs to the second GPU. target is the
    // presenting swapchain's current back buffer on the second GPU.
    bool TransferFrame(ID3D12Resource* backBuffer, ID3D12Resource* target);

    // Waits until both GPUs finish their submitted work (resize, release)
    void WaitIdle();

    // Frees the shared surfaces after waiting for both GPUs; they are recreated on the next frame
    void ReleaseSurfaces();

    // Menu status
    const std::string& SecondGpuName() const { return _secondName; }
    bool UsesCopyQueue() const { return _useCopyQueue; }
    double MegabytesPerFrame() const { return (double) _bytesPerFrame / 1.0e6; }
    UINT64 FramesTransferred() const { return _framesTransferred; }

    // The game's own frame rate, measured as its frames reach the second GPU, 0 until known. Generated frames don't
    // count, so the FPS overlay can show both rates without deriving one from the other.
    double GameFrameRate() const { return _gameFrameRate; }

  private:
    // One surface both GPUs can reach: a heap created on the game GPU and opened on the second GPU, a placed buffer
    // in it on each side, and, for inputs, the texture the second GPU copies into
    struct SharedSurface
    {
        ID3D12Heap* heap = nullptr;
        ID3D12Heap* heapOnSecond = nullptr;
        ID3D12Resource* shared = nullptr;
        ID3D12Resource* sharedOnSecond = nullptr;
        ID3D12Resource* staging = nullptr; // Copy queue mode: game GPU memory the game's queue copies into first
        UINT64 size = 0;
        UINT64 bytes = 0;

        D3D12_RESOURCE_DESC sourceDesc {};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};

        ID3D12Resource* texture = nullptr;
        D3D12_RESOURCE_STATES textureState = D3D12_RESOURCE_STATE_COMMON;
    };

    struct PendingInput
    {
        FG_ResourceType type;
        int fIndex = -1;
        UINT slot = 0;

        // Set when the copy is recorded at present time
        ID3D12Resource* deferredSource = nullptr;
        D3D12_RESOURCE_STATES deferredState = D3D12_RESOURCE_STATE_COMMON;
    };

    // Per-slot command lists on one queue, reused once the queue has finished with them
    struct CommandRing
    {
        ID3D12CommandAllocator* allocator[kSlots] {};
        ID3D12GraphicsCommandList* list[kSlots] {};
        UINT64 fenceValue[kSlots] {};
        ID3D12Fence* fence = nullptr;
        UINT64 lastValue = 0;
        HANDLE event = nullptr;
    };

    static IDXGIAdapter1* PickAdapter(LUID gameLuid, HWND hwnd);
    static DXGI_FORMAT TextureFormat(DXGI_FORMAT footprintFormat);

    bool CreateSecondGpu(ID3D12Device* gameDevice, HWND hwnd);
    bool CreateGameSide();
    void ReleaseGameSide();

    bool CreateRing(ID3D12Device* device, CommandRing& ring, const wchar_t* name, D3D12_COMMAND_LIST_TYPE type);
    void ReleaseRing(CommandRing& ring);
    ID3D12GraphicsCommandList* BeginRing(CommandRing& ring, UINT slot);
    bool SubmitRing(CommandRing& ring, UINT slot, ID3D12CommandQueue* queue);

    bool EnsureSurface(SharedSurface& surface, ID3D12Resource* source, bool needsTexture);
    void ReleaseSurface(SharedSurface& surface);
    void ReleaseSurfacesLocked();
    void RecordCopyOut(ID3D12GraphicsCommandList* list, SharedSurface& surface, ID3D12Resource* source,
                       D3D12_RESOURCE_STATES state);
    void RecordCopyIn(ID3D12GraphicsCommandList* list, SharedSurface& surface, ID3D12Resource* target);

    // CPU wait until the second GPU has finished with a slot, so the game GPU can overwrite it
    void WaitForSlot(UINT64 consumedValue);
    void WaitIdleLocked();
    bool DevicesHealthy();

    bool _active = false;
    std::mutex _mutex;

    ID3D12Device* _gameDevice = nullptr;
    ID3D12CommandQueue* _gameQueue = nullptr;
    ID3D12Device* _secondDevice = nullptr;
    ID3D12CommandQueue* _secondQueue = nullptr;
    LUID _secondLuid {};
    std::string _secondName;

    // The game GPU signals ready (frame value) once a frame is in shared memory; the second GPU signals consumed once
    // it has copied it out
    ID3D12Fence* _ready = nullptr;
    ID3D12Fence* _readyOnSecond = nullptr;
    ID3D12Fence* _consumed = nullptr;
    ID3D12Fence* _consumedOnGame = nullptr;
    HANDLE _consumedEvent = nullptr;
    UINT64 _frameValue = 0;
    UINT64 _colorConsumed[kSlots] {};
    UINT64 _inputConsumed[kSlots][FG_ResourceType::ResourceTypeCOUNT] {};

    CommandRing _gameRing {};
    CommandRing _secondRing {};

    bool _useCopyQueue = false;
    ID3D12CommandQueue* _copyQueue = nullptr;
    CommandRing _copyRing {};
    ID3D12Fence* _staged = nullptr;

    SharedSurface _color[kSlots] {};
    SharedSurface _inputs[kSlots][FG_ResourceType::ResourceTypeCOUNT] {};

    std::vector<PendingInput> _pending;
    UINT _presentSlot = 0;
    bool _loggedUnsupported[FG_ResourceType::ResourceTypeCOUNT] {};
    int _removedLogs = 0;

    UINT64 _bytesPerFrame = 0;
    UINT64 _framesTransferred = 0;

    // Game frame rate over the last kRateFrames transferred frames, about the span the FPS overlay averages
    static constexpr UINT kRateFrames = 50;
    double _rateIntervals[kRateFrames] {};
    double _rateSum = 0.0;
    UINT _rateIndex = 0;
    UINT _rateCount = 0;
    double _lastTransfer = 0.0;
    std::atomic<double> _gameFrameRate = 0.0;

    void UpdateGameFrameRate();
};
