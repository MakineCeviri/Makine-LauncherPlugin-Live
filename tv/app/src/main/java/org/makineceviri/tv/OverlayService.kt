package org.makineceviri.tv

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.WindowManager
import org.json.JSONObject

/**
 * Arka planda çalışan altyazı katmanı. TV başka bir girişe (PS5'in HDMI'ı) geçse de çalışır:
 * PC'den gelen satırı TYPE_APPLICATION_OVERLAY penceresinde, görüntünün üstünde gösterir.
 * Katman izni (SYSTEM_ALERT_WINDOW) yoksa bağlantı yine kurulur, ama altyazı yalnızca uygulama öndeyken görünmez;
 * PC'deki "TV'ye kur" izni kurulum sırasında verir.
 */
class OverlayService : Service(), LinkServer.Listener {

    data class Status(val pc: String?, val overlay: Boolean, val port: Int)

    private val main = Handler(Looper.getMainLooper())
    private lateinit var prefs: Prefs
    private lateinit var link: LinkServer
    private var view: SubtitleView? = null
    private var params: WindowManager.LayoutParams? = null
    private var passThrough = true
    private var multicast: WifiManager.MulticastLock? = null
    private var pc: String? = null
    private val hideSample = Runnable { view?.hide() }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        prefs = Prefs(this)
        startInForeground()
        // Bazı Wi-Fi yongaları ekran kapalıyken / kilitsiz yayın paketlerini atar: keşif yanıtı için kilit
        multicast = (applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager)
            ?.createMulticastLock("makine-tv")?.apply { setReferenceCounted(false); runCatching { acquire() } }
        link = LinkServer(prefs, this)
        link.start()
        ensureOverlay()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun onDestroy() {
        instance = null
        link.stop()
        view?.let { runCatching { (getSystemService(WINDOW_SERVICE) as WindowManager).removeView(it) } }
        view = null
        params = null
        runCatching { multicast?.release() }
        publish()
        super.onDestroy()
    }

    private fun startInForeground() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.channel_name), NotificationManager.IMPORTANCE_MIN)
        )
        val n = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(getString(R.string.notif_title))
            .setContentText(getString(R.string.notif_text))
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(1, n)
        }
    }

    /** İzin sonradan verildiyse katmanı ekler (MainActivity her açılışta çağırır). */
    fun ensureOverlay() {
        if (view != null || !Settings.canDrawOverlays(this)) {
            publish()
            return
        }
        val v = SubtitleView(this)
        v.onHidden = { setPassThrough(true) }
        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT,
        ).apply { gravity = Gravity.TOP or Gravity.START }
        try {
            (getSystemService(WINDOW_SERVICE) as WindowManager).addView(v, lp)
            view = v
            params = lp
            passThrough = true
            applyLook()
        } catch (_: Exception) {
            view = null
        }
        publish()
    }

    /**
     * Android 12+ dokunmayı geçiren (FLAG_NOT_TOUCHABLE) katman penceresini %80 opaklığa indirir: altyazı soluk görünür.
     * Bu yüzden altyazı ekrandayken pencere dokunmayı geçirmez, kaybolunca yeniden geçirir. Odak hiçbir zaman almaz,
     * yani kumanda tuşları her zaman öndeki uygulamaya (HDMI girişi) gider. Android 11 ve öncesinde böyle bir sınır yok.
     */
    private fun setPassThrough(pass: Boolean) {
        if (Build.VERSION.SDK_INT < 31 || pass == passThrough) return
        val v = view ?: return
        val lp = params ?: return
        lp.flags = if (pass) lp.flags or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
        else lp.flags and WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE.inv()
        lp.alpha = 1f
        runCatching { (getSystemService(WINDOW_SERVICE) as WindowManager).updateViewLayout(v, lp) }
        passThrough = pass
    }

    fun applyLook() {
        view?.apply {
            sizeLevel = prefs.size
            position = prefs.position
            opacity = prefs.opacity
        }
    }

    /** Ayarlardaki "Örnek altyazı": spec/sample_quotes.json'dan rastgele bir replik, 4 sn. */
    fun showSample() {
        val tr = runCatching {
            val json = assets.open("sample_quotes.json").bufferedReader(Charsets.UTF_8).use { it.readText() }
            val quotes = JSONObject(json).getJSONArray("quotes")
            val list = (0 until quotes.length()).mapNotNull { quotes.getJSONObject(it).optString("tr").ifBlank { null } }
            list.random()
        }.getOrNull() ?: "Savaş. Savaş asla değişmez."
        main.removeCallbacks(hideSample)
        setPassThrough(false)
        view?.show(tr, "verified")
        main.postDelayed(hideSample, 4000)
    }

    fun newCode() {
        prefs.newCode()
        link.kick()
    }

    val status: Status
        get() = Status(pc, view != null, link.port)

    private fun publish() {
        val s = if (instance != null) status else Status(null, false, 0)
        main.post { listener?.invoke(s) }
    }

    // ── LinkServer.Listener (bağlantı iş parçacıklarından gelir) ──

    override fun onLine(text: String, kind: String) {
        main.post {
            main.removeCallbacks(hideSample)
            setPassThrough(false)
            view?.show(text, kind)
        }
    }

    override fun onClear() {
        main.post { view?.hide() }
    }

    override fun onStyle(size: Int?, position: String?, opacity: Int?) {
        main.post {
            size?.let { prefs.size = it }
            position?.let { prefs.position = it }
            opacity?.let { prefs.opacity = it }
            applyLook()
        }
    }

    override fun onLook(relSize: Float?, safeArea: Float?, maxLines: Int?, textColor: String?, bandColor: String?) {
        main.post { view?.applyLook(relSize, safeArea, maxLines, textColor, bandColor) }
    }

    override fun onCodeChanged() {
        main.post { publish() }
    }

    override fun onPeer(pc: String?) {
        main.post {
            this.pc = pc
            publish()
        }
    }

    companion object {
        private const val CHANNEL = "overlay"

        @Volatile
        var instance: OverlayService? = null
            private set

        /** Ekrandaki durum satırı için (ana iş parçacığında çağrılır). */
        var listener: ((Status) -> Unit)? = null

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, OverlayService::class.java))
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, OverlayService::class.java))
        }
    }
}
