/* fusion.h -- platform ("Java wrapper") implementations for the Fusion engine
 *
 * jni_fake.c routes every engine->Java method call here by (class, method,
 * signature). We implement the ones that matter for bring-up and log the rest.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef __FUSION_H__
#define __FUSION_H__

#include <stdarg.h>
#include "jni.h"

// central dispatch target for CallXxxMethod[V]. `self` is the receiver (object
// or class; usually ignored). `ap` holds the call arguments; we read them using
// the JNI `sig` (e.g. "(JIII)L..."). Returns a jvalue; the caller reads the
// field matching the call's return type.
jvalue fusion_call(const char *cls, const char *method, const char *sig,
                   void *self, va_list ap);

// --- audio format negotiated via createAudioOutput (read by audio.c) ---
int  fusion_audio_rate(void);      // samples/sec (e.g. 44100)
int  fusion_audio_channels(void);  // 1 or 2
int  fusion_audio_bits(void);      // 8 or 16
void *fusion_audio_peer(void);     // native stream ptr to echo back to nativeMixData
void fusion_set_audio_peer(void *p);
int  fusion_audio_wanted(void);    // set once the engine has created an output

#endif
