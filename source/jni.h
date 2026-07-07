/* jni.h -- minimal JNI ABI for the Fusion engine wrapper
 *
 * devkitPro ships no jni.h. The Fusion engine calls JNI functions purely by
 * fixed table offset (it does `ldr xN,[env]; ldr xN,[xN,#slot*8]; blr xN`), so
 * what matters is that our JNINativeInterface has the JNI-1.6 slot layout. We
 * therefore model the interface as a flat array of function pointers indexed by
 * the JNI-spec slot number (see jni_slots.h, generated + checked against the
 * engine's own offsets). Implemented slots are filled by name-index; the rest
 * are filled with a logging catch-all in jni_fake.c.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef __JNI_H__
#define __JNI_H__

#include <stdint.h>
#include "jni_slots.h"

// --- primitive types (LP64 / AArch64) ---
typedef int32_t   jint;
typedef int64_t   jlong;
typedef int8_t    jbyte;
typedef uint8_t   jboolean;
typedef uint16_t  jchar;
typedef int16_t   jshort;
typedef float     jfloat;
typedef double    jdouble;
typedef jint      jsize;

// --- reference types (opaque; our fake objects are tagged heap blocks) ---
typedef void *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jarray;
typedef jobject jthrowable;
typedef jarray  jbyteArray;
typedef jarray  jintArray;
typedef jarray  jshortArray;
typedef jarray  jfloatArray;
typedef jarray  jobjectArray;
typedef jarray  jbooleanArray;
typedef jarray  jcharArray;
typedef jarray  jlongArray;
typedef jarray  jdoubleArray;

typedef void *jmethodID;
typedef void *jfieldID;
typedef void *jweak;

typedef union jvalue {
  jboolean z; jbyte b; jchar c; jshort s; jint i; jlong j; jfloat f; jdouble d; jobject l;
} jvalue;

typedef struct {
  const char *name;
  const char *signature;
  void *fnPtr;
} JNINativeMethod;

// --- the flat interface table (slot i == JNI-spec function i) ---
//
// IMPORTANT ABI NOTE: the engine (compiled C++ against Android's jni.h) treats
// JNIEnv as a pointer to a struct whose first field is the function-table
// pointer, i.e. it does two dereferences to reach a function:
//     table    = *(void**)env;          // ldr x, [env]
//     fn       = table[slot];           // ldr x, [x, #slot*8]
//     fn(env, ...)
// (confirmed by disassembly of nativeMixData). JNIEnv is therefore a POINTER TO
// the table pointer -- two levels -- not the table pointer itself.
typedef struct JNINativeInterface {
  void *fn[JNI_SLOT_COUNT];
} JNINativeInterface;
typedef const struct JNINativeInterface  *JNITable;   // the "functions" pointer
typedef const JNITable                   *JNIEnv;     // env == &functions (two-level)

// --- the invoke interface (JavaVM). Same two-level rule; GetEnv is slot 6. ---
typedef struct JNIInvokeInterface {
  void *reserved0, *reserved1, *reserved2;
  jint (*DestroyJavaVM)(void *vm);
  jint (*AttachCurrentThread)(void *vm, void **penv, void *args);
  jint (*DetachCurrentThread)(void *vm);
  jint (*GetEnv)(void *vm, void **penv, jint version);
  jint (*AttachCurrentThreadAsDaemon)(void *vm, void **penv, void *args);
} JNIInvokeInterface;
typedef const struct JNIInvokeInterface  *JNIInvokeTable;
typedef const JNIInvokeTable             *JavaVM;      // vm == &invoke (two-level)

// --- constants ---
#define JNI_OK           0
#define JNI_ERR         (-1)
#define JNI_EDETACHED   (-2)
#define JNI_EVERSION    (-3)
#define JNI_FALSE        0
#define JNI_TRUE         1
#define JNI_VERSION_1_2  0x00010002
#define JNI_VERSION_1_4  0x00010004
#define JNI_VERSION_1_6  0x00010006
#define JNI_COMMIT       1
#define JNI_ABORT        2

#endif
