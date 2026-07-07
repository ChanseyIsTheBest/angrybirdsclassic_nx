/* audio.h -- SDL2 pull-audio bridge to the Fusion engine's mixer
 *
 * The engine mixes its own audio: it exports
 *   Java_com_rovio_fusion_AudioOutput_nativeMixData(env, thiz, peer, byte[], count)
 * which fills a PCM byte[] on demand. On Android a Java AudioTrack thread called
 * that to feed the device. We replace that thread with an SDL audio device whose
 * callback calls nativeMixData to fill each buffer.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef __AUDIO_H__
#define __AUDIO_H__

#include "jni.h"

// register the resolved nativeMixData entry point and the AudioOutput receiver
void audio_set_mixer(void *native_mix_data_addr, void *thiz);

// call once per frame: opens the SDL device when the engine has negotiated a
// format and asked for output (fusion.c), and pauses it when it hasn't.
void audio_poll(void);

void audio_shutdown(void);

#endif
