package org.makineceviri.tv

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** TV açılınca katman kendiliğinden başlar (Ayarlar → "Açılışta başlat"). */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED && Prefs(context).autostart) {
            runCatching { OverlayService.start(context) }
        }
    }
}
