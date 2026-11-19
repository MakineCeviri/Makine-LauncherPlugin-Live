package org.makineceviri.tv

import android.content.Context
import android.os.Build
import android.provider.Settings
import java.security.SecureRandom
import java.util.UUID

/** Kalıcı ayarlar: TV kimliği, eşleşme kodu, altyazı görünümü. */
class Prefs(private val ctx: Context) {
    private val sp = ctx.getSharedPreferences("makine_tv", Context.MODE_PRIVATE)

    /** Ağda bu TV'yi tanıtan kalıcı kimlik (adres değişse de PC aynı TV'yi bulur). */
    val tvId: String
        get() = sp.getString(KEY_ID, null) ?: UUID.randomUUID().toString().also { sp.edit().putString(KEY_ID, it).apply() }

    /** Ekranda görünen 4 haneli eşleşme kodu. */
    val code: String
        get() = sp.getString(KEY_CODE, null) ?: newCode()

    fun newCode(): String {
        val c = "%04d".format(SecureRandom().nextInt(10000))
        sp.edit().putString(KEY_CODE, c).apply()
        return c
    }

    val tvName: String
        get() = Settings.Global.getString(ctx.contentResolver, "device_name")?.takeIf { it.isNotBlank() }
            ?: Build.MODEL ?: "Android TV"

    /** 0 küçük · 1 orta · 2 büyük */
    var size: Int
        get() = sp.getInt("size", 1).coerceIn(0, 2)
        set(v) = sp.edit().putInt("size", v.coerceIn(0, 2)).apply()

    /** "alt" | "ust" */
    var position: String
        get() = if (sp.getString("position", "alt") == "ust") "ust" else "alt"
        set(v) = sp.edit().putString("position", if (v == "ust") "ust" else "alt").apply()

    /** Şerit opaklığı, yüzde */
    var opacity: Int
        get() = sp.getInt("opacity", 78).coerceIn(40, 100)
        set(v) = sp.edit().putInt("opacity", v.coerceIn(40, 100)).apply()

    var autostart: Boolean
        get() = sp.getBoolean("autostart", true)
        set(v) = sp.edit().putBoolean("autostart", v).apply()

    companion object {
        private const val KEY_ID = "tv_id"
        private const val KEY_CODE = "code"
        val SIZE_LABELS = listOf("Küçük", "Orta", "Büyük")
        val OPACITY_STEPS = listOf(60, 78, 90)
    }
}
