/*
 * VSyncMeter — MPC-BE Atmos Edition
 *
 * Mesure la fréquence de rafraîchissement réelle de l'écran à partir des
 * interruptions de retour de trame (D3DKMTWaitForVerticalBlankEvent), avec une
 * précision de quelques millionièmes après une à deux minutes de mesure.
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

	void Start();  // mesure sur l'écran principal
	void Stop();

	// Fréquence mesurée (Hz) et durée de mesure (s). false si pas encore de mesure.
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
	std::deque<Sample> m_samples;   // fenêtre glissante (index de trame, temps en s)
	double   m_lastT = 0.0;
	double   m_index = 0.0;
	double   m_period = 0.0;        // période estimée (s)
	int      m_outliers = 0;
	double   m_qpcFreq = 0.0;
	double   m_fitPeriod = 0.0;     // résultat de la régression
	double   m_fitSeconds = 0.0;
	unsigned m_fitCounter = 0;
};
