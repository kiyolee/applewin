/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2007, Tom Charlesworth, Michael Pohoreski

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: Core sound related functionality
 *
 * Author: Tom Charlesworth
 */

#include "StdAfx.h"

#include "SoundCore.h"
#include "Core.h"
#include "Interface.h"
#include "Log.h"
#include "Speaker.h"

//-------------------------------------

// Used for muting & fading:

static const UINT uMAX_VOICES = NUM_SLOTS * 2 + 1 + 1;	// 8x (2x SSI263) + spkr + MockingboardCardManager
UINT g_uNumVoices = 0;
static VOICE* g_pVoices[uMAX_VOICES] = {NULL};

static VOICE* g_pSpeakerVoice = NULL;

//-----------------------------------------------------------------------------

// NB. Also similar is done by: MockingboardCardManager::Destroy()
// - which is called from WM_DESTROY (when both restarting VM & exiting the app)

VOICE::~VOICE(void)
{
	if (lpDSBvoice)
	{
		DSVoiceStop(this);
		DSReleaseSoundBuffer(this);
	}
}

//-----------------------------------------------------------------------------

#ifdef _DEBUG
static const char *DirectSound_ErrorText (HRESULT error)
{
    switch( error )
    {
    case DSERR_ALLOCATED:
        return "Allocated";
    case DSERR_CONTROLUNAVAIL:
        return "Control Unavailable";
    case DSERR_INVALIDPARAM:
        return "Invalid Parameter";
    case DSERR_INVALIDCALL:
        return "Invalid Call";
    case DSERR_GENERIC:
        return "Generic";
    case DSERR_PRIOLEVELNEEDED:
        return "Priority Level Needed";
    case DSERR_OUTOFMEMORY:
        return "Out of Memory";
    case DSERR_BADFORMAT:
        return "Bad Format";
    case DSERR_UNSUPPORTED:
        return "Unsupported";
    case DSERR_NODRIVER:
        return "No Driver";
    case DSERR_ALREADYINITIALIZED:
        return "Already Initialized";
    case DSERR_NOAGGREGATION:
        return "No Aggregation";
    case DSERR_BUFFERLOST:
        return "Buffer Lost";
    case DSERR_OTHERAPPHASPRIO:
        return "Other Application Has Priority";
    case DSERR_UNINITIALIZED:
        return "Uninitialized";
    case DSERR_NOINTERFACE:
        return "No Interface";
    default:
        return "Unknown";
    }
}
#endif

//-----------------------------------------------------------------------------

HRESULT DSGetLock(const std::shared_ptr<SoundBuffer>& pVoice, uint32_t dwOffset, uint32_t dwBytes,
					  SHORT** ppDSLockedBuffer0, DWORD* pdwDSLockedBufferSize0,
					  SHORT** ppDSLockedBuffer1, DWORD* pdwDSLockedBufferSize1)
{
	DWORD nStatus;
	HRESULT hr = pVoice->GetStatus(&nStatus);
	if(hr != DS_OK)
		return hr;

	if(nStatus & DSBSTATUS_BUFFERLOST)
	{
		do
		{
			hr = pVoice->Restore();
			if(hr == DSERR_BUFFERLOST)
				Sleep(10);
		}
		while(hr != DS_OK);
	}

	// Get write only pointer(s) to sound buffer
	if(dwBytes == 0)
	{
		if(FAILED(hr = pVoice->Lock(0, 0,
								(void**)ppDSLockedBuffer0, pdwDSLockedBufferSize0,
								(void**)ppDSLockedBuffer1, pdwDSLockedBufferSize1,
								DSBLOCK_ENTIREBUFFER)))
			return hr;
	}
	else
	{
		if(FAILED(hr = pVoice->Lock(dwOffset, dwBytes,
								(void**)ppDSLockedBuffer0, pdwDSLockedBufferSize0,
								(void**)ppDSLockedBuffer1, pdwDSLockedBufferSize1,
								0)))
			return hr;
	}

	return hr;
}

//-----------------------------------------------------------------------------

HRESULT DSGetSoundBuffer(VOICE* pVoice, uint32_t dwBufferSize, uint32_t nSampleRate, int nChannels, const char* pszVoiceName)
{
	pVoice->name = pszVoiceName;

	std::shared_ptr<SoundBuffer> soundBuffer = GetFrame().CreateSoundBuffer(dwBufferSize, nSampleRate, nChannels, pszVoiceName);
	if (!soundBuffer)
		return E_FAIL;

	pVoice->lpDSBvoice = soundBuffer;

	_ASSERT(g_uNumVoices < uMAX_VOICES);
	if(g_uNumVoices < uMAX_VOICES)
		g_pVoices[g_uNumVoices++] = pVoice;

	if(pVoice->bIsSpeaker)
		g_pSpeakerVoice = pVoice;

	return DS_OK;
}

void DSReleaseSoundBuffer(VOICE* pVoice)
{
	if(pVoice->bIsSpeaker)
		g_pSpeakerVoice = NULL;

	for(UINT i=0; i<g_uNumVoices; i++)
	{
		if(g_pVoices[i] == pVoice)
		{
			g_pVoices[i] = g_pVoices[g_uNumVoices-1];
			g_pVoices[g_uNumVoices-1] = NULL;
			g_uNumVoices--;
			break;
		}
	}

	pVoice->lpDSBvoice.reset();
}

//-----------------------------------------------------------------------------

bool DSVoiceStop(PVOICE Voice)
{
#ifdef NO_DIRECT_X
	return false;
#else
	_ASSERT(Voice->lpDSBvoice);
	HRESULT hr = Voice->lpDSBvoice->Stop();
	if(FAILED(hr))
	{
		if(g_fh) fprintf(g_fh, "%s: DSStop failed (%08X)\n", Voice->name.c_str(), (uint32_t)hr);
		return false;
	}

	Voice->bActive = false;
	return true;
#endif // NO_DIRECT_X
}

// Use this to Play()
bool DSZeroVoiceBuffer(PVOICE Voice, uint32_t dwBufferSize)
{
#ifdef NO_DIRECT_X
	return false;
#else

	DWORD dwDSLockedBufferSize = 0;    // Size of the locked DirectSound buffer
	SHORT* pDSLockedBuffer;

	if (!DSVoiceStop(Voice))
		return false;

	HRESULT hr = DSGetLock(Voice->lpDSBvoice, 0, 0, &pDSLockedBuffer, &dwDSLockedBufferSize, NULL, 0);
	if(FAILED(hr))
	{
		if(g_fh) fprintf(g_fh, "%s: DSGetLock failed (%08X)\n", Voice->name.c_str(), (uint32_t)hr);
		return false;
	}

	_ASSERT(dwDSLockedBufferSize == dwBufferSize);
	memset(pDSLockedBuffer, 0x00, dwDSLockedBufferSize);

	hr = Voice->lpDSBvoice->Unlock((void*)pDSLockedBuffer, dwDSLockedBufferSize, NULL, 0);
	if(FAILED(hr))
	{
		if(g_fh) fprintf(g_fh, "%s: DSUnlock failed (%08X)\n", Voice->name.c_str(), (uint32_t)hr);
		return false;
	}

	hr = Voice->lpDSBvoice->Play(0,0,DSBPLAY_LOOPING);
	if(FAILED(hr))
	{
		if(g_fh) fprintf(g_fh, "%s: DSPlay failed (%08X)\n", Voice->name.c_str(), (uint32_t)hr);
		return false;
	}

	Voice->bActive = true;
	return true;
#endif // NO_DIRECT_X
}

//-----------------------------------------------------------------------------

bool DSZeroVoiceWritableBuffer(PVOICE Voice, uint32_t dwBufferSize)
{
	DWORD dwDSLockedBufferSize0=0, dwDSLockedBufferSize1=0;
	SHORT *pDSLockedBuffer0, *pDSLockedBuffer1;


	HRESULT hr = DSGetLock(Voice->lpDSBvoice,
							0, dwBufferSize,
							&pDSLockedBuffer0, &dwDSLockedBufferSize0,
							&pDSLockedBuffer1, &dwDSLockedBufferSize1);
	if(FAILED(hr))
	{
		if(g_fh) fprintf(g_fh, "%s: DSGetLock failed (%08X)\n", Voice->name.c_str(), (uint32_t)hr);
		return false;
	}

	memset(pDSLockedBuffer0, 0x00, dwDSLockedBufferSize0);
	if(pDSLockedBuffer1)
		memset(pDSLockedBuffer1, 0x00, dwDSLockedBufferSize1);

	hr = Voice->lpDSBvoice->Unlock((void*)pDSLockedBuffer0, dwDSLockedBufferSize0,
									(void*)pDSLockedBuffer1, dwDSLockedBufferSize1);
	if(FAILED(hr))
	{
		if(g_fh) fprintf(g_fh, "%s: DSUnlock failed (%08X)\n", Voice->name.c_str(), (uint32_t)hr);
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------

static bool g_bTimerActive = false;
static eFADE g_FadeType = FADE_NONE;
static UINT_PTR g_nTimerID = 0;

//-------------------------------------

static VOID CALLBACK SoundCore_TimerFunc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);

static bool SoundCore_StartTimer()
{
	if(g_bTimerActive)
		return true;

	g_nTimerID = SetTimer(NULL, 0, 1, SoundCore_TimerFunc);	// 1ms interval
	if(g_nTimerID == 0)
	{
		fprintf(stderr, "Error creating timer\n");
		_ASSERT(0);
		return false;
	}

	g_bTimerActive = true;
	return true;
}

void SoundCore_StopTimer()
{
	if(!g_bTimerActive)
		return;

	if(KillTimer(NULL, g_nTimerID) == FALSE)
	{
		fprintf(stderr, "Error killing timer\n");
		_ASSERT(0);
		return;
	}

	g_bTimerActive = false;
}

bool SoundCore_GetTimerState()
{
	return g_bTimerActive;
}

//-------------------------------------

// [OLD: Used to fade volume in/out]
// FADE_OUT : Just keep filling speaker soundbuffer with same value
// FADE_IN  : Switch to FADE_NONE & StopTimer()

static VOID CALLBACK SoundCore_TimerFunc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	if((g_pSpeakerVoice == NULL) || (g_pSpeakerVoice->bActive == false))
		g_FadeType = FADE_NONE;

	// Timer expired
	if(g_FadeType == FADE_NONE)
	{
		SoundCore_StopTimer();
		return;
	}

	//

#if 1
	if(g_FadeType == FADE_IN)
		g_FadeType = FADE_NONE;
	else
		SpkrUpdate_Timer();
#else
	const LONG nFadeUnit_Fast = (DSBVOLUME_MAX - DSBVOLUME_MIN) / 10;
	const LONG nFadeUnit_Slow = (DSBVOLUME_MAX - DSBVOLUME_MIN) / 1000;	// Less noisy for 'silence'

	LONG nFadeUnit = g_pSpeakerVoice->bRecentlyActive ? nFadeUnit_Fast : nFadeUnit_Slow;
	LONG nFadeVolume = g_pSpeakerVoice->nFadeVolume;

	if(g_FadeType == FADE_IN)
	{
		if(nFadeVolume == g_pSpeakerVoice->nVolume)
		{
			g_FadeType = FADE_NONE;
			SoundCore_StopTimer();
			return;
		}

		nFadeVolume += nFadeUnit;

		if(nFadeVolume > g_pSpeakerVoice->nVolume)
			nFadeVolume = g_pSpeakerVoice->nVolume;
	}
	else // FADE_OUT
	{
		if(nFadeVolume == DSBVOLUME_MIN)
		{
			g_FadeType = FADE_NONE;
			SoundCore_StopTimer();
			return;
		}

		nFadeVolume -= nFadeUnit;

		if(nFadeVolume < DSBVOLUME_MIN)
			nFadeVolume = DSBVOLUME_MIN;
	}

	g_pSpeakerVoice->nFadeVolume = nFadeVolume;
	g_pSpeakerVoice->lpDSBvoice->SetVolume(nFadeVolume);
#endif
}

//-----------------------------------------------------------------------------

void SoundCore_SetFade(eFADE FadeType)
{
	static AppMode_e nLastMode = MODE_UNDEFINED;

	if(g_nAppMode == MODE_DEBUG)
		return;

	// Fade in/out for speaker, the others are unmuted/muted here
	if(FadeType != FADE_NONE)
	{
		for(UINT i=0; i<g_uNumVoices; i++)
		{
			// Note: Kludge for fading speaker if curr/last g_nAppMode is/was MODE_LOGO:
			// . Bug in DirectSound? SpeakerVoice.lpDSBvoice->SetVolume() doesn't work without this!
			// . See SoundCore_TweakVolumes() - could be this?
			if((g_pVoices[i]->bIsSpeaker) && (g_nAppMode != MODE_LOGO) && (nLastMode != MODE_LOGO))
			{
				g_pVoices[i]->lpDSBvoice->GetVolume(&g_pVoices[i]->nFadeVolume);
				g_FadeType = FadeType;
				SoundCore_StartTimer();
			}
			else if(FadeType == FADE_OUT)
			{
				g_pVoices[i]->lpDSBvoice->SetVolume(DSBVOLUME_MIN);
				g_pVoices[i]->bMute = true;
			}
			else // FADE_IN
			{
				g_pVoices[i]->lpDSBvoice->SetVolume(g_pVoices[i]->nVolume);
				g_pVoices[i]->bMute = false;
			}
		}
	}
	else // FadeType == FADE_NONE
	{
		if( (g_FadeType != FADE_NONE) &&	// Currently fading-in/out
			(g_pSpeakerVoice && g_pSpeakerVoice->bActive) )
		{
			g_FadeType = FADE_NONE;			// TimerFunc will call StopTimer()
			g_pSpeakerVoice->lpDSBvoice->SetVolume(g_pSpeakerVoice->nVolume);
		}
	}

	nLastMode = g_nAppMode;
}

//-----------------------------------------------------------------------------

// If AppleWin started by double-clicking a .dsk, then our window won't have focus when volumes are set (so gets ignored).
// Subsequent setting (to the same volume) will get ignored, as DirectSound thinks that volume is already set.

void SoundCore_TweakVolumes()
{
	for (UINT i=0; i<g_uNumVoices; i++)
	{
		g_pVoices[i]->lpDSBvoice->SetVolume(g_pVoices[i]->nVolume-1);
		g_pVoices[i]->lpDSBvoice->SetVolume(g_pVoices[i]->nVolume);
	}
}

//-----------------------------------------------------------------------------

LONG NewVolume(uint32_t dwVolume, uint32_t dwVolumeMax)
{
	float fVol = (float) dwVolume / (float) dwVolumeMax;	// 0.0=Max, 1.0=Min

	return (LONG) ((float) DSBVOLUME_MIN * fVol);
}

//=============================================================================

static int g_nErrorInc = 20;	// Old: 1
static int g_nErrorMax = 200;	// Old: 50

int SoundCore_GetErrorInc()
{
	return g_nErrorInc;
}

void SoundCore_SetErrorInc(const int nErrorInc)
{
	g_nErrorInc = nErrorInc < g_nErrorMax ? nErrorInc : g_nErrorMax;
	if(g_fh) fprintf(g_fh, "Speaker/MB Error Inc = %d\n", g_nErrorInc);
}

int SoundCore_GetErrorMax()
{
	return g_nErrorMax;
}

void SoundCore_SetErrorMax(const int nErrorMax)
{
	g_nErrorMax = nErrorMax < MAX_SAMPLES ? nErrorMax : MAX_SAMPLES;
	if(g_fh) fprintf(g_fh, "Speaker/MB Error Max = %d\n", g_nErrorMax);
}

//=============================================================================

// Use DWORD_PTR according to IReferenceClock from <strmif.h>.
static DWORD_PTR g_pdwAdviseCookie = 0; // Not really used as pointer.
static IReferenceClock *g_pRefClock = NULL;
static HANDLE g_hSemaphore = NULL;
static bool g_bRefClockTimerActive = false;
static uint32_t g_dwLastUsecPeriod = 0;


bool SysClk_InitTimer()
{
	g_hSemaphore = CreateSemaphore(NULL, 0, 1, NULL);		// Max count = 1
	if (g_hSemaphore == NULL)
	{
		fprintf(stderr, "Error creating semaphore\n");
		return false;
	}

	if (CoCreateInstance(CLSID_SystemClock, NULL, CLSCTX_INPROC,
                         IID_IReferenceClock, (LPVOID*)&g_pRefClock) != S_OK)
	{
		fprintf(stderr, "Error initialising COM\n");
		return false;	// Fails for Win95!
	}

	return true;
}

void SysClk_UninitTimer()
{
	SysClk_StopTimer();

	SAFE_RELEASE(g_pRefClock);

	if (CloseHandle(g_hSemaphore) == 0)
		fprintf(stderr, "Error closing semaphore handle\n");
}

//

void SysClk_WaitTimer()
{
	if(!g_bRefClockTimerActive)
		return;

	WaitForSingleObject(g_hSemaphore, INFINITE);
}

//

void SysClk_StartTimerUsec(uint32_t dwUsecPeriod)
{
	if(g_bRefClockTimerActive && (g_dwLastUsecPeriod == dwUsecPeriod))
		return;

	SysClk_StopTimer();

	REFERENCE_TIME rtPeriod = (REFERENCE_TIME) (dwUsecPeriod * 10);	// In units of 100ns
	REFERENCE_TIME rtNow;

	HRESULT hr = g_pRefClock->GetTime(&rtNow);
	// S_FALSE : Returned time is the same as the previous value

	if ((hr != S_OK) && (hr != S_FALSE))
	{
		_ASSERT(0);
		return;
	}

	// IReferenceClock from <strmif.h> (origin <axcore.idl>) is "oddly" defined to use HSEMAPHORE.
	static_assert(sizeof(HSEMAPHORE) == sizeof(HANDLE), "must be same size");
	if (g_pRefClock->AdvisePeriodic(rtNow, rtPeriod, (HSEMAPHORE)g_hSemaphore, &g_pdwAdviseCookie) != S_OK)
	{
		fprintf(stderr, "Error creating timer\n");
		_ASSERT(0);
		return;
	}

	g_dwLastUsecPeriod = dwUsecPeriod;
	g_bRefClockTimerActive = true;
}

void SysClk_StopTimer()
{
	if(!g_bRefClockTimerActive)
		return;

	if (g_pRefClock->Unadvise(g_pdwAdviseCookie) != S_OK)
	{
		fprintf(stderr, "Error deleting timer\n");
		_ASSERT(0);
		return;
	}

	g_bRefClockTimerActive = false;
}
