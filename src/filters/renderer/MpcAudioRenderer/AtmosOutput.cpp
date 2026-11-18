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
#include <cmath>

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

	// Noyau d'interpolation sinc fenêtré (Kaiser), 48 coefficients, 2048 phases (~110 dB).
	constexpr int KERNEL_HALF = 24;
	constexpr int KERNEL_PHASES = 2048;

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
		float taps[KERNEL_PHASES + 1][2 * KERNEL_HALF];
		Kernel() {
			const double beta = 10.0, cutoff = 1.0;
			const double i0b = BesselI0(beta);
			for (int ph = 0; ph <= KERNEL_PHASES; ph++) {
				const double frac = (double)ph / KERNEL_PHASES;
				for (int k = 0; k < 2 * KERNEL_HALF; k++) {
					// distance entre l'échantillon k et la position fractionnaire
					const double t = (double)(k - KERNEL_HALF + 1) - frac;
					const double x = 3.14159265358979323846 * cutoff * t;
					const double sinc = (std::fabs(t) < 1e-12) ? 1.0 : std::sin(x) / x;
					const double r = t / KERNEL_HALF;
					const double w = (std::fabs(r) >= 1.0) ? 0.0 : BesselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
					taps[ph][k] = (float)(cutoff * sinc * w);
				}
			}
		}
	};
	const Kernel g_kernel;
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
	ResetBuffers();
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

void CAtmosOutput::ResetBuffers()
{
	for (auto& v : m_in) {
		v.clear();
	}
	m_base = m_end = 0;
	m_pos = 0.0;
	m_segments.clear();
	m_rtNextQueued = 0;
}

void CAtmosOutput::Flush()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_queue.clear();
	m_nQueuedSamples = 0;
	ResetBuffers();
	m_rtNextDecoded = INVALID_TIME_VALUE;
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

void CAtmosOutput::SetSpeed(double speed)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_dSpeed = speed;
}

double CAtmosOutput::GetSpeed()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_dSpeed;
}

const CAtmosOutput::Segment* CAtmosOutput::SegmentAt(uint64_t index) const
{
	const Segment* s = nullptr;
	for (const auto& seg : m_segments) {
		if (seg.start <= index) {
			s = &seg;
		} else {
			break;
		}
	}
	return s;
}

REFERENCE_TIME CAtmosOutput::GetCurrentRenderedTime()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (const Segment* s = SegmentAt((uint64_t)m_pos)) {
		return s->rtStart + (REFERENCE_TIME)((m_pos - (double)s->start) * UNITS_PER_SEC / SAMPLE_RATE);
	}
	if (!m_queue.empty()) {
		return m_queue.front().rtStart;
	}
	return m_rtCurrentRendered;
}

size_t CAtmosOutput::QueuedSamples()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const double buffered = (double)m_end - m_pos;
	return m_nQueuedSamples + (buffered > 0 ? (size_t)buffered : 0);
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
		const DWORD now = GetTickCount();
		if (now - m_dwLastTick >= 1000) {
			m_dwLastTick = now;
			if (OnTick) {
				OnTick();
			}
		}
	}
	return 0;
}

bool CAtmosOutput::PullFrame()
{
	if (m_queue.empty()) {
		return false;
	}
	QueuedFrame q = std::move(m_queue.front());
	m_queue.pop_front();
	m_nQueuedSamples -= q.f->samples;
	SetEvent(m_hSpaceEvent);

	// Saut d'horodatage : on recale l'horloge, comme le renderer d'origine.
	if (!m_segments.empty()) {
		const REFERENCE_TIME delta = q.rtStart - m_rtNextQueued;
		if ((delta > 200 || delta < -200) && OnClockOffset) {
			OnClockOffset(delta);
		}
	}
	m_rtNextQueued = q.rtStart + SamplesToTime(q.f->samples);

	Segment seg;
	seg.start    = m_end;
	seg.len      = q.f->samples;
	seg.channels = std::min<uint32_t>(q.f->channels, 16);
	seg.rtStart  = q.rtStart;
	memcpy(seg.elements, q.f->elements, sizeof(seg.elements));
	m_segments.push_back(seg);

	for (unsigned c = 0; c < 16; c++) {
		auto& v = m_in[c];
		const size_t old = v.size();
		v.resize(old + q.f->samples);
		for (uint32_t k = 0; k < q.f->samples; k++) {
			v[old + k] = (c < seg.channels) ? q.f->pcm[k][c] : 0.0f;
		}
	}
	m_end += q.f->samples;
	return true;
}

float CAtmosOutput::Sample(unsigned c, int64_t index) const
{
	if (index < (int64_t)m_base || index >= (int64_t)m_end) {
		return 0.0f;
	}
	return m_in[c][(size_t)(index - (int64_t)m_base)];
}

void CAtmosOutput::RenderPeriod()
{
	UINT32 availableDynamic = 0, frames = 0;
	if (FAILED(m_pStream->BeginUpdatingAudioObjects(&availableDynamic, &frames))) {
		return;
	}

	std::unique_lock<std::mutex> lock(m_mutex);

	// Alimente le tampon d'entrée : il faut couvrir la période plus le noyau d'interpolation.
	const double speed = m_bRunning ? m_dSpeed : 1.0;
	if (m_bRunning) {
		const double need = m_pos + frames * speed + KERNEL_HALF + 2;
		while ((double)m_end < need && PullFrame()) {
		}
	}

	// Découvre la nature des éléments (LFE ou objet) dès que des données sont là.
	const Segment* pFirst = SegmentAt((uint64_t)m_pos);
	if (!pFirst && !m_segments.empty()) {
		pFirst = &m_segments.front();
	}
	if (pFirst) {
		for (unsigned c = 0; c < pFirst->channels; c++) {
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

	if (m_bRunning && m_end > m_base) {
		if (const Segment* s = SegmentAt((uint64_t)m_pos)) {
			memcpy(m_State, s->elements, sizeof(m_State));
			m_bHaveState = true;
		}

		// Échantillons produits tant que le noyau reste dans le tampon.
		UINT32 i = 0;
		for (; i < frames; i++) {
			const double p = m_pos;
			const int64_t ip = (int64_t)std::floor(p);
			if (ip + KERNEL_HALF >= (int64_t)m_end) {
				break; // famine : le reste de la période est silencieux
			}
			const double frac = p - (double)ip;
			const unsigned phase = (unsigned)(frac * KERNEL_PHASES);
			if (phase == 0 && frac * KERNEL_PHASES < 1e-9) {
				// Position entière : copie exacte (aucune altération à vitesse 1).
				for (unsigned c = 0; c < 16; c++) {
					if (out[c]) {
						out[c][i] = Sample(c, ip);
					}
				}
			} else {
				const float mu = (float)(frac * KERNEL_PHASES - phase);
				const float* h0 = &g_kernel.taps[phase][0];
				const float* h1 = &g_kernel.taps[phase + 1][0];
				for (unsigned c = 0; c < 16; c++) {
					if (!out[c]) {
						continue;
					}
					float acc = 0.0f;
					for (int k = 0; k < 2 * KERNEL_HALF; k++) {
						const float h = h0[k] + mu * (h1[k] - h0[k]);
						acc += h * Sample(c, ip - KERNEL_HALF + 1 + k);
					}
					out[c][i] = acc;
				}
			}
			m_pos += speed;
		}

		if (const Segment* s = SegmentAt((uint64_t)m_pos)) {
			m_rtCurrentRendered = s->rtStart + (REFERENCE_TIME)((m_pos - (double)s->start) * UNITS_PER_SEC / SAMPLE_RATE);
		}

		// Libère l'entrée consommée (en gardant le contexte du noyau).
		const int64_t keep = (int64_t)std::floor(m_pos) - KERNEL_HALF - 1;
		if (keep > (int64_t)m_base + 4096) {
			const size_t drop = (size_t)(keep - (int64_t)m_base);
			for (auto& v : m_in) {
				v.erase(v.begin(), v.begin() + drop);
			}
			m_base += drop;
			while (m_segments.size() > 1 && m_segments[1].start <= m_base) {
				m_segments.pop_front();
			}
		}
	}

	// Positions et gains au début de la période.
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
