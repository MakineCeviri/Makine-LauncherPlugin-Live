package org.makineceviri.tv

import android.app.Activity
import android.app.ActivityManager
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * TV ekranı (kumandayla, 3 metreden okunur): büyük eşleşme kodu, bağlantı durumu, birkaç ayar.
 * Geri tuşu uygulamayı arka plana alır; katman ve bağlantı çalışmaya devam eder.
 * Tasarım: spec/design_tokens.json ("altyazı stüdyosu"): koyu zemin, tek vurgu rengi altyazı sarısı.
 */
class MainActivity : Activity() {

    private lateinit var prefs: Prefs
    private lateinit var codeView: TextView
    private lateinit var statusDot: View
    private lateinit var statusText: TextView
    private lateinit var addressText: TextView
    private lateinit var permCard: LinearLayout
    private lateinit var permTitle: TextView
    private lateinit var permBody: TextView
    private lateinit var permButton: View
    private val rows = mutableMapOf<String, TextView>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)
        setContentView(build())
        OverlayService.start(this)
    }

    override fun onResume() {
        super.onResume()
        OverlayService.listener = { render(it) }
        OverlayService.instance?.let { it.ensureOverlay(); render(it.status) } ?: render(null)
        refreshRows()
        codeView.text = prefs.code
    }

    override fun onPause() {
        OverlayService.listener = null
        super.onPause()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            moveTaskToBack(true)        // kapanmaz: altyazı arka planda gelmeye devam eder
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    // ── Durum ──

    private fun render(s: OverlayService.Status?) {
        codeView.text = prefs.code          // çok sayıda yanlış denemeden sonra kod yenilenmiş olabilir
        val connected = s?.pc != null
        statusDot.background = dot(if (connected) VERIFIED else ACCENT)
        statusText.text = when {
            s == null -> "Başlatılıyor…"
            connected -> "Bağlı  ·  ${s.pc}"
            else -> "PC bekleniyor"
        }
        // PC TV'yi kendiliğinden bulamazsa (yayın paketlerini engelleyen modem): "TV bul › Adresle bağlan"
        addressText.text = localAddress()?.let { "Bu TV'nin adresi  $it" } ?: "Ağ bağlantısı yok"
        val noOverlay = s != null && !s.overlay
        permCard.visibility = if (noOverlay) View.VISIBLE else View.GONE
        if (noOverlay && lowRam()) {
            // Düşük bellekli cihazlarda Android katman iznini hiç vermez (ayar da yoktur)
            permTitle.text = "Bu cihaz altyazı katmanına izin vermiyor"
            permBody.text = "Android, düşük bellekli cihazlarda uygulamaların görüntünün üstüne çizmesine izin vermez. " +
                "PC'deki Makine Live'da Konsol › Altyazı nerede görünsün › Telefonda'yı seçip altyazıyı telefonda ya da " +
                "tablette okuyabilirsiniz."
            permButton.visibility = View.GONE
        }
    }

    private fun lowRam(): Boolean =
        (getSystemService(ACTIVITY_SERVICE) as? ActivityManager)?.isLowRamDevice == true

    /** Ev ağındaki IPv4 adresi (Wi-Fi ya da Ethernet). */
    private fun localAddress(): String? = runCatching {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback && !it.isVirtual }
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<Inet4Address>()
            .firstOrNull { it.isSiteLocalAddress }?.hostAddress
    }.getOrNull()

    private fun refreshRows() {
        rows["size"]?.text = Prefs.SIZE_LABELS[prefs.size]
        rows["pos"]?.text = if (prefs.position == "ust") "Üst" else "Alt"
        rows["opacity"]?.text = "%${prefs.opacity}"
        rows["autostart"]?.text = if (prefs.autostart) "Açık" else "Kapalı"
    }

    private fun changed() {
        refreshRows()
        OverlayService.instance?.applyLook()
        OverlayService.instance?.showSample()
    }

    // ── Arayüz ──

    private fun build(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(BG)
            setPadding(dp(56), dp(44), dp(56), dp(36))
        }

        // Sol: marka, kod, durum, adımlar
        val left = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(left, LinearLayout.LayoutParams(0, MATCH_PARENT, 1f))

        val brand = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        brand.addView(ImageView(this).apply { setImageResource(R.drawable.logo) }, LinearLayout.LayoutParams(dp(40), dp(40)))
        brand.addView(text("Makine TV", 26f, TEXT, bold = true).apply { setPadding(dp(14), 0, 0, 0) })
        left.addView(brand)
        left.addView(text("Konsol oyunları için Türkçe altyazı", 16f, TEXT2).apply { setPadding(0, dp(6), 0, dp(22)) })

        permTitle = text("Katman izni gerekli", 16f, TEXT, bold = true)
        permBody = text("İzin olmadan altyazı oyunun üstünde görünemez. PC'deki \"TV'ye kur\" bu izni kendisi verir. " +
            "Elle vermek için: Ayarlar › Uygulamalar › Özel uygulama erişimi › Diğer uygulamaların üzerinde göster.",
            14f, TEXT2).apply { setPadding(0, dp(6), 0, dp(12)) }
        permButton = button("İzin ekranını aç") { openOverlaySettings() }
        permCard = card(stroke = DISPUTED).apply {
            visibility = View.GONE
            addView(permTitle)
            addView(permBody)
            addView(permButton, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        }
        left.addView(permCard, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { bottomMargin = dp(16) })

        val codeCard = card()
        codeCard.addView(text("EŞLEŞME KODU", 13f, TEXT3, bold = true).apply { letterSpacing = 0.12f })
        codeView = text(prefs.code, 76f, ACCENT, mono = true).apply {
            letterSpacing = 0.28f
            setPadding(0, dp(4), 0, dp(6))
        }
        codeCard.addView(codeView)
        val status = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        statusDot = View(this)
        status.addView(statusDot, LinearLayout.LayoutParams(dp(10), dp(10)))
        statusText = text("Başlatılıyor…", 18f, TEXT).apply { setPadding(dp(10), 0, 0, 0) }
        status.addView(statusText)
        codeCard.addView(status)
        addressText = text("", 14f, TEXT3, mono = true).apply { setPadding(0, dp(10), 0, 0) }
        codeCard.addView(addressText)
        left.addView(codeCard, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        val steps = listOf(
            "PC'de Makine Live'ı açın ve Konsol / TV sekmesine geçin.",
            "Listede bu TV'yi (${prefs.tvName}) seçin ve yukarıdaki kodu girin.",
            "TV'yi konsolun girişine alın. Altyazılar oyunun üstünde belirir.",
        )
        val stepBox = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(22), 0, 0)
        }
        steps.forEachIndexed { i, s ->
            val row = LinearLayout(this).apply { setPadding(0, 0, 0, dp(10)) }
            row.addView(text("${i + 1}", 15f, ACCENT, mono = true).apply { setPadding(0, 0, dp(12), 0) })
            row.addView(text(s, 15f, TEXT2), LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            stepBox.addView(row)
        }
        left.addView(stepBox)

        // Sağ: ayarlar
        val right = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(44), dp(6), 0, 0)
        }
        root.addView(right, LinearLayout.LayoutParams(dp(380), MATCH_PARENT))
        right.addView(text("Altyazı", 13f, TEXT3, bold = true).apply { letterSpacing = 0.12f; setPadding(dp(4), 0, 0, dp(10)) })
        right.addView(setting("size", "Yazı boyutu") { prefs.size = (prefs.size + 1) % 3; changed() })
        right.addView(setting("pos", "Konum") { prefs.position = if (prefs.position == "ust") "alt" else "ust"; changed() })
        right.addView(setting("opacity", "Arka plan") {
            val steps = Prefs.OPACITY_STEPS
            prefs.opacity = steps[(steps.indexOf(prefs.opacity).coerceAtLeast(0) + 1) % steps.size]
            changed()
        })
        right.addView(setting(null, "Örnek altyazı göster") { OverlayService.instance?.showSample() })
        right.addView(text("Uygulama", 13f, TEXT3, bold = true).apply { letterSpacing = 0.12f; setPadding(dp(4), dp(10), 0, dp(10)) })
        right.addView(setting("autostart", "Açılışta başlat") { prefs.autostart = !prefs.autostart; refreshRows() })
        right.addView(setting(null, "Yeni kod üret") {
            OverlayService.instance?.newCode() ?: prefs.newCode()
            codeView.text = prefs.code
        })
        right.addView(text("Geri tuşu uygulamayı kapatmaz; altyazılar arka planda gelmeye devam eder.", 13f, TEXT3)
            .apply { setPadding(dp(4), dp(8), 0, 0) })

        right.getChildAt(1).requestFocus()
        return root
    }

    private fun setting(key: String?, label: String, onClick: () -> Unit): View {
        val row = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            isFocusable = true
            isClickable = true
            background = rowBackground()
            setPadding(dp(18), dp(11), dp(18), dp(11))
            setOnClickListener { onClick() }
        }
        val name = text(label, 16f, TEXT)
        row.addView(name, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        if (key != null) {
            val value = text("", 16f, ACCENT, bold = true)
            rows[key] = value
            row.addView(value)
        }
        row.setOnFocusChangeListener { _, has -> name.setTextColor(if (has) Color.WHITE else TEXT) }
        row.layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { bottomMargin = dp(8) }
        return row
    }

    private fun button(label: String, onClick: () -> Unit): View =
        text(label, 15f, ACCENT_INK, bold = true).apply {
            isFocusable = true
            isClickable = true
            setPadding(dp(18), dp(10), dp(18), dp(10))
            background = StateListDrawable().apply {
                addState(intArrayOf(android.R.attr.state_focused), round(Color.WHITE, 0, dp(6)))
                addState(intArrayOf(), round(ACCENT, 0, dp(6)))
            }
            setOnClickListener { onClick() }
        }

    private fun openOverlaySettings() {
        val intents = listOf(
            Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")),
            Intent(Settings.ACTION_SETTINGS),
        )
        for (i in intents) {
            if (runCatching { startActivity(i) }.isSuccess) return
        }
    }

    private fun card(stroke: Int = LINE): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        background = round(SURFACE, stroke, dp(10))
        setPadding(dp(28), dp(22), dp(28), dp(24))
    }

    private fun rowBackground() = StateListDrawable().apply {
        addState(intArrayOf(android.R.attr.state_focused), round(RAISED, ACCENT, dp(6), strokePx = dp(2)))
        addState(intArrayOf(), round(SURFACE, LINE, dp(6)))
    }

    private fun round(fill: Int, stroke: Int, radius: Int, strokePx: Int = dp(1)) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = radius.toFloat()
        if (stroke != 0) setStroke(strokePx, stroke)
    }

    private fun dot(color: Int) = GradientDrawable().apply {
        shape = GradientDrawable.OVAL
        setColor(color)
    }

    private fun text(s: String, sp: Float, color: Int, bold: Boolean = false, mono: Boolean = false) = TextView(this).apply {
        text = s
        setTextSize(TypedValue.COMPLEX_UNIT_SP, sp)
        setTextColor(color)
        typeface = when {
            mono -> Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
            bold -> Typeface.create("sans-serif-medium", Typeface.NORMAL)
            else -> Typeface.create("sans-serif", Typeface.NORMAL)
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    companion object {
        // spec/design_tokens.json → color.dark
        private val BG = Color.parseColor("#0f1012")
        private val SURFACE = Color.parseColor("#16171a")
        private val RAISED = Color.parseColor("#24262b")
        private val LINE = Color.parseColor("#2a2c31")
        private val TEXT = Color.parseColor("#ecebe6")
        private val TEXT2 = Color.parseColor("#a9a8a2")
        private val TEXT3 = Color.parseColor("#86868b")
        private val ACCENT = Color.parseColor("#f2c230")
        private val ACCENT_INK = Color.parseColor("#16171a")
        private val VERIFIED = Color.parseColor("#3fae72")
        private val DISPUTED = Color.parseColor("#e2674a")
    }
}
