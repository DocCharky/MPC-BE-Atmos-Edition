/*
 * SyncTools - MPC-BE Atmos Edition (see SyncTools.h)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "stdafx.h"
#include "SyncTools.h"
#include <cmath>

namespace
{
	double BesselI0(double x)
	{
		double sum = 1.0, term = 1.0;
		for (int k = 1; k < 50; k++) {
			term *= (x / (2.0 * k)) * (x / (2.0 * k));
			sum += term;
			if (term < 1e-12 * sum) {
				break;
			}
		}
		return sum;
	}

	struct Kernel {
		float taps[SincKernel::PHASES + 1][2 * SincKernel::HALF];
		Kernel() {
			const int H = SincKernel::HALF;
			const double beta = 10.0;
			const double i0b = BesselI0(beta);
			for (int ph = 0; ph <= SincKernel::PHASES; ph++) {
				const double frac = (double)ph / SincKernel::PHASES;
				for (int k = 0; k < 2 * H; k++) {
					// distance between sample k and the fractional position
					const double t = (double)(k - H + 1) - frac;
					const double x = 3.14159265358979323846 * t;
					const double sinc = (std::fabs(t) < 1e-12) ? 1.0 : std::sin(x) / x;
					const double r = t / H;
					const double w = (std::fabs(r) >= 1.0) ? 0.0 : BesselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
					taps[ph][k] = (float)(sinc * w);
				}
			}
		}
	};

	const Kernel& GetKernel()
	{
		static const Kernel k;
		return k;
	}
}

const float* SincKernel::Taps(unsigned phase)
{
	return GetKernel().taps[phase];
}

// ---- CClockRateMeter ----

void CClockRateMeter::Reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_samples.clear();
	m_freq = 0;
	m_rate = m_seconds = 0.0;
}

void CClockRateMeter::Poll(IAudioClock* pClock)
{
	if (!pClock) {
		return;
	}
	const DWORD now = GetTickCount();
	if (now - m_dwLastSample >= 100) {
		m_dwLastSample = now;
		UINT64 freq = 0, pos = 0, qpcPos = 0;
		if (SUCCEEDED(pClock->GetFrequency(&freq)) && freq
				&& SUCCEEDED(pClock->GetPosition(&pos, &qpcPos)) && qpcPos) {
			const double qpc = qpcPos / 1e7; // 100 ns units
			const double audio = (double)pos / (double)freq;
			std::lock_guard<std::mutex> lock(m_mutex);
			if (freq != m_freq) {
				m_samples.clear();
				m_freq = freq;
			}
			if (!m_samples.empty()) {
				const Sample& last = m_samples.back();
				// A glitch, a pause or a stream restart breaks the linear relation: start over.
				if (qpc <= last.qpc || std::fabs((audio - last.audio) - (qpc - last.qpc)) > 0.05) {
					m_samples.clear();
				}
			}
			m_samples.push_back({ qpc, audio });
			while (!m_samples.empty() && qpc - m_samples.front().qpc > 120.0) {
				m_samples.pop_front();
			}
		}
	}
	if (now - m_dwLastFit >= 1000) {
		m_dwLastFit = now;
		Fit();
	}
}

void CClockRateMeter::Fit()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_samples.size() < 50) {
		m_rate = m_seconds = 0.0;
		return;
	}
	const double x0 = m_samples.front().qpc, y0 = m_samples.front().audio;
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	const double n = (double)m_samples.size();
	for (const auto& s : m_samples) {
		const double x = s.qpc - x0, y = s.audio - y0;
		sx += x; sy += y; sxx += x * x; sxy += x * y;
	}
	const double den = n * sxx - sx * sx;
	if (den > 0) {
		m_rate = (n * sxy - sx * sy) / den;
		m_seconds = m_samples.back().qpc - x0;
	}
}

bool CClockRateMeter::GetRate(double& rate, double& seconds)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_rate <= 0.0) {
		return false;
	}
	rate = m_rate;
	seconds = m_seconds;
	return true;
}

// ---- CVarResampler ----

void CVarResampler::Reset(unsigned channels)
{
	m_channels = channels;
	m_buf.clear();
	m_base = m_end = 0;
	m_pos = 0.0;
}

float CVarResampler::Sample(unsigned c, int64_t index) const
{
	if (index < (int64_t)m_base || index >= (int64_t)m_end) {
		return 0.0f;
	}
	return m_buf[(size_t)(index - (int64_t)m_base) * m_channels + c];
}

size_t CVarResampler::Process(const float* in, size_t n, double speed, std::vector<float>& out, double& firstPos)
{
	const unsigned C = m_channels;
	const int H = SincKernel::HALF;
	m_buf.insert(m_buf.end(), in, in + n * C);
	m_end += n;

	firstPos = m_pos;
	out.clear();
	size_t produced = 0;
	for (;;) {
		const int64_t ip = (int64_t)std::floor(m_pos);
		if (ip + H >= (int64_t)m_end) {
			break; // needs more input
		}
		const size_t o = out.size();
		out.resize(o + C);
		const double frac = m_pos - (double)ip;
		const unsigned phase = (unsigned)(frac * SincKernel::PHASES);
		if (frac == 0.0) {
			for (unsigned c = 0; c < C; c++) {
				out[o + c] = Sample(c, ip);
			}
		} else {
			const float mu = (float)(frac * SincKernel::PHASES - phase);
			const float* h0 = SincKernel::Taps(phase);
			const float* h1 = SincKernel::Taps(phase + 1);
			for (unsigned c = 0; c < C; c++) {
				float acc = 0.0f;
				for (int k = 0; k < 2 * H; k++) {
					acc += (h0[k] + mu * (h1[k] - h0[k])) * Sample(c, ip - H + 1 + k);
				}
				out[o + c] = acc;
			}
		}
		m_pos += speed;
		produced++;
	}

	// Drop consumed input (keeping the kernel context).
	const int64_t keep = (int64_t)std::floor(m_pos) - H - 1;
	if (keep > (int64_t)m_base + 4096) {
		const size_t drop = (size_t)(keep - (int64_t)m_base);
		m_buf.erase(m_buf.begin(), m_buf.begin() + drop * C);
		m_base += drop;
	}
	return produced;
}
