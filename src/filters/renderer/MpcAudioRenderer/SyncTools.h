/*
 * SyncTools - MPC-BE Atmos Edition
 *
 * Shared pieces of the ReClock-style display sync:
 *  - the windowed-sinc interpolation kernel;
 *  - CClockRateMeter: measures an audio device clock against the system clock;
 *  - CVarResampler: streaming variable-speed resampler (interleaved float).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <windows.h>
#include <audioclient.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// Kaiser-windowed sinc kernel, 48 taps, 2048 phases (~110 dB).
namespace SincKernel
{
	constexpr int HALF = 24;
	constexpr int PHASES = 2048;
	// Taps for a phase in [0, PHASES]; 2 * HALF values. Tap k weighs input sample
	// (ip - HALF + 1 + k), where ip is the integer part of the read position.
	const float* Taps(unsigned phase);
}

// Rate of an audio device clock relative to the system clock (QPC), by linear
// regression over a sliding 120 s window. Thread-safe.
class CClockRateMeter
{
public:
	void Reset();
	// Call often (e.g. from the render thread): samples every 100 ms, fits every second.
	void Poll(IAudioClock* pClock);
	// Audio seconds per system second (about 1 +/- a few ppm) and the span measured.
	bool GetRate(double& rate, double& seconds);

private:
	struct Sample { double qpc; double audio; };
	std::mutex         m_mutex;
	std::deque<Sample> m_samples;
	UINT64             m_freq = 0;
	DWORD              m_dwLastSample = 0;
	DWORD              m_dwLastFit = 0;
	double             m_rate = 0.0;
	double             m_seconds = 0.0;
	void               Fit();
};

// Streaming resampler with a speed factor close to 1 (interleaved float).
// Output sample i is read at input position pos_i, with pos_{i+1} = pos_i + speed.
// At speed 1 the read position stays integer and samples are copied exactly.
class CVarResampler
{
public:
	void Reset(unsigned channels);
	unsigned Channels() const { return m_channels; }
	// Index (in input samples since Reset) of the next input sample expected.
	uint64_t InputEnd() const { return m_end; }
	// Appends n input frames and produces as many output frames as the kernel allows.
	// firstPos receives the input position of the first produced frame.
	size_t Process(const float* in, size_t n, double speed, std::vector<float>& out, double& firstPos);

private:
	float Sample(unsigned c, int64_t index) const;
	unsigned           m_channels = 0;
	std::vector<float> m_buf;      // interleaved, sample m_base first
	uint64_t           m_base = 0;
	uint64_t           m_end = 0;
	double             m_pos = 0.0;
};
