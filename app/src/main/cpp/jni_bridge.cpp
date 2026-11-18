// Pont JNI entre l'interface Kotlin (NativeBridge.kt) et le cœur C++.
#include <android/native_window_jni.h>
#include <jni.h>

#include <memory>
#include <string>

#include "vita/emulator.h"

namespace {

struct Session {
    vita::Emulator emulator;
    ANativeWindow* window = nullptr;

    ~Session() {
        emulator.stop();
        if (window) ANativeWindow_release(window);
    }
};

Session* from_handle(jlong handle) { return reinterpret_cast<Session*>(handle); }

std::string to_string(JNIEnv* env, jstring s) {
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(s, chars);
    return result;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeCreate(JNIEnv*, jobject) {
    auto session = std::make_unique<Session>();
    if (!session->emulator.init()) return 0;
    return reinterpret_cast<jlong>(session.release());
}

JNIEXPORT void JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeDestroy(JNIEnv*, jobject, jlong handle) {
    delete from_handle(handle);
}

JNIEXPORT jboolean JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeLoad(JNIEnv* env, jobject, jlong handle, jstring path) {
    return from_handle(handle)->emulator.load(to_string(env, path)).ok();
}

JNIEXPORT void JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeStart(JNIEnv*, jobject, jlong handle) {
    from_handle(handle)->emulator.start();
}

JNIEXPORT void JNICALL
Java_com_ft_vitaemu_NativeBridge_nativePause(JNIEnv*, jobject, jlong handle) {
    from_handle(handle)->emulator.pause();
}

JNIEXPORT void JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeResume(JNIEnv*, jobject, jlong handle) {
    from_handle(handle)->emulator.resume();
}

JNIEXPORT void JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeStop(JNIEnv*, jobject, jlong handle) {
    from_handle(handle)->emulator.stop();
}

JNIEXPORT jboolean JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeSetSurface(JNIEnv* env, jobject, jlong handle,
                                                  jobject surface, jint width, jint height) {
    Session* s = from_handle(handle);
    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = nullptr;
    }
    if (!surface) return JNI_TRUE;  // surface détruite
    s->window = ANativeWindow_fromSurface(env, surface);
    return s->emulator.attach_surface(s->window, static_cast<vita::u32>(width),
                                      static_cast<vita::u32>(height));
}

JNIEXPORT jstring JNICALL
Java_com_ft_vitaemu_NativeBridge_nativeStatus(JNIEnv* env, jobject, jlong handle) {
    return env->NewStringUTF(from_handle(handle)->emulator.status().c_str());
}

}  // extern "C"
