/*
 * AtmosOutput — MPC-BE Atmos Edition (voir AtmosOutput.h)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "stdafx.h"
#include "AtmosOutput.h"
#include <algorithm>
#include <cstring>

// ---- thdatmos.dll ----
namespace
{
	typedef void* (*PFN_thd_create)();
	typedef void  (*PFN_thd_reset)(void*);
	typedef int   (*PFN_thd_push_iec61937)(void*, const uint8_t*, size_t);
	typedef int   (*PFN_thd_pull)(void*, CAtmosOutput::Frame*);
	typedef void  (*PFN_thd_close)(void*);

	static int s_moduleAnchor = 0;

	struct ThdLib {
		HMODULE hLib = nullptr;
		bool tried = false;
		PFN_thd_create        create = nullptr;
		PFN_thd_reset         reset = nullptr;
		PFN_thd_push_iec61937 push = nullptr;
		PFN_thd_pull          pull = nullptr;
		PFN_thd_close         close = nullptr;

		bool Load() {
			if (tried) {
				return hLib != nullptr;
			}
			tried = true;
			wchar_t path[MAX_PATH] = {};
			// thdatmos.dll est cherchee a cote du module qui contient ce code (.ax ou .exe).
			HMODULE hSelf = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							   (LPCWSTR)&s_moduleAnchor, &hSelf);
			GetModuleFileNameW(hSelf, path, MAX_PATH);
			if (wchar_t* p = wcsrchr(path, L'\\')) {
				wcscpy_s(p + 1, MAX_PATH - (p + 1 - path), L"thdatmos.dll");
			}
			hLib = LoadLibraryW(path);
			if (!hLib) {
				return false;
			}
			create = (PFN_thd_create)GetProcAddress(hLib, "thd_create");
			reset  = (PFN_thd_reset)GetProcAddress(hLib, "thd_reset");
			push   = (PFN_thd_push_iec61937)GetProcAddress(hLib, "thd_push_iec61937");
			pull   = (PFN_thd_pull)GetProcAddress(hLib, "thd_pull");
			close  = (PFN_thd_close)GetProcAddress(hLib, "thd_close");
			if (!create || !reset || !push || !pull || !close) {
				FreeLibrary(hLib);
				hLib = nullptr;
			}
			return hLib != nullptr;
		}
	};
	ThdLib g_thd;

	constexpr uint32_t KIND_LFE = 1;
	constexpr REFERENCE_TIME UNITS_PER_SEC = 10000000;
	constexpr size_t MAX_QUEUE_SAMPLES = 48000;   // ~1 s d'avance
	constexpr uint32_t SAMPLE_RATE = 48000;

	inline REFERENCE_TIME SamplesToTime(uint64_t n) { return (REFERENCE_TIME)(n * UNITS_PER_SEC / SAMPLE_RATE); }

	template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}

CAtmosOutput::CAtmosOutput()
{
	m_hStopEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	m_hSpaceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

CAtmosOutput::~CAtmosOutput()
{
	Close();
	CloseHandle(m_hStopEvent);
	CloseHandle(m_hSpaceEvent);
}

bool CAtmosOutput::LibraryAvailable()
{
	return g_thd.Load();
}

HRESULT CAtmosOutput::Open(IMMDevice* pDevice)
{
	if (IsOpen()) {
		return S_OK;
	}
	if (!pDevice || !LibraryAvailable()) {
		return E_FAIL;
	}

	HRESULT hr = pDevice->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, (void**)&m_pClient);
	if (FAILED(hr)) {
		return hr;
	}
	hr = m_pClient->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr);
	if (FAILED(hr)) {
		Close();
		return hr;
	}

	UINT32 maxDyn = 0;
	m_pClient->GetMaxDynamicObjectCount(&maxDyn);
	m_nMaxDynamic = std::min<UINT32>(maxDyn, 16);
	if (m_nMaxDynamic < 15) { // une piste Atmos de Blu-ray compte jusqu'à 15 objets + LFE
		Close();
		return E_FAIL;
	}

	WAVEFORMATEX wf = {};
	wf.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
	wf.nChannels       = 1;
	wf.nSamplesPerSec  = SAMPLE_RATE;
	wf.wBitsPerSample  = 32;
	wf.nBlockAlign     = 4;
	wf.nAvgBytesPerSec = SAMPLE_RATE * 4;
	hr = m_pClient->IsAudioObjectFormatSupported(&wf);
	if (FAILED(hr)) {
		Close();
		return hr;
	}

	m_hBufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	SpatialAudioObjectRenderStreamActivationParams p = {};
	p.ObjectFormat          = &wf;
	p.StaticObjectTypeMask  = AudioObjectType_LowFrequency;
	p.MinDynamicObjectCount = 0;
	p.MaxDynamicObjectCount = m_nMaxDynamic;
	p.Category              = AudioCategory_Movie;
	p.EventHandle           = m_hBufferEvent;
	p.NotifyObject          = nullptr;

	PROPVARIANT act;
	PropVariantInit(&act);
	act.vt             = VT_BLOB;
	act.blob.cbSize    = sizeof(p);
	act.blob.pBlobData = (BYTE*)&p;
	hr = m_pClient->ActivateSpatialAudioStream(&act, __uuidof(ISpatialAudioObjectRenderStream), (void**)&m_pStream);
	if (FAILED(hr)) {
		Close();
		return hr;
	}

	hr = m_pStream->GetService(__uuidof(IAudioClock), (void**)&m_pAudioClock);
	if (FAILED(hr)) {
		Close();
		return hr;
	}

	m_pDecoder = g_thd.create();

	ResetEvent(m_hStopEvent);
	hr = m_pStream->Start();
	if (FAILED(hr)) {
		Close();
		return hr;
	}
	m_hThread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);

	return S_OK;
}

void CAtmosOutput::Close()
{
	if (m_hThread) {
		SetEvent(m_hStopEvent);
		WaitForSingleObject(m_hThread, 2000);
		CloseHandle(m_hThread);
		m_hThread = nullptr;
	}
	if (m_pStream) {
		m_pStream->Stop();
	}
	for (auto& o : m_pObjects) {
		SafeRelease(o);
	}
	SafeRelease(m_pAudioClock);
	SafeRelease(m_pStream);
	SafeRelease(m_pClient);
	if (m_hBufferEvent) {
		CloseHandle(m_hBufferEvent);
		m_hBufferEvent = nullptr;
	}
	if (m_pDecoder) {
		g_thd.close(m_pDecoder);
		m_pDecoder = nullptr;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	m_queue.clear();
	m_nQueuedSamples = 0;
	m_pCurrent.reset();
	m_bHaveState = false;
}

HRESULT CAtmosOutput::Receive(const BYTE* pData, long size, REFERENCE_TIME rtStart, bool bDiscontinuity,
							  HANDLE hAbort1, HANDLE hAbort2)
{
	if (!m_pDecoder) {
		return E_FAIL;
	}

	// Contre-pression : on n'avance pas de plus d'environ une seconde sur la lecture.
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_nQueuedSamples < MAX_QUEUE_SAMPLES) {
				break;
			}
		}
		HANDLE h[3] = { m_hSpaceEvent, hAbort1, hAbort2 };
		const DWORD n = hAbort2 ? 3 : (hAbort1 ? 2 : 1);
		const DWORD r = WaitForMultipleObjects(n, h, FALSE, 100);
		if (r == WAIT_OBJECT_0 + 1 || r == WAIT_OBJECT_0 + 2) {
			return S_FALSE;
		}
	}

	if (!g_thd.push(m_pDecoder, pData, (size_t)size)) {
		return S_FALSE; // pas du TrueHD emballé
	}

	auto frame = std::make_unique<Frame>();
	while (g_thd.pull(m_pDecoder, frame.get()) == 1) {
		if (!frame->samples) {
			continue;
		}
		if (frame->sample_rate != SAMPLE_RATE) {
			// Le flux spatial est en 48 kHz : 96/192 kHz ramenés par moyenne (pistes non-Atmos rares).
			const uint32_t f = frame->sample_rate / SAMPLE_RATE;
			if (f < 2 || frame->sample_rate % SAMPLE_RATE) {
				continue; // 44,1 kHz et dérivés : non gérés
			}
			const uint32_t n = frame->samples / f;
			for (uint32_t s = 0; s < n; s++) {
				for (uint32_t c = 0; c < frame->channels && c < 16; c++) {
					float acc = 0.0f;
					for (uint32_t k = 0; k < f; k++) {
						acc += frame->pcm[s * f + k][c];
					}
					frame->pcm[s][c] = acc / (float)f;
				}
			}
			frame->samples = n;
			frame->sample_rate = SAMPLE_RATE;
			if (!n) {
				continue;
			}
		}
		if (m_rtNextDecoded == INVALID_TIME_VALUE || bDiscontinuity) {
			// Ancrage de l'horodatage : début de la salve en cours.
			m_rtNextDecoded = rtStart;
			m_nDecodedSinceAnchor = 0;
			bDiscontinuity = false;
		}
		const REFERENCE_TIME t = m_rtNextDecoded + SamplesToTime(m_nDecodedSinceAnchor);
		m_nDecodedSinceAnchor += frame->samples;

		std::lock_guard<std::mutex> lock(m_mutex);
		m_nQueuedSamples += frame->samples;
		m_queue.push_back({ t, std::move(frame) });
		frame = std::make_unique<Frame>();
	}

	return S_OK;
}

void CAtmosOutput::Flush()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_queue.clear();
	m_nQueuedSamples = 0;
	m_pCurrent.reset();
	m_nCurrentOffset = 0;
	m_rtNextDecoded = INVALID_TIME_VALUE;
	m_rtNextRendered = 0;
	m_rtCurrentRendered = 0;
	if (m_pDecoder) {
		g_thd.reset(m_pDecoder);
	}
	SetEvent(m_hSpaceEvent);
}

void CAtmosOutput::SetRunning(bool bRunning)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_bRunning = bRunning;
}

void CAtmosOutput::SetVolume(float volume)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_fVolume = volume;
}

REFERENCE_TIME CAtmosOutput::GetCurrentRenderedTime()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_pCurrent) {
		return m_rtCurrentStart + SamplesToTime(m_nCurrentOffset);
	}
	if (!m_queue.empty()) {
		return m_queue.front().rtStart;
	}
	return m_rtCurrentRendered;
}

size_t CAtmosOutput::QueuedSamples()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_nQueuedSamples + (m_pCurrent ? m_pCurrent->samples - m_nCurrentOffset : 0);
}

DWORD CAtmosOutput::RenderThread()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	HANDLE h[2] = { m_hStopEvent, m_hBufferEvent };
	for (;;) {
		const DWORD r = WaitForMultipleObjects(2, h, FALSE, 200);
		if (r == WAIT_OBJECT_0) {
			break;
		}
		if (r == WAIT_OBJECT_0 + 1) {
			RenderPeriod();
		}
	}
	return 0;
}

void CAtmosOutput::RenderPeriod()
{
	UINT32 availableDynamic = 0, frames = 0;
	if (FAILED(m_pStream->BeginUpdatingAudioObjects(&availableDynamic, &frames))) {
		return;
	}

	std::unique_lock<std::mutex> lock(m_mutex);

	// Découvre la nature des éléments (LFE ou objet) dès la première trame disponible.
	const Frame* pFirst = m_pCurrent ? m_pCurrent.get() : (!m_queue.empty() ? m_queue.front().f.get() : nullptr);
	unsigned nch = 0;
	if (pFirst) {
		nch = std::min<uint32_t>(pFirst->channels, 16);
		for (unsigned c = 0; c < nch; c++) {
			const uint32_t kind = pFirst->elements[c].kind;
			if (m_pObjects[c] && m_ObjectKind[c] != kind) {
				m_pObjects[c]->SetEndOfStream(0);
				SafeRelease(m_pObjects[c]);
			}
			if (!m_pObjects[c]) {
				const AudioObjectType t = (kind == KIND_LFE) ? AudioObjectType_LowFrequency : AudioObjectType_Dynamic;
				if (SUCCEEDED(m_pStream->ActivateSpatialAudioObject(t, &m_pObjects[c]))) {
					m_ObjectKind[c] = kind;
				}
			}
		}
	}

	// Tampons de tous les objets actifs, remis à zéro.
	float* out[16] = {};
	for (unsigned c = 0; c < 16; c++) {
		if (m_pObjects[c]) {
			BYTE* b = nullptr;
			UINT32 len = 0;
			if (SUCCEEDED(m_pObjects[c]->GetBuffer(&b, &len)) && b) {
				memset(b, 0, len);
				out[c] = (float*)b;
			}
		}
	}

	UINT32 i = 0;
	bool stateSet = false;
	if (m_bRunning) {
		while (i < frames) {
			if (!m_pCurrent) {
				if (m_queue.empty()) {
					break;
				}
				QueuedFrame q = std::move(m_queue.front());
				m_queue.pop_front();
				m_nQueuedSamples -= q.f->samples;
				m_pCurrent = std::move(q.f);
				m_rtCurrentStart = q.rtStart;
				m_nCurrentOffset = 0;

				// Saut d'horodatage : on recale l'horloge, comme le renderer d'origine.
				const REFERENCE_TIME delta = m_rtCurrentStart - m_rtNextRendered;
				if (m_rtNextRendered && (delta > 200 || delta < -200) && OnClockOffset) {
					OnClockOffset(delta);
				}
				m_rtNextRendered = m_rtCurrentStart + SamplesToTime(m_pCurrent->samples);
				SetEvent(m_hSpaceEvent);
			}
			if (!stateSet) {
				memcpy(m_State, m_pCurrent->elements, sizeof(m_State));
				m_bHaveState = stateSet = true;
			}
			const UINT32 n = std::min<UINT32>(frames - i, m_pCurrent->samples - m_nCurrentOffset);
			const unsigned ch = std::min<uint32_t>(m_pCurrent->channels, 16);
			for (unsigned c = 0; c < ch; c++) {
				if (out[c]) {
					for (UINT32 k = 0; k < n; k++) {
						out[c][i + k] = m_pCurrent->pcm[m_nCurrentOffset + k][c];
					}
				}
			}
			i += n;
			m_nCurrentOffset += n;
			m_rtCurrentRendered = m_rtCurrentStart + SamplesToTime(m_nCurrentOffset);
			if (m_nCurrentOffset >= m_pCurrent->samples) {
				m_pCurrent.reset();
			}
		}
	}

	// Positions et gains de la trame qui couvre le début de la période.
	for (unsigned c = 0; c < 16; c++) {
		if (!m_pObjects[c]) {
			continue;
		}
		const Element& e = m_State[c];
		const float gain = (m_bHaveState && e.active) ? e.gain * m_fVolume : 0.0f;
		m_pObjects[c]->SetVolume(gain);
		if (m_ObjectKind[c] != KIND_LFE && m_bHaveState) {
			// DAMF : x gauche/droite, y arrière/avant, z sol/plafond
			// Windows : x droite, y haut, z vers l'arrière
			m_pObjects[c]->SetPosition(e.x, e.z, -e.y);
		}
	}

	lock.unlock();
	m_pStream->EndUpdatingAudioObjects();
}
