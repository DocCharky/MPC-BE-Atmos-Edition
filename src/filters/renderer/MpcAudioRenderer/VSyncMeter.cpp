/*
 * VSyncMeter — MPC-BE Atmos Edition (voir VSyncMeter.h)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "stdafx.h"
#include "VSyncMeter.h"
#include <cmath>

namespace
{
	// Structures D3DKMT (d3dkmthk.h), déclarées ici pour ne pas dépendre du WDK.
	typedef UINT D3DKMT_HANDLE;
	struct KMT_OPENADAPTERFROMHDC { HDC hDc; D3DKMT_HANDLE hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
	struct KMT_WAITFORVERTICALBLANKEVENT { D3DKMT_HANDLE hAdapter; D3DKMT_HANDLE hDevice; UINT VidPnSourceId; };
	struct KMT_CLOSEADAPTER { D3DKMT_HANDLE hAdapter; };
	typedef LONG (APIENTRY *PFN_OpenAdapterFromHdc)(KMT_OPENADAPTERFROMHDC*);
	typedef LONG (APIENTRY *PFN_WaitForVerticalBlankEvent)(const KMT_WAITFORVERTICALBLANKEVENT*);
	typedef LONG (APIENTRY *PFN_CloseAdapter)(const KMT_CLOSEADAPTER*);

	constexpr double WINDOW_SECONDS = 120.0; // fenêtre de mesure glissante
}

CVSyncMeter::CVSyncMeter()
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	m_qpcFreq = (double)f.QuadPart;
}

CVSyncMeter::~CVSyncMeter()
{
	Stop();
}

void CVSyncMeter::Start()
{
	if (m_hThread) {
		return;
	}
	m_bStop = false;
	Reset();
	m_hThread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
}

void CVSyncMeter::Stop()
{
	if (m_hThread) {
		m_bStop = true;
		WaitForSingleObject(m_hThread, 1000);
		CloseHandle(m_hThread);
		m_hThread = nullptr;
	}
}

void CVSyncMeter::Reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_samples.clear();
	m_lastT = 0.0;
	m_index = 0.0;
	m_period = 0.0;
	m_outliers = 0;
	m_fitPeriod = 0.0;
	m_fitSeconds = 0.0;
}

bool CVSyncMeter::GetRefreshRate(double& hz, double& seconds)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_fitPeriod <= 0.0) {
		return false;
	}
	hz = 1.0 / m_fitPeriod;
	seconds = m_fitSeconds;
	return true;
}

void CVSyncMeter::AddVBlank(LONGLONG qpc)
{
	const double t = qpc / m_qpcFreq;
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_lastT == 0.0) {
		m_lastT = t;
		m_samples.push_back({ 0.0, t });
		return;
	}
	const double dt = t - m_lastT;
	m_lastT = t;
	if (dt <= 0.0) {
		return;
	}

	if (m_period == 0.0) {
		// Première estimation : on attend quelques intervalles cohérents.
		m_period = dt;
		m_index += 1.0;
		m_samples.push_back({ m_index, t });
		return;
	}

	const double n = std::floor(dt / m_period + 0.5);
	const double err = std::fabs(dt / (n > 0 ? n : 1) - m_period) / m_period;
	if (n < 1 || n > 10 || err > 0.15) {
		// Intervalle aberrant ; s'ils se répètent, la fréquence a changé (changement de mode).
		if (++m_outliers > 10) {
			m_samples.clear();
			m_index = 0.0;
			m_period = dt;
			m_outliers = 0;
			m_fitPeriod = 0.0;
			m_fitSeconds = 0.0;
			m_samples.push_back({ 0.0, t });
		}
		return;
	}
	m_outliers = 0;
	m_index += n;
	m_samples.push_back({ m_index, t });
	while (!m_samples.empty() && t - m_samples.front().t > WINDOW_SECONDS) {
		m_samples.pop_front();
	}

	// Régression linéaire t = a + période * index sur la fenêtre (environ deux fois par seconde).
	if (m_samples.size() >= 30 && (++m_fitCounter % 30) == 0) {
		const double i0 = m_samples.front().index, t0 = m_samples.front().t;
		double si = 0, st = 0, sii = 0, sit = 0;
		const double N = (double)m_samples.size();
		for (const auto& s : m_samples) {
			const double i = s.index - i0, tt = s.t - t0;
			si += i; st += tt; sii += i * i; sit += i * tt;
		}
		const double den = N * sii - si * si;
		if (den > 0) {
			const double p = (N * sit - si * st) / den;
			if (p > 0) {
				m_fitPeriod = p;
				m_period = p;
				m_fitSeconds = m_samples.back().t - t0;
			}
		}
	}
}

DWORD CVSyncMeter::Run()
{
	HMODULE hGdi = GetModuleHandleW(L"gdi32.dll");
	auto pOpen  = hGdi ? (PFN_OpenAdapterFromHdc)GetProcAddress(hGdi, "D3DKMTOpenAdapterFromHdc") : nullptr;
	auto pWait  = hGdi ? (PFN_WaitForVerticalBlankEvent)GetProcAddress(hGdi, "D3DKMTWaitForVerticalBlankEvent") : nullptr;
	auto pClose = hGdi ? (PFN_CloseAdapter)GetProcAddress(hGdi, "D3DKMTCloseAdapter") : nullptr;
	if (!pOpen || !pWait || !pClose) {
		return 1;
	}

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	while (!m_bStop) {
		// Écran principal (le téléviseur sur un HTPC).
		MONITORINFOEXW mi = {};
		mi.cbSize = sizeof(mi);
		HMONITOR hMon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
		if (!GetMonitorInfoW(hMon, &mi)) {
			Sleep(500);
			continue;
		}
		HDC hdc = CreateDCW(nullptr, mi.szDevice, nullptr, nullptr);
		if (!hdc) {
			Sleep(500);
			continue;
		}
		KMT_OPENADAPTERFROMHDC oa = {};
		oa.hDc = hdc;
		const LONG st = pOpen(&oa);
		DeleteDC(hdc);
		if (st != 0) {
			Sleep(500);
			continue;
		}

		KMT_WAITFORVERTICALBLANKEVENT we = {};
		we.hAdapter = oa.hAdapter;
		we.VidPnSourceId = oa.VidPnSourceId;
		while (!m_bStop) {
			if (pWait(&we) != 0) {
				break; // adaptateur perdu (changement de mode) : on rouvre
			}
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			AddVBlank(now.QuadPart);
		}
		KMT_CLOSEADAPTER ca = { oa.hAdapter };
		pClose(&ca);
		Reset();
	}
	return 0;
}
