/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#include "kdspaudio.h"
#include <SDL.h>
#include "../../source/kernel/devs/oss.h"

#define DSP_BUFFER_SIZE (1024*32)

static bool sdlAudioOpen;
static U8 sdlSilence;

void closeSdlAudio() {
	if (sdlAudioOpen) {
		sdlAudioOpen = false;
		SDL_PauseAudio(1);
		SDL_CloseAudio();
	}
}

class KDspAudioSdl : public KDspAudio, public std::enable_shared_from_this<KDspAudioSdl> {
public:
	KDspAudioSdl() {
		this->want.format = AUDIO_U8;
		this->want.channels = 1;
		this->want.freq = 11025;
#ifdef __EMSCRIPTEN__
		this->want.samples = 8192; //Must be pow of 2
#else
		this->want.samples = 5512;
#endif
		this->got.channels = 1;
		this->got.freq = 11025;
#ifdef __EMSCRIPTEN__
		this->got.samples = 8192; //Must be pow of 2
#else
		this->got.samples = 5512;
#endif
	}

	virtual ~KDspAudioSdl() {
		if (this->cvtBuf) {
			SDL_free(this->cvtBuf);
		}
	}

	void openAudio(U32 format, U32 freq, U32 channels) override;
	void soundEnabled() override;
	bool isOpen() override { return this->open; }
	void closeAudio() override;
	U32 writeAudio(U8* data, U32 len) override;
	U32 getFragmentSize() override {return this->dspFragSize;}
	U32 getBufferSize() override {return (U32)0;}
	U32 getBufferCapacity() override { return DSP_BUFFER_SIZE;}

	void onClose();
	void closeAudioFromAudioThread();

	U32 bytesPerSampleWant() {
		if (this->want.format == AUDIO_S16LSB || this->want.format == AUDIO_S16MSB || this->want.format == AUDIO_U16LSB || this->want.format == AUDIO_U16MSB)
			return 2;
		else if (this->want.format == AUDIO_F32LSB)
			return 4;
		else
			return 1;
	}

	U32 getSdlFormat(U32 format) {
		switch (format) {
		case AFMT_MU_LAW:
		case AFMT_A_LAW:
		case AFMT_IMA_ADPCM:
		case AFMT_U8:
			return AUDIO_U8;		
		case AFMT_S16_LE:
			return AUDIO_S16LSB;
		case AFMT_S16_BE:
			return AUDIO_S16MSB;
		case AFMT_S8:
			return AUDIO_S8;
		case AFMT_U16_LE:
			return AUDIO_U16LSB;
		case AFMT_U16_BE:
			return AUDIO_U16MSB;
		case AFMT_MPEG:
			return AUDIO_U8;
        case AFMT_FLOAT:
                return AUDIO_F32LSB;
		default:
			kpanic_fmt("KNativeAudioSdl Unknow audio format %d", format);
			return 0;
		}
	}
	SDL_AudioSpec want = { 0 };
	SDL_AudioSpec got = { 0 };
	SDL_AudioCVT cvt = { 0 };
	U32 openedFormat = 0;
	int cvtBufLen = 0;
	int cvtBufPos = 0;
	unsigned char* cvtBuf = nullptr;
	bool sameFormat = false;
	U32 dspFragSize = 4096;
	bool open = false;
	std::deque<U8> audioBuffer;
	bool closeWhenDone = false;
};

// not really a voice, currently they are not mixed
std::list<std::shared_ptr<KDspAudioSdl>> voices;

void audioCallback(void* userdata, U8* stream, S32 len) {
	if (!voices.size()) {
		memset(stream, sdlSilence, len);
		return;
	}
	std::shared_ptr<KDspAudioSdl> data = voices.front();
	if (data->closeWhenDone && data->audioBuffer.size()==0 && (data->cvtBufPos == 0 || data->cvtBufPos >= data->cvt.len_cvt)) {
		data->closeAudioFromAudioThread();
		memset(stream, sdlSilence, len);
		return;
	}

	S32 available = (S32)data->audioBuffer.size();

	if (!data->sameFormat) {
		if (data->cvtBufPos < data->cvt.len_cvt) {
			S32 todo = data->cvt.len_cvt - data->cvtBufPos;
			if (todo > len) {
				todo = len;
			}
			memcpy(stream, data->cvt.buf + data->cvtBufPos, todo);
			data->cvtBufPos += todo;
			stream += todo;
			len -= todo;
		}
		if (len) {
			data->cvt.len = available;
			if (data->cvtBufLen && data->cvtBufLen < data->cvt.len * data->cvt.len_mult) {
				data->cvtBufLen = 0;
				SDL_free(data->cvtBuf);
				data->cvtBuf = nullptr;
			}
			if (!data->cvtBufLen) {
				data->cvtBufLen = data->cvt.len * data->cvt.len_mult;
				data->cvtBuf = (Uint8*)SDL_malloc(data->cvt.len * data->cvt.len_mult);
			}
			data->cvt.buf = data->cvtBuf;

			std::copy(data->audioBuffer.begin(), data->audioBuffer.begin() + available, data->cvt.buf);
			data->audioBuffer.erase(data->audioBuffer.begin(), data->audioBuffer.begin() + available);

			SDL_ConvertAudio(&data->cvt);
			S32 todo = data->cvt.len_cvt;
			if (todo > len) {
				todo = len;
			}
			memcpy(stream, data->cvt.buf, todo);
			stream += todo;
			len -= todo;
			data->cvtBufPos = todo;
		}
	} else {
		if (available > len)
			available = len;
		if (available) {
			std::copy(data->audioBuffer.begin(), data->audioBuffer.begin() + available, stream);
			data->audioBuffer.erase(data->audioBuffer.begin(), data->audioBuffer.begin() + available);
			len -= available;
			stream += available;
		}
	}
	if (len) {
		memset(stream, data->got.silence, len);
	}
}

void KDspAudioSdl::soundEnabled() {
	if (this->open) {
		openAudio(this->openedFormat, this->want.freq, this->want.channels);
	}
}

void KDspAudioSdl::openAudio(U32 format, U32 freq, U32 channels) {
	//want.samples = 4096;    
	this->want.callback = audioCallback;
	this->want.format = getSdlFormat(format);
	this->want.freq = freq;
	this->want.channels = channels;
	this->openedFormat = format;

	if (!KSystem::soundEnabled) {
		sdlAudioOpen = true;
		this->open = true;
		return;
	}
	// If the previous audio is still playing, it will get cut off.  If I find a game that needs this, then perhaps I should think of a mixer.
	closeSdlAudio();
    SDL_AudioSpec requested = this->want;
#ifdef __MACH__
    if (requested.freq < 44100) {
        requested.freq = 44100;
    }
#endif
	if (SDL_OpenAudio(&requested, &this->got) < 0) {
		klog_fmt("Failed to open audio: %s", SDL_GetError());
	}
	sdlSilence = this->got.silence;
	sdlAudioOpen = true;
	if (this->want.freq != this->got.freq || this->want.channels != this->got.channels || this->want.format != this->got.format) {
		this->sameFormat = false;
		SDL_BuildAudioCVT(&this->cvt, this->want.format, this->want.channels, this->want.freq, this->got.format, this->got.channels, this->got.freq);
	} else {
		this->sameFormat = true;
	}

	this->open = true;
	voices.push_back(shared_from_this());
	SDL_PauseAudio(0);
	if (this->got.size) {
		//this->dspFragSize = this->got.size;
	}
	klog_fmt("openAudio: freq=%d(got %d) format=%x(got %x) channels=%d(got %d)", this->want.freq, this->got.freq, this->want.format, this->got.format, this->want.channels, this->got.channels);
}

void KDspAudioSdl::closeAudioFromAudioThread() {
	if (this->open) {
		this->onClose();
	}
}

void KDspAudioSdl::closeAudio() {
	if (!KSystem::soundEnabled) {
		this->open = false;
		return;
	}
	if (this->open) {
		bool needClose = true;
		SDL_LockAudio();
		if (audioBuffer.size() || (this->cvtBufPos != 0 && this->cvtBufPos < this->cvt.len_cvt)) {
			closeWhenDone = true;
			needClose = false;
		}
		SDL_UnlockAudio();
		if (needClose) {
			closeSdlAudio();
			this->onClose();
		}				
	}
}

void KDspAudioSdl::onClose() {
	if (!KSystem::soundEnabled) {
		this->open = false;
		return;
	}
	auto it = voices.begin();
	while (it != voices.end()) {
		std::shared_ptr<KDspAudioSdl> p = *it;
		if (p == shared_from_this()) {
			it = voices.erase(it);
			break;
		} else {
			it++;
		}
	}
	this->open = false;
}

U32 KDspAudioSdl::writeAudio(U8* data, U32 len) {
	U32 bytesPerSecond = want.freq * want.channels * bytesPerSampleWant();
	U32 delay = bytesPerSecond / 8;

	if (!KSystem::soundEnabled) {
		static U32 timeSinceLastWrite;

		U32 elapsedTime = KSystem::getMilliesSinceStart() - timeSinceLastWrite;
		U32 bytesToRemove = bytesPerSecond * elapsedTime / 1000;
		
		bytesToRemove = std::min((U32)this->audioBuffer.size(), bytesToRemove);
		this->audioBuffer.erase(this->audioBuffer.begin(), this->audioBuffer.begin() + bytesToRemove);

		if (this->audioBuffer.size() > delay) {
			return -K_EWOULDBLOCK;
		}
		U32 blockSize = bytesPerSampleWant() * want.channels;
		len = std::min(len, ((delay - (U32)this->audioBuffer.size()) & ~(blockSize - 1)));
		audioBuffer.insert(this->audioBuffer.end(), data, data + len);
		timeSinceLastWrite = KSystem::getMilliesSinceStart();
		return len;
	}
	SDL_LockAudio();
	{
		if (this->audioBuffer.size() > delay) {
			return -K_EWOULDBLOCK;
		}
		U32 blockSize = bytesPerSampleWant() * want.channels;
		len = std::min(len, ((delay - (U32)this->audioBuffer.size()) & ~(blockSize - 1)));
		audioBuffer.insert(this->audioBuffer.end(), data, data + len);
	}
	SDL_UnlockAudio();

	return len;	
}

static U32 nextId = 0;
BOXEDWINE_MUTEX KDspAudio::mutex;
BHashTable<U32, KDspAudioWeakPtr> KDspAudio::openAudios;

KDspAudioPtr KDspAudio::createDspAudio() {
	KDspAudioPtr result = std::make_shared<KDspAudioSdl>();	
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	result->id = nextId++;
	KDspAudioWeakPtr weak = result;
	openAudios.set(result->id, weak);
	return result;
}

KDspAudio::~KDspAudio() {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	openAudios.remove(this->id);
}

void KDspAudio::iterateOpenAudio(std::function<void(KDspAudioPtr&)> callback) {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	for (auto& it : openAudios) {
		KDspAudioPtr audio = it.value.lock();
		if (audio) {
			callback(audio);
		}
	}
}

void KDspAudio::shutdown() {
	if (!KSystem::soundEnabled) {
		return;
	}
	SDL_PauseAudio(1);
	SDL_CloseAudio();
}
