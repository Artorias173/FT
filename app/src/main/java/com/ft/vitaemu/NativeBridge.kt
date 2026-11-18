package com.ft.vitaemu

import android.view.Surface

/** Accès au cœur d'émulation C++ (voir app/src/main/cpp/jni_bridge.cpp). */
object NativeBridge {
    init {
        System.loadLibrary("ftvita")
    }

    external fun nativeCreate(): Long
    external fun nativeDestroy(handle: Long)
    external fun nativeLoad(handle: Long, path: String): Boolean
    external fun nativeStart(handle: Long)
    external fun nativePause(handle: Long)
    external fun nativeResume(handle: Long)
    external fun nativeStop(handle: Long)
    external fun nativeSetSurface(handle: Long, surface: Surface?, width: Int, height: Int): Boolean
    external fun nativeStatus(handle: Long): String
}
