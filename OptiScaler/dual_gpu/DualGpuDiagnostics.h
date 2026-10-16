#pragma once
#include "SysUtils.h"

#include <fstream>
#include <mutex>
#include <vector>

// CrossFlames benchmark diagnostics (crossflames/diagnostics branch only, not proposed upstream).
//
// When the [DualGpu] section is present in OptiScaler.ini, every present is logged next to the game:
// - CrossFlames_frames.csv: "game" rows (frames the game presents) and "output" rows (presents that reach the screen,
//   real and generated), with the CPU time of the handoff and of the presenting swapchain's Present.
// - CrossFlames_gpu.csv: GPU timestamps of the dual GPU handoff per frame.
// A 2-second summary also goes to the log.
class DualGpuDiagnostics
{
  public:
    static DualGpuDiagnostics& Instance();
    static bool Enabled();
    static double NowMs();

    void RecordGamePresent(double transferMs, double presentMs);
    void RecordOutputPresent();
    void RecordGpuTimings(double gameColorMs, double gameInputsMs, double copyQueueMs, double secondCopyMs,
                          double megabytes);

  private:
    void WriteFrameRow(const char* kind, double now, double interval, double transferMs, double presentMs);
    void MaybeSummarise(double now);

    std::mutex _mutex;
    std::ofstream _frames;
    std::ofstream _gpu;
    bool _framesOpened = false;
    bool _gpuOpened = false;

    double _windowStart = 0.0;
    double _lastGame = 0.0;
    double _lastOutput = 0.0;
    std::vector<double> _gameIntervals;
    std::vector<double> _outputIntervals;
    double _transferSum = 0.0;
    double _presentSum = 0.0;

    UINT _gpuSamples = 0;
    double _gameQueueSum = 0.0;
    double _copyQueueSum = 0.0;
    double _secondCopySum = 0.0;
};
