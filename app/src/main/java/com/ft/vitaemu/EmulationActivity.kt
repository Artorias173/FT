package com.ft.vitaemu

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView

/** Écran de jeu : une SurfaceView pour le rendu et l'état de l'émulateur en surimpression. */
class EmulationActivity : Activity(), SurfaceHolder.Callback {

    private var handle = 0L
    private lateinit var statusView: TextView
    private val ui = Handler(Looper.getMainLooper())

    private val statusPoller = object : Runnable {
        override fun run() {
            if (handle != 0L) statusView.text = NativeBridge.nativeStatus(handle)
            ui.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val surface = SurfaceView(this).also { it.holder.addCallback(this) }
        statusView = TextView(this).apply {
            setTextColor(Color.WHITE)
            setShadowLayer(4f, 0f, 0f, Color.BLACK)
            setPadding(16, 16, 16, 16)
        }
        setContentView(FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(surface)
            addView(statusView)
        })
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY

        handle = NativeBridge.nativeCreate()
        val path = intent.getStringExtra(EXTRA_PATH)
        if (handle != 0L && path != null && NativeBridge.nativeLoad(handle, path)) {
            NativeBridge.nativeStart(handle)
        }
        ui.post(statusPoller)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (handle != 0L) NativeBridge.nativeSetSurface(handle, holder.surface, width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (handle != 0L) NativeBridge.nativeSetSurface(handle, null, 0, 0)
    }

    override fun onPause() {
        super.onPause()
        if (handle != 0L) NativeBridge.nativePause(handle)
    }

    override fun onResume() {
        super.onResume()
        if (handle != 0L) NativeBridge.nativeResume(handle)
    }

    override fun onDestroy() {
        ui.removeCallbacks(statusPoller)
        if (handle != 0L) {
            NativeBridge.nativeDestroy(handle)  // arrête aussi le thread d'émulation
            handle = 0L
        }
        super.onDestroy()
    }

    companion object {
        const val EXTRA_PATH = "path"
    }
}
