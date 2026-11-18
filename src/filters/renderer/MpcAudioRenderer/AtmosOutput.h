/*
 * AtmosOutput — MPC-BE Atmos Edition
 *
 * Décode le TrueHD (Atmos ou non) reçu emballé en IEC 61937 / MAT et le joue
 * par Windows Spatial Sound (« Dolby Atmos for home theater ») : chaque élément
 * de la piste devient un objet audio positionné. Le décodage TrueHD est assuré
 * par la bibliothèque thdatmos.dll (fondée sur truehdd, licence Apache 2.0).
 *
 * Synchronisation façon ReClock : quand elle est active, la lecture est
 * accélérée ou ralentie d'un facteur proche de 1 (rééchantillonnage) pour que
 * le film suive exactement la fréquence réelle de l'écran.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <spatialaudioclient.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// Journal de diagnostic : atmos.log, a cote du module (.ax).
void AtmosLog(const wchar_t* fmt, ...);

class CAtmosOutput
{
public:
	CAtmosOutput();
	~CAtmosOutput();

	// thdatmos.dll présente à côté du module et chargée ?
	static bool LibraryAvailable();

	// Ouvre le flux spatial sur le périphérique (mode « Dolby Atmos for home theater »
	// actif requis) et démarre le rendu (silence tant que SetRunning(false)).
	HRESULT Open(IMMDevice* pDevice);
	void    Close();
	bool    IsOpen() const { return m_pStream != nullptr; }

	// Une salve IEC 61937 TrueHD. Bloque tant que la file dépasse ~1 s, sauf si
	// hAbort1/hAbort2 sont signalés (arrêt, vidage).
	HRESULT Receive(const BYTE* pData, long size, REFERENCE_TIME rtStart, bool bDiscontinuity,
					HANDLE hAbort1, HANDLE hAbort2);

	void Flush();                     // vide la file et remet le décodeur à zéro
	void SetRunning(bool bRunning);   // false : rendu de silence
	void SetVolume(float volume);     // gain linéaire global

	// Vitesse de lecture (1.0 = normale). Appliquée par rééchantillonnage.
	void   SetSpeed(double speed);
	double GetSpeed();

	IAudioClock*   GetAudioClock() const { return m_pAudioClock; }
	REFERENCE_TIME GetCurrentRenderedTime();
	size_t         QueuedSamples();
	unsigned       GetDynamicObjectCount() const { return m_nMaxDynamic; }

	// Appelé par le fil de rendu quand l'horodatage saute (comme le renderer d'origine).
	std::function<void(REFERENCE_TIME)> OnClockOffset;
	// Appelé par le fil de rendu environ une fois par seconde (réglage de la vitesse).
	std::function<void()> OnTick;

	struct Element { uint32_t kind, active; float x, y, z, gain; };
	struct Frame {
		uint64_t sample_pos; uint32_t samples, channels, sample_rate;
		float pcm[160][16];
		Element elements[16];
	};

private:
	struct QueuedFrame {
		REFERENCE_TIME rtStart;
		std::unique_ptr<Frame> f;
	};
	// Tronçon d'entrée tamponné : métadonnées valables à partir de l'échantillon start.
	struct Segment {
		uint64_t       start;
		uint32_t       len;
		uint32_t       channels;
		REFERENCE_TIME rtStart;
		Element        elements[16];
	};

	static DWORD WINAPI ThreadProc(LPVOID p) { return static_cast<CAtmosOutput*>(p)->RenderThread(); }
	DWORD RenderThread();
	void  RenderPeriod();
	bool  PullFrame();                             // file -> tampon d'entrée (verrou tenu)
	float Sample(unsigned c, int64_t index) const; // échantillon d'entrée (0 hors tampon)
	const Segment* SegmentAt(uint64_t index) const;
	void  ResetBuffers();

	ISpatialAudioClient*             m_pClient = nullptr;
	ISpatialAudioObjectRenderStream* m_pStream = nullptr;
	IAudioClock*                     m_pAudioClock = nullptr;
	ISpatialAudioObject*             m_pObjects[16] = {};
	uint32_t                         m_ObjectKind[16] = {};
	unsigned                         m_nMaxDynamic = 0;

	HANDLE m_hBufferEvent = nullptr;
	HANDLE m_hStopEvent = nullptr;
	HANDLE m_hSpaceEvent = nullptr;   // signalé quand la file se vide
	HANDLE m_hThread = nullptr;

	std::mutex              m_mutex;
	std::deque<QueuedFrame> m_queue;
	size_t                  m_nQueuedSamples = 0;

	// Tampon d'entrée : m_in[c][k] = échantillon d'indice m_base + k
	std::vector<float>  m_in[16];
	uint64_t            m_base = 0;
	uint64_t            m_end = 0;      // indice suivant le dernier échantillon tamponné
	double              m_pos = 0.0;    // position de lecture (en échantillons d'entrée)
	std::deque<Segment> m_segments;
	REFERENCE_TIME      m_rtNextQueued = 0;

	Element m_State[16] = {};
	bool    m_bHaveState = false;

	void*          m_pDecoder = nullptr; // contexte thdatmos
	REFERENCE_TIME m_rtNextDecoded = INVALID_TIME_VALUE; // ancre (début de segment)
	uint64_t       m_nDecodedSinceAnchor = 0;
	REFERENCE_TIME m_rtCurrentRendered = 0;
	bool           m_bRunning = false;
	float          m_fVolume = 1.0f;
	double         m_dSpeed = 1.0;
	DWORD          m_dwLastTick = 0;

	static constexpr REFERENCE_TIME INVALID_TIME_VALUE = INT64_MIN;
};
