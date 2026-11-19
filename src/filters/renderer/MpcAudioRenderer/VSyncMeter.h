/*
 * VSyncMeter — MPC-BE Atmos Edition
 *
 * Measures the real display refresh rate from vertical blank events
 * (D3DKMTWaitForVerticalBlankEvent), to within a few ppm after one or two
 * minutes.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <windows.h>
#include <atomic>
#include <deque>
#include <mutex>

class CVSyncMeter
{
public:
	CVSyncMeter();
	~CVSyncMeter();

	void Start();  // measures the primary display
	void Stop();

	// Measured rate (Hz) and measurement span (s). false if no measurement yet.
	bool GetRefreshRate(double& hz, double& seconds);

private:
	static DWORD WINAPI ThreadProc(LPVOID p) { return static_cast<CVSyncMeter*>(p)->Run(); }
	DWORD Run();
	void  AddVBlank(LONGLONG qpc);
	void  Reset();

	HANDLE            m_hThread = nullptr;
	std::atomic<bool> m_bStop{false};

	std::mutex m_mutex;
	struct Sample { double index; double t; };
	std::deque<Sample> m_samples;   // sliding window (frame index, time in s)
	double   m_lastT = 0.0;
	double   m_index = 0.0;
	double   m_period = 0.0;        // estimated period (s)
	int      m_outliers = 0;
	double   m_qpcFreq = 0.0;
	double   m_fitPeriod = 0.0;     // regression result
	double   m_fitSeconds = 0.0;
	unsigned m_fitCounter = 0;
};
