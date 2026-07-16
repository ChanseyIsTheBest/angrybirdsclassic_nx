/* jni_fake.c -- fake JNI environment for the Rovio Fusion engine
 *
 * See jni_fake.h. The interface is a flat array of function pointers indexed by
 * JNI-spec slot (jni.h / jni_slots.h). We implement the structural functions
 * the engine relies on (FindClass, Get*MethodID, the Call* families, strings,
 * byte/int arrays, refs, exceptions, GetJavaVM, RegisterNatives) and fill every
 * remaining slot with a logging catch-all so an unexpected call is visible in
 * the log rather than a jump through NULL. Method calls are routed by name into
 * fusion.c, which implements the platform behaviour (audio, device, fonts, ...).
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "jni.h"
#include "jni_fake.h"
#include "util.h"
#include "fusion.h"

JavaVM fake_vm = NULL;
JNIEnv fake_env = NULL;
volatile int g_kbd_requested = 0;
char g_kbd_initial[64] = {0};
volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

typedef enum { T_GENERIC, T_STRING, T_CLASS, T_BYTEARRAY, T_INTARRAY, T_OBJARRAY } Tag;

typedef struct FakeObj {
  Tag tag;
  // string
  char *str;
  // arrays
  void *data;       // element storage
  int   len;        // element count
  int   owns_data;  // free data on release?
  jobject *objs;    // for object arrays
  // class
  const char *cls;  // interned class name (for T_CLASS)
} FakeObj;

static FakeObj *obj_new(Tag t) {
  FakeObj *o = calloc(1, sizeof(*o));
  if (o) o->tag = t;
  return o;
}

// --- class registry: intern class names so the same name -> same handle ---
#define MAX_CLASSES 64
static FakeObj *g_classes[MAX_CLASSES];
static int g_nclasses = 0;

static jclass find_or_make_class(const char *name) {
  for (int i = 0; i < g_nclasses; i++)
    if (strcmp(g_classes[i]->cls, name) == 0)
      return g_classes[i];
  if (g_nclasses >= MAX_CLASSES) return g_classes[0];
  FakeObj *c = obj_new(T_CLASS);
  c->cls = strdup(name);
  g_classes[g_nclasses++] = c;
  return c;
}

// --- method registry: (class,name,sig) -> stable token, so cached IDs work ---
typedef struct { const char *cls; char *name; char *sig; } FakeMethod;
#define MAX_METHODS 512
static FakeMethod *g_methods[MAX_METHODS];
static int g_nmethods = 0;

static jmethodID find_or_make_method(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < g_nmethods; i++)
    if (g_methods[i]->cls == cls && strcmp(g_methods[i]->name, name) == 0)
      return g_methods[i];
  if (g_nmethods >= MAX_METHODS) return g_methods[0];
  FakeMethod *m = calloc(1, sizeof(*m));
  m->cls = cls; m->name = strdup(name); m->sig = strdup(sig ? sig : "");
  g_methods[g_nmethods++] = m;
  return m;
}

// a shared, never-freed "field" token (fields are rarely read by the engine)
static int g_field_token;

// ---------------------------------------------------------------------------
// JNI function implementations
// ---------------------------------------------------------------------------

static jint jf_GetVersion(JNIEnv e) { (void)e; return JNI_VERSION_1_6; }

static jclass jf_FindClass(JNIEnv e, const char *name) {
  (void)e;
  return find_or_make_class(name ? name : "?");
}

static jmethodID jf_GetMethodID(JNIEnv e, jclass c, const char *name, const char *sig) {
  (void)e;
  FakeObj *cl = c;
  const char *cn = (cl && cl->tag == T_CLASS) ? cl->cls : "?";
  return find_or_make_method(cn, name ? name : "?", sig);
}
static jmethodID jf_GetStaticMethodID(JNIEnv e, jclass c, const char *name, const char *sig) {
  return jf_GetMethodID(e, c, name, sig);
}
static jfieldID jf_GetFieldID(JNIEnv e, jclass c, const char *n, const char *s) {
  (void)e; (void)c; (void)n; (void)s; return &g_field_token;
}
static jfieldID jf_GetStaticFieldID(JNIEnv e, jclass c, const char *n, const char *s) {
  (void)e; (void)c; (void)n; (void)s; return &g_field_token;
}

// --- strings ---
static jstring jf_NewStringUTF(JNIEnv e, const char *bytes) {
  (void)e;
  FakeObj *o = obj_new(T_STRING);
  o->str = strdup(bytes ? bytes : "");
  return o;
}
static int looks_like_locale(const char *s) {
  if (!s) return 0;
  size_t n = strlen(s);
  if (n == 1 && s[0] == 'C') return 1;                       // the C locale
  if (n >= 2 && n <= 6) {                                    // "fr", "fr_FR", "es_419"
    if ((s[0] >= 'a' && s[0] <= 'z') && (s[1] >= 'a' && s[1] <= 'z')) return 1;
  }
  return 0;
}
static const char *jf_GetStringUTFChars(JNIEnv e, jstring s, jboolean *isCopy) {
  (void)e;
  FakeObj *o = s;
  if (isCopy) *isCopy = JNI_FALSE;
  const char *r = (o && o->tag == T_STRING) ? o->str : "";
  if (looks_like_locale(r)) {                                // trace locale reads
    static int n = 0;
    if (n < 40) { debugPrintf("GetStringUTFChars locale-ish: \"%s\"\n", r); n++; }
  }
  return r;
}
static void jf_ReleaseStringUTFChars(JNIEnv e, jstring s, const char *c) {
  (void)e; (void)s; (void)c; // chars point straight into the FakeObj; nothing to free
}
static jsize jf_GetStringUTFLength(JNIEnv e, jstring s) {
  (void)e; FakeObj *o = s;
  return (o && o->str) ? (jsize)strlen(o->str) : 0;
}
static jsize jf_GetStringLength(JNIEnv e, jstring s) {
  return jf_GetStringUTFLength(e, s); // ASCII paths/locales: char count == byte count
}
// UTF-16 variants: the engine occasionally uses these for font/text work. We
// back them with a per-string malloc'd jchar buffer (freed on release).
static const jchar *jf_GetStringChars(JNIEnv e, jstring s, jboolean *isCopy) {
  (void)e; FakeObj *o = s;
  const char *u = (o && o->str) ? o->str : "";
  size_t n = strlen(u);
  jchar *w = malloc((n + 1) * sizeof(jchar));
  for (size_t i = 0; i < n; i++) w[i] = (jchar)(unsigned char)u[i];
  w[n] = 0;
  if (isCopy) *isCopy = JNI_TRUE;
  return w;
}
static void jf_ReleaseStringChars(JNIEnv e, jstring s, const jchar *c) {
  (void)e; (void)s; free((void *)c);
}
static jstring jf_NewString(JNIEnv e, const jchar *u, jsize len) {
  (void)e;
  FakeObj *o = obj_new(T_STRING);
  o->str = malloc(len + 1);
  for (jsize i = 0; i < len; i++) o->str[i] = (char)u[i];
  o->str[len] = 0;
  return o;
}

// --- arrays ---
static jsize jf_GetArrayLength(JNIEnv e, jarray a) {
  (void)e; FakeObj *o = a; return o ? (jsize)o->len : 0;
}
static jbyteArray jf_NewByteArray(JNIEnv e, jsize len) {
  (void)e;
  FakeObj *o = obj_new(T_BYTEARRAY);
  o->len = len; o->data = calloc(len > 0 ? len : 1, 1); o->owns_data = 1;
  return o;
}
static jintArray jf_NewIntArray(JNIEnv e, jsize len) {
  (void)e;
  FakeObj *o = obj_new(T_INTARRAY);
  o->len = len; o->data = calloc(len > 0 ? len : 1, sizeof(jint)); o->owns_data = 1;
  return o;
}
static jbyte *jf_GetByteArrayElements(JNIEnv e, jbyteArray a, jboolean *isCopy) {
  (void)e; FakeObj *o = a;
  if (isCopy) *isCopy = JNI_FALSE;
  return o ? (jbyte *)o->data : NULL;   // direct pointer: the engine fills PCM here
}
static void jf_ReleaseByteArrayElements(JNIEnv e, jbyteArray a, jbyte *p, jint mode) {
  (void)e; (void)a; (void)p; (void)mode; // data is the array's own storage; nothing to copy back
}
static jint *jf_GetIntArrayElements(JNIEnv e, jintArray a, jboolean *isCopy) {
  (void)e; FakeObj *o = a; if (isCopy) *isCopy = JNI_FALSE;
  return o ? (jint *)o->data : NULL;
}
static void jf_ReleaseIntArrayElements(JNIEnv e, jintArray a, jint *p, jint m) {
  (void)e; (void)a; (void)p; (void)m;
}
static void *jf_GetPrimitiveArrayCritical(JNIEnv e, jarray a, jboolean *isCopy) {
  (void)e; FakeObj *o = a; if (isCopy) *isCopy = JNI_FALSE;
  return o ? o->data : NULL;
}
static void jf_ReleasePrimitiveArrayCritical(JNIEnv e, jarray a, void *p, jint m) {
  (void)e; (void)a; (void)p; (void)m;
}
static jobjectArray jf_NewObjectArray(JNIEnv e, jsize len, jclass cls, jobject init) {
  (void)e; (void)cls;
  FakeObj *o = obj_new(T_OBJARRAY);
  o->len = len; o->objs = calloc(len > 0 ? len : 1, sizeof(jobject));
  for (jsize i = 0; i < len; i++) o->objs[i] = init;
  return o;
}
static jobject jf_GetObjectArrayElement(JNIEnv e, jobjectArray a, jsize i) {
  (void)e; FakeObj *o = a;
  return (o && o->objs && i >= 0 && i < o->len) ? o->objs[i] : NULL;
}
static void jf_SetObjectArrayElement(JNIEnv e, jobjectArray a, jsize i, jobject v) {
  (void)e; FakeObj *o = a;
  if (o && o->objs && i >= 0 && i < o->len) o->objs[i] = v;
}

// --- objects / refs / exceptions ---
static jclass jf_GetObjectClass(JNIEnv e, jobject o) {
  (void)e; FakeObj *f = o;
  if (f && f->tag == T_CLASS) return f;
  return find_or_make_class("java/lang/Object");
}
static jboolean jf_IsInstanceOf(JNIEnv e, jobject o, jclass c) { (void)e;(void)o;(void)c; return JNI_TRUE; }
static jboolean jf_IsSameObject(JNIEnv e, jobject a, jobject b) { (void)e; return a == b; }
static jobject jf_NewRef(JNIEnv e, jobject o) { (void)e; return o; }     // global/local/weak all identity
static void    jf_DeleteRef(JNIEnv e, jobject o) { (void)e; (void)o; }   // leak; engine may retain refs
static jint    jf_EnsureLocalCapacity(JNIEnv e, jint n) { (void)e;(void)n; return 0; }
static jint    jf_PushLocalFrame(JNIEnv e, jint n) { (void)e;(void)n; return 0; }
static jobject jf_PopLocalFrame(JNIEnv e, jobject r) { (void)e; return r; }
static jobject jf_AllocObject(JNIEnv e, jclass c) { (void)e;(void)c; return obj_new(T_GENERIC); }
// NewObject(clazz, ctor, ...) -- we don't run constructors, but the object must
// be non-NULL or the engine treats it as "NewObject failed" and throws. Return
// a generic instance; any methods later called on it route through the catch-all.
//
// Special case: the engine builds its Java AudioOutput by passing the native
// mixer pointer to the constructor; that pointer is exactly what nativeMixData
// needs back as its "peer". Capture it here. A real Switch pointer is a large
// value, so we ignore small ctor args (sample rate, channel count, ...).
static jobject new_object_common(jclass c, va_list ap) {
  FakeObj *cl = c;
  const char *cn = (cl && cl->tag == T_CLASS) ? cl->cls : "?";
  if (cl && cl->tag == T_CLASS && strstr(cl->cls, "AudioOutput")) {
    unsigned long a0 = (unsigned long)va_arg(ap, unsigned long);
    debugPrintf("NewObject(%s) arg0=%#lx\n", cn, a0);
    if (a0 > 0x100000UL && a0 < 0x8000000000UL)   // plausible pointer, not an int arg
      fusion_set_audio_peer((void *)(uintptr_t)a0);
  } else {
    debugPrintf("NewObject(%s)\n", cn);
  }
  return obj_new(T_GENERIC);
}
static jobject jf_NewObject(JNIEnv e, jclass c, jmethodID m, ...) {
  (void)e;(void)m; va_list ap; va_start(ap, m);
  jobject o = new_object_common(c, ap); va_end(ap); return o;
}
static jobject jf_NewObjectV(JNIEnv e, jclass c, jmethodID m, va_list ap) {
  (void)e;(void)m; return new_object_common(c, ap);
}
static jobject jf_NewObjectA(JNIEnv e, jclass c, jmethodID m, void *av) {
  (void)e;(void)m;
  FakeObj *cl = c;
  const char *cn = (cl && cl->tag == T_CLASS) ? cl->cls : "?";
  if (cl && cl->tag == T_CLASS && strstr(cl->cls, "AudioOutput") && av) {
    unsigned long a0 = *(unsigned long *)av;  // jvalue[0]
    debugPrintf("NewObjectA(%s) arg0=%#lx\n", cn, a0);
    if (a0 > 0x100000UL && a0 < 0x8000000000UL)
      fusion_set_audio_peer((void *)(uintptr_t)a0);
  }
  return obj_new(T_GENERIC);
}
static jthrowable jf_ExceptionOccurred(JNIEnv e) { (void)e; return NULL; }
static void    jf_ExceptionClear(JNIEnv e) { (void)e; }
static void    jf_ExceptionDescribe(JNIEnv e) { (void)e; }
static jboolean jf_ExceptionCheck(JNIEnv e) { (void)e; return JNI_FALSE; }
static jint    jf_MonitorOp(JNIEnv e, jobject o) { (void)e; (void)o; return 0; }

static jint jf_GetJavaVM(JNIEnv e, JavaVM *vm) { (void)e; if (vm) *vm = fake_vm; return JNI_OK; }

static jint jf_RegisterNatives(JNIEnv e, jclass c, const JNINativeMethod *m, jint n) {
  (void)e;
  FakeObj *cl = c;
  debugPrintf("JNI RegisterNatives(%s, %d methods) -> ok\n",
              (cl && cl->tag == T_CLASS) ? cl->cls : "?", n);
  // We resolve the engine's exported Java_* symbols directly, so we don't need
  // the table; just acknowledge it. (Left here as the hook if a build ever
  // registers a native that isn't also exported.)
  (void)m;
  return JNI_OK;
}
static jint jf_UnregisterNatives(JNIEnv e, jclass c) { (void)e;(void)c; return JNI_OK; }

// ---------------------------------------------------------------------------
// method-call dispatch: read the method token, hand it to fusion.c
// ---------------------------------------------------------------------------

static jvalue call_v(JNIEnv e, jobject self, jmethodID mid, va_list ap) {
  (void)e;
  jvalue r; r.j = 0;
  FakeMethod *m = mid;
  if (!m) return r;
  return fusion_call(m->cls, m->name, m->sig, self, ap);
}

// vararg forms build a va_list and forward; the "A" (jvalue*) forms are rare in
// precompiled NDK C++ and route to the catch-all.
#define CALL_FORMS(RT, FIELD, PREFIX)                                          \
  static RT jf_##PREFIX(JNIEnv e, jobject o, jmethodID m, ...) {                \
    va_list ap; va_start(ap, m); jvalue v = call_v(e, o, m, ap); va_end(ap);   \
    return v.FIELD;                                                            \
  }                                                                            \
  static RT jf_##PREFIX##V(JNIEnv e, jobject o, jmethodID m, va_list ap) {      \
    jvalue v = call_v(e, o, m, ap); return v.FIELD;                            \
  }

CALL_FORMS(jobject,  l, CallObjectMethod)
CALL_FORMS(jboolean, z, CallBooleanMethod)
CALL_FORMS(jint,     i, CallIntMethod)
CALL_FORMS(jlong,    j, CallLongMethod)
CALL_FORMS(jfloat,   f, CallFloatMethod)
// void: dispatch but ignore the return
static void jf_CallVoidMethod(JNIEnv e, jobject o, jmethodID m, ...) {
  va_list ap; va_start(ap, m); call_v(e, o, m, ap); va_end(ap);
}
static void jf_CallVoidMethodV(JNIEnv e, jobject o, jmethodID m, va_list ap) {
  call_v(e, o, m, ap);
}
// static variants share the same dispatch (self is the class, ignored by fusion)
CALL_FORMS(jobject,  l, CallStaticObjectMethod)
CALL_FORMS(jboolean, z, CallStaticBooleanMethod)
CALL_FORMS(jint,     i, CallStaticIntMethod)
CALL_FORMS(jlong,    j, CallStaticLongMethod)
CALL_FORMS(jfloat,   f, CallStaticFloatMethod)
static void jf_CallStaticVoidMethod(JNIEnv e, jobject o, jmethodID m, ...) {
  va_list ap; va_start(ap, m); call_v(e, o, m, ap); va_end(ap);
}
static void jf_CallStaticVoidMethodV(JNIEnv e, jobject o, jmethodID m, va_list ap) {
  call_v(e, o, m, ap);
}

// ---------------------------------------------------------------------------
// catch-all for every slot we didn't wire (keeps stray calls non-fatal)
// ---------------------------------------------------------------------------
static jlong jni_catch_all(void) { return 0; }

// ---------------------------------------------------------------------------
// JavaVM invoke interface
// ---------------------------------------------------------------------------
static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm; (void)version;
  if (penv) *penv = (void *)fake_env;
  return JNI_OK;
}
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args;
  if (penv) *penv = (void *)fake_env; // hand back the (two-level) env
  return JNI_OK;
}
static jint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static jint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }

static JNINativeInterface s_iface;
static JNIInvokeInterface s_invoke;
static const JNINativeInterface *s_env_ptr;
static const JNIInvokeInterface *s_vm_ptr;

void jni_init(void) {
  // fill every slot with the catch-all first, then override the real ones
  for (int i = 0; i < JNI_SLOT_COUNT; i++)
    s_iface.fn[i] = (void *)jni_catch_all;

  #define SET(name, func) s_iface.fn[J_##name] = (void *)(func)
  SET(GetVersion,               jf_GetVersion);
  SET(FindClass,                jf_FindClass);
  SET(GetMethodID,              jf_GetMethodID);
  SET(GetStaticMethodID,        jf_GetStaticMethodID);
  SET(GetFieldID,               jf_GetFieldID);
  SET(GetStaticFieldID,         jf_GetStaticFieldID);

  SET(NewStringUTF,             jf_NewStringUTF);
  SET(GetStringUTFChars,        jf_GetStringUTFChars);
  SET(ReleaseStringUTFChars,    jf_ReleaseStringUTFChars);
  SET(GetStringUTFLength,       jf_GetStringUTFLength);
  SET(GetStringLength,          jf_GetStringLength);
  SET(NewString,                jf_NewString);
  SET(GetStringChars,           jf_GetStringChars);
  SET(ReleaseStringChars,       jf_ReleaseStringChars);

  SET(GetArrayLength,           jf_GetArrayLength);
  SET(NewByteArray,             jf_NewByteArray);
  SET(NewIntArray,              jf_NewIntArray);
  SET(GetByteArrayElements,     jf_GetByteArrayElements);
  SET(ReleaseByteArrayElements, jf_ReleaseByteArrayElements);
  SET(GetPrimitiveArrayCritical, jf_GetPrimitiveArrayCritical);
  SET(ReleasePrimitiveArrayCritical, jf_ReleasePrimitiveArrayCritical);
  SET(NewObjectArray,           jf_NewObjectArray);
  SET(GetObjectArrayElement,    jf_GetObjectArrayElement);
  SET(SetObjectArrayElement,    jf_SetObjectArrayElement);

  SET(GetObjectClass,           jf_GetObjectClass);
  SET(IsInstanceOf,             jf_IsInstanceOf);
  SET(IsSameObject,             jf_IsSameObject);
  SET(NewGlobalRef,             jf_NewRef);
  SET(NewLocalRef,              jf_NewRef);
  SET(NewWeakGlobalRef,         jf_NewRef);
  SET(DeleteGlobalRef,          jf_DeleteRef);
  SET(DeleteLocalRef,           jf_DeleteRef);
  SET(DeleteWeakGlobalRef,      jf_DeleteRef);
  SET(EnsureLocalCapacity,      jf_EnsureLocalCapacity);
  SET(PushLocalFrame,           jf_PushLocalFrame);
  SET(PopLocalFrame,            jf_PopLocalFrame);
  SET(AllocObject,              jf_AllocObject);
  SET(NewObject,                jf_NewObject);
  SET(NewObjectV,               jf_NewObjectV);
  SET(NewObjectA,               jf_NewObjectA);
  SET(ExceptionOccurred,        jf_ExceptionOccurred);
  SET(ExceptionClear,           jf_ExceptionClear);
  SET(ExceptionDescribe,        jf_ExceptionDescribe);
  SET(ExceptionCheck,           jf_ExceptionCheck);
  SET(MonitorEnter,             jf_MonitorOp);
  SET(MonitorExit,              jf_MonitorOp);
  SET(GetJavaVM,                jf_GetJavaVM);
  SET(RegisterNatives,          jf_RegisterNatives);
  SET(UnregisterNatives,        jf_UnregisterNatives);

  SET(CallObjectMethod,   jf_CallObjectMethod);   SET(CallObjectMethodV, jf_CallObjectMethodV);
  SET(CallBooleanMethod,  jf_CallBooleanMethod);  SET(CallBooleanMethodV, jf_CallBooleanMethodV);
  SET(CallIntMethod,      jf_CallIntMethod);      SET(CallIntMethodV, jf_CallIntMethodV);
  SET(CallLongMethod,     jf_CallLongMethod);     SET(CallLongMethodV, jf_CallLongMethodV);
  SET(CallFloatMethod,    jf_CallFloatMethod);    SET(CallFloatMethodV, jf_CallFloatMethodV);
  SET(CallVoidMethod,     jf_CallVoidMethod);     SET(CallVoidMethodV, jf_CallVoidMethodV);
  SET(CallStaticObjectMethod,  jf_CallStaticObjectMethod);  SET(CallStaticObjectMethodV, jf_CallStaticObjectMethodV);
  SET(CallStaticBooleanMethod, jf_CallStaticBooleanMethod); SET(CallStaticBooleanMethodV, jf_CallStaticBooleanMethodV);
  SET(CallStaticIntMethod,     jf_CallStaticIntMethod);     SET(CallStaticIntMethodV, jf_CallStaticIntMethodV);
  SET(CallStaticLongMethod,    jf_CallStaticLongMethod);    SET(CallStaticLongMethodV, jf_CallStaticLongMethodV);
  SET(CallStaticFloatMethod,   jf_CallStaticFloatMethod);   SET(CallStaticFloatMethodV, jf_CallStaticFloatMethodV);
  SET(CallStaticVoidMethod,    jf_CallStaticVoidMethod);    SET(CallStaticVoidMethodV, jf_CallStaticVoidMethodV);

  SET(GetIntArrayElements,     jf_GetIntArrayElements);
  SET(ReleaseIntArrayElements, jf_ReleaseIntArrayElements);
  #undef SET

  s_env_ptr = &s_iface;
  fake_env  = (JNIEnv)&s_env_ptr;   // env points at the table pointer (two-level)

  s_invoke.reserved0 = s_invoke.reserved1 = s_invoke.reserved2 = NULL;
  s_invoke.DestroyJavaVM = vm_DestroyJavaVM;
  s_invoke.AttachCurrentThread = vm_AttachCurrentThread;
  s_invoke.DetachCurrentThread = vm_DetachCurrentThread;
  s_invoke.GetEnv = vm_GetEnv;
  s_invoke.AttachCurrentThreadAsDaemon = vm_AttachCurrentThread;
  s_vm_ptr = &s_invoke;
  fake_vm  = (JavaVM)&s_vm_ptr;   // vm points at the invoke-table pointer (two-level)

  debugPrintf("jni_init: env=%p vm=%p slots=%d\n", (void*)fake_env, (void*)fake_vm, JNI_SLOT_COUNT);
}

// ---------------------------------------------------------------------------
// helpers used by main.c / audio.c
// ---------------------------------------------------------------------------

void *jni_make_thiz(void) {
  // a generic non-NULL object to stand in for the Activity/Renderer instance
  return obj_new(T_GENERIC);
}

jstring jni_make_string(const char *utf) { return jf_NewStringUTF(fake_env, utf); }

jclass jni_find_class_c(const char *name) { return find_or_make_class(name); }

jbyteArray jni_wrap_bytearray(void *data, int len_bytes) {
  FakeObj *o = obj_new(T_BYTEARRAY);
  o->data = data; o->len = len_bytes; o->owns_data = 0; // external buffer
  return o;
}

void jni_free_wrapper(jobject o) {
  FakeObj *f = o;
  if (!f) return;
  if (f->owns_data) free(f->data);
  free(f->str);
  free(f->objs);
  free(f);
}
