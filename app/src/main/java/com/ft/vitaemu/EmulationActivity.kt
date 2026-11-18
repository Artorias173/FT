package com.ft.vitaemu

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import java.io.File
import kotlin.math.roundToInt

/** Écran de jeu : une SurfaceView pour le rendu et l'état de l'émulateur en surimpression. */
class EmulationActivity : Activity(), SurfaceHolder.Callback {

    private var handle = 0L
    private var buttons = 0
    private var sticks = intArrayOf(128, 128, 128, 128)
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
        if (handle != 0L) {
            // ux0: (stockage de la console) correspond à un dossier privé de l'application.
            val ux0 = File(filesDir, "ux0").apply { File(this, "data").mkdirs() }
            NativeBridge.nativeMount(handle, "ux0:", ux0.absolutePath)
        }
        if (handle != 0L && path != null && NativeBridge.nativeLoad(handle, path)) {
            NativeBridge.nativeStart(handle)
        }
        ui.post(statusPoller)
    }

    // --- Manette : boutons SCE_CTRL_* (psp2common/ctrl.h) ----------------------

    private fun buttonFor(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> 0x4000       // Croix
        KeyEvent.KEYCODE_BUTTON_B -> 0x2000       // Rond
        KeyEvent.KEYCODE_BUTTON_X -> 0x8000       // Carré
        KeyEvent.KEYCODE_BUTTON_Y -> 0x1000       // Triangle
        KeyEvent.KEYCODE_BUTTON_L1 -> 0x0100      // L
        KeyEvent.KEYCODE_BUTTON_R1 -> 0x0200      // R
        KeyEvent.KEYCODE_BUTTON_START -> 0x0008
        KeyEvent.KEYCODE_BUTTON_SELECT -> 0x0001
        KeyEvent.KEYCODE_DPAD_UP -> 0x0010
        KeyEvent.KEYCODE_DPAD_RIGHT -> 0x0020
        KeyEvent.KEYCODE_DPAD_DOWN -> 0x0040
        KeyEvent.KEYCODE_DPAD_LEFT -> 0x0080
        else -> 0
    }

    private fun sendInput() {
        if (handle != 0L) NativeBridge.nativeSetInput(handle, buttons, sticks[0], sticks[1], sticks[2], sticks[3])
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val bit = buttonFor(event.keyCode)
        if (bit == 0) return super.dispatchKeyEvent(event)
        buttons = if (event.action == KeyEvent.ACTION_DOWN) buttons or bit else buttons and bit.inv()
        sendInput()
        return true
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK) {
            return super.dispatchGenericMotionEvent(event)
        }
        // Axes de -1 à 1 convertis vers 0..255 (128 au repos).
        fun axis(a: Int) = ((event.getAxisValue(a) + 1f) * 127.5f).roundToInt().coerceIn(0, 255)
        sticks = intArrayOf(axis(MotionEvent.AXIS_X), axis(MotionEvent.AXIS_Y), axis(MotionEvent.AXIS_Z), axis(MotionEvent.AXIS_RZ))
        sendInput()
        return true
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
