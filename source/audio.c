/* audio.c -- SDL2 pull-audio bridge to the Fusion engine's mixer. See audio.h.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL2/SDL.h>

#include "jni.h"
#include "jni_fake.h"
#include "fusion.h"
#include "audio.h"
#include "util.h"

// nativeMixData(env, thiz, peer, jbyteArray buffer, jint count)
typedef jint (*fn_mixdata)(JNIEnv env, void *thiz, void *peer, jbyteArray buf, jint count);

static fn_mixdata s_mix = NULL;
static void      *s_thiz = NULL;
static SDL_AudioDeviceID s_dev = 0;
static int s_open = 0;
static int s_frame_bytes = 4; // channels * bytes-per-sample, set on open

// The unit of nativeMixData's `count` arg is not documented. On Android the
// AudioOutput passed the byte[]'s length, so we default to BYTES. If audio comes
// out at the wrong speed/pitch, try FRAMES (len / frame_bytes) or SAMPLES
// (len / bytes-per-sample) instead -- flip this and rebuild.
#define AUDIO_COUNT_BYTES  0
#define AUDIO_COUNT_FRAMES 1
#define AUDIO_COUNT_SAMPLES 2
#define AUDIO_COUNT_UNIT AUDIO_COUNT_BYTES

static void SDLCALL audio_cb(void *user, Uint8 *stream, int len) {
  (void)user;
  void *peer = fusion_audio_peer();
  if (!s_mix || !peer) { memset(stream, 0, len); return; } // no peer yet -> silence

  static int announced = 0;
  if (!announced) { debugPrintf("audio: first mix, peer=%p len=%d\n", peer, len); announced = 1; }

  jint count;
  switch (AUDIO_COUNT_UNIT) {
    case AUDIO_COUNT_FRAMES:  count = len / (s_frame_bytes ? s_frame_bytes : 1); break;
    case AUDIO_COUNT_SAMPLES: count = len / ((fusion_audio_bits() / 8) ? (fusion_audio_bits() / 8) : 1); break;
    default:                  count = len; break;
  }

  jbyteArray arr = jni_wrap_bytearray(stream, len);
  s_mix(fake_env, s_thiz, peer, arr, count);
  jni_free_wrapper(arr);
}

void audio_set_mixer(void *native_mix_data_addr, void *thiz) {
  s_mix  = (fn_mixdata)native_mix_data_addr;
  s_thiz = thiz;
  debugPrintf("audio: mixer=%p thiz=%p\n", native_mix_data_addr, thiz);
}

static void audio_open(void) {
  int rate = fusion_audio_rate();
  int ch   = fusion_audio_channels();
  int bits = fusion_audio_bits();

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq     = rate;
  want.format   = (bits == 8) ? AUDIO_U8 : AUDIO_S16SYS;
  want.channels = (Uint8)ch;
  want.samples  = 1024;                 // ~23 ms at 44100; latency vs. underrun
  want.callback = audio_cb;

  s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                              SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!s_dev) {
    debugPrintf("audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    return;
  }
  s_frame_bytes = have.channels * (SDL_AUDIO_BITSIZE(have.format) / 8);
  s_open = 1;
  SDL_PauseAudioDevice(s_dev, 0); // start
  debugPrintf("audio: opened %d Hz, %d ch, fmt=0x%x, frame=%d bytes\n",
              have.freq, have.channels, have.format, s_frame_bytes);
}

void audio_poll(void) {
  // Open only once we have the peer: its struct carries the exact format, and
  // the mixer needs it anyway. This also avoids opening at a guessed format.
  if (!s_open && fusion_audio_wanted() && s_mix && fusion_audio_peer())
    audio_open();
  else if (s_open && s_dev)
    SDL_PauseAudioDevice(s_dev, fusion_audio_wanted() ? 0 : 1);
}

void audio_shutdown(void) {
  if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
  s_open = 0;
}
