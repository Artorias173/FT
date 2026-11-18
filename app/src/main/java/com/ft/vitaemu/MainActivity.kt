package com.ft.vitaemu

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import java.io.File

/** Écran d'accueil : choix du programme à lancer. */
class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val padding = (24 * resources.displayMetrics.density).toInt()
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(padding, padding, padding, padding)
        }
        layout.addView(TextView(this).apply {
            setText(R.string.intro)
            gravity = Gravity.CENTER
        })
        layout.addView(Button(this).apply {
            setText(R.string.pick_file)
            setOnClickListener { pickFile() }
        })
        setContentView(layout)
    }

    private fun pickFile() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        @Suppress("DEPRECATION")
        startActivityForResult(intent, REQUEST_PICK)
    }

    @Deprecated("API Activity simple, sans dépendance AndroidX")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode != REQUEST_PICK || resultCode != RESULT_OK || uri == null) return

        val local = copyToCache(uri)
        if (local == null) {
            Toast.makeText(this, R.string.copy_failed, Toast.LENGTH_LONG).show()
            return
        }
        startActivity(Intent(this, EmulationActivity::class.java)
            .putExtra(EmulationActivity.EXTRA_PATH, local.absolutePath))
    }

    /** Le cœur C++ lit un chemin de fichier : on copie le document choisi en local. */
    private fun copyToCache(uri: Uri): File? = try {
        val target = File(cacheDir, "eboot.bin")
        contentResolver.openInputStream(uri)?.use { input ->
            target.outputStream().use { output -> input.copyTo(output) }
        } ?: return null
        target
    } catch (e: Exception) {
        null
    }

    private companion object {
        const val REQUEST_PICK = 1
    }
}
