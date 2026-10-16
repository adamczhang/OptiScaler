#include "pch.h"
#include "DualGpuDiagnostics.h"

#include <Config.h>

#include <algorithm>

DualGpuDiagnostics& DualGpuDiagnostics::Instance()
{
    static DualGpuDiagnostics instance;
    return instance;
}

bool DualGpuDiagnostics::Enabled() { return Config::Instance()->DualGpuEnabled.has_value(); }

double DualGpuDiagnostics::NowMs()
{
    static LARGE_INTEGER frequency = []
    {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        return f;
    }();

    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    return (double) now.QuadPart * 1000.0 / (double) frequency.QuadPart;
}

void DualGpuDiagnostics::WriteFrameRow(const char* kind, double now, double interval, double transferMs,
                                       double presentMs)
{
    if (!_framesOpened)
    {
        _framesOpened = true;
        _frames.open("CrossFlames_frames.csv", std::ios::out | std::ios::trunc);
        if (_frames.is_open())
            _frames << "kind,time_ms,interval_ms,extra_ms,present_ms\n";
    }

    if (_frames.is_open())
    {
        _frames << kind << ',' << std::format("{:.4f},{:.4f},{:.4f},{:.4f}", now, interval, transferMs, presentMs)
                << '\n';
    }
}

void DualGpuDiagnostics::RecordGamePresent(double transferMs, double presentMs)
{
    if (!Enabled())
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    const double now = NowMs();
    const double interval = _lastGame > 0.0 ? now - _lastGame : 0.0;
    _lastGame = now;

    if (interval > 0.0)
        _gameIntervals.push_back(interval);

    _transferSum += transferMs;
    _presentSum += presentMs;

    WriteFrameRow("game", now, interval, transferMs, presentMs);
    MaybeSummarise(now);
}

void DualGpuDiagnostics::RecordOutputPresent()
{
    if (!Enabled())
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    const double now = NowMs();
    const double interval = _lastOutput > 0.0 ? now - _lastOutput : 0.0;
    _lastOutput = now;

    if (interval > 0.0)
        _outputIntervals.push_back(interval);

    WriteFrameRow("output", now, interval, 0.0, 0.0);
    MaybeSummarise(now);
}

void DualGpuDiagnostics::RecordGpuTimings(double gameColorMs, double gameInputsMs, double copyQueueMs,
                                          double secondCopyMs, double megabytes)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_gpuOpened)
    {
        _gpuOpened = true;
        _gpu.open("CrossFlames_gpu.csv", std::ios::out | std::ios::trunc);
        if (_gpu.is_open())
            _gpu << "time_ms,a_graphics_ms,a_color_ms,a_inputs_ms,a_copy_queue_ms,b_copy_in_ms,mb_per_frame\n";
    }

    if (_gpu.is_open())
    {
        _gpu << std::format("{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.3f}\n", NowMs(), gameColorMs + gameInputsMs,
                            gameColorMs, gameInputsMs, copyQueueMs, secondCopyMs, megabytes);
    }

    _gpuSamples++;
    _gameQueueSum += gameColorMs + gameInputsMs;
    _copyQueueSum += copyQueueMs > 0.0 ? copyQueueMs : 0.0;
    _secondCopySum += secondCopyMs > 0.0 ? secondCopyMs : 0.0;
}

void DualGpuDiagnostics::MaybeSummarise(double now)
{
    if (_windowStart == 0.0)
        _windowStart = now;

    const double elapsed = now - _windowStart;
    if (elapsed < 2000.0)
        return;

    auto percentile = [](std::vector<double>& values, double p)
    {
        if (values.empty())
            return 0.0;

        std::sort(values.begin(), values.end());
        auto index = (size_t) std::min<double>((double) values.size() - 1, p * (double) (values.size() - 1) + 0.5);
        return values[index];
    };

    const auto games = _gameIntervals.size();
    const double gpuSamples = _gpuSamples > 0 ? (double) _gpuSamples : 1.0;

    LOG_INFO("DualGpu stats: game {:.1f} fps (p99 {:.2f} ms), shown {:.1f} fps (p50 {:.2f}, p99 {:.2f} ms), "
             "handoff CPU {:.3f} ms, present call {:.3f} ms, GPU: game queue {:.3f} ms, copy queue {:.3f} ms, "
             "second GPU copy-in {:.3f} ms",
             games * 1000.0 / elapsed, percentile(_gameIntervals, 0.99), _outputIntervals.size() * 1000.0 / elapsed,
             percentile(_outputIntervals, 0.50), percentile(_outputIntervals, 0.99),
             games > 0 ? _transferSum / games : 0.0, games > 0 ? _presentSum / games : 0.0, _gameQueueSum / gpuSamples,
             _copyQueueSum / gpuSamples, _secondCopySum / gpuSamples);

    if (_frames.is_open())
        _frames.flush();

    if (_gpu.is_open())
        _gpu.flush();

    _gameIntervals.clear();
    _outputIntervals.clear();
    _transferSum = 0.0;
    _presentSum = 0.0;
    _gpuSamples = 0;
    _gameQueueSum = 0.0;
    _copyQueueSum = 0.0;
    _secondCopySum = 0.0;
    _windowStart = now;
}
