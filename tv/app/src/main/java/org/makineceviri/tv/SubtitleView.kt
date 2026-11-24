package org.makineceviri.tv

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.view.View
import android.view.animation.PathInterpolator

/**
 * Altyazı şeridi — PC'deki çeviri kutusunun (overlay_render.py) TV karşılığı, spec/design_tokens.json → overlay:
 * koyu şerit (#0f1012, opaklık ayarlı), 10 px köşe, beyaz yazı + ince siyah kontur, solda durum şeridi
 * (onaylı = yeşil, havuzda bekleyen = sarı, ilk çeviri = yok). Geliş 160 ms, gidiş 220 ms.
 * Ekranın tamamını kaplar ama yalnızca şeridi çizer; odak almaz, dokunmayı yalnızca altyazı ekrandayken tutar
 * (OverlayService.setPassThrough).
 */
class SubtitleView(context: Context) : View(context) {

    var sizeLevel = 1
        set(v) { field = v; relayout() }
    var position = "alt"
        set(v) { field = v; invalidate() }
    var opacity = 78
        set(v) { field = v; invalidate() }

    // PC 0.4+ stili (yoksa eski kural): boyut ekran yüksekliğine oran, kenar güvenli alanı, en çok satır
    private var relSize: Float? = null
    private var safeArea: Float? = null
    private var maxLines = 4

    fun applyLook(relSize: Float?, safeArea: Float?, maxLines: Int?, textColor: String?, bandColor: String?) {
        this.relSize = relSize
        this.safeArea = safeArea
        this.maxLines = maxLines ?: 4
        fill.color = textColor?.let { runCatching { Color.parseColor(it) }.getOrNull() } ?: Color.WHITE
        band.color = bandColor?.let { runCatching { Color.parseColor(it) }.getOrNull() } ?: BAND
        relayout()
    }

    /** Gidiş animasyonu bitip şerit tamamen kaybolunca. */
    var onHidden: (() -> Unit)? = null

    private var text: String? = null
    private var kind = "api"
    private var layout: StaticLayout? = null
    private var strokeLayout: StaticLayout? = null
    private var alphaF = 0f
    private var anim: ValueAnimator? = null

    private val fill = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }
    private val stroke = TextPaint(fill).apply {
        style = Paint.Style.STROKE
        color = Color.BLACK
        strokeJoin = Paint.Join.ROUND
    }
    private val band = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = BAND }
    private val bar = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()
    private val ease = PathInterpolator(0.2f, 0.7f, 0.2f, 1f)

    /** Kimlikli bir satır ilk kez görünür biçimde çizildiğinde (LinkServer.drawn) */
    var onDrawn: ((Int) -> Unit)? = null
    private var pendingDrawn = -1

    fun show(newText: String, newKind: String, id: Int = -1) {
        pendingDrawn = id
        val wasVisible = text != null && alphaF > 0.5f
        text = newText
        kind = newKind
        relayout()
        // Görünürken satır değişiyorsa kısa bir göz kırpması yeter; değilse yumuşak geliş
        fadeTo(1f, if (wasVisible) 90 else FADE_IN_MS, from = if (wasVisible) 0.7f else alphaF)
    }

    fun hide() {
        if (text == null) return
        fadeTo(0f, FADE_OUT_MS) { text = null; layout = null; strokeLayout = null; onHidden?.invoke() }
    }

    private fun fadeTo(target: Float, ms: Long, from: Float = alphaF, done: (() -> Unit)? = null) {
        anim?.cancel()
        anim = ValueAnimator.ofFloat(from, target).apply {
            duration = ms
            interpolator = ease
            addUpdateListener { alphaF = it.animatedValue as Float; invalidate() }
            if (done != null) {
                addListener(object : android.animation.AnimatorListenerAdapter() {
                    private var cancelled = false
                    override fun onAnimationCancel(animation: android.animation.Animator) { cancelled = true }
                    override fun onAnimationEnd(animation: android.animation.Animator) { if (!cancelled) done() }
                })
            }
            start()
        }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        relayout()
    }

    /** Yazı en fazla 4 satıra sığacak şekilde %74'e kadar küçülür (PC'deki kutuyla aynı kural). */
    private fun relayout() {
        val t = text
        if (t == null || width == 0 || height == 0) {
            layout = null
            invalidate()
            return
        }
        val base = height * (relSize ?: SIZE_RATIO[sizeLevel.coerceIn(0, 2)])
        val maxW = (width * MAX_WIDTH_RATIO).toInt()
        var scale = 1f
        var l: StaticLayout
        while (true) {
            fill.textSize = base * scale
            l = StaticLayout.Builder.obtain(t, 0, t.length, fill, maxW)
                .setAlignment(Layout.Alignment.ALIGN_CENTER)
                .setLineSpacing(0f, 1.08f)
                .setIncludePad(false)
                .build()
            if (l.lineCount <= maxLines || scale <= 0.74f) break
            scale -= 0.06f
        }
        stroke.textSize = fill.textSize
        stroke.strokeWidth = fill.textSize * 0.06f
        strokeLayout = StaticLayout.Builder.obtain(t, 0, t.length, stroke, maxW)
            .setAlignment(Layout.Alignment.ALIGN_CENTER)
            .setLineSpacing(0f, 1.08f)
            .setIncludePad(false)
            .build()
        layout = l
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val l = layout ?: return
        if (alphaF <= 0.01f) return
        var textW = 0f
        for (i in 0 until l.lineCount) textW = maxOf(textW, l.getLineWidth(i))
        val padX = fill.textSize * 0.85f
        val padY = fill.textSize * 0.46f
        val bw = textW + 2 * padX
        val bh = l.height + 2 * padY
        val cx = width / 2f
        val top = if (position == "ust") height * (safeArea ?: 0.06f) else height * (1f - (safeArea ?: 0.08f)) - bh
        rect.set(cx - bw / 2, top, cx + bw / 2, top + bh)
        val radius = fill.textSize * 0.38f

        band.alpha = (255 * opacity / 100f * alphaF).toInt()
        canvas.drawRoundRect(rect, radius, radius, band)
        val barColor = when (kind) {
            "verified" -> VERIFIED
            "pending" -> ACCENT
            else -> 0
        }
        if (barColor != 0) {
            bar.color = barColor
            bar.alpha = (255 * alphaF).toInt()
            val bwPx = maxOf(3f, fill.textSize * 0.08f)
            canvas.drawRect(rect.left, rect.top + radius, rect.left + bwPx, rect.bottom - radius, bar)
        }

        val a = (255 * alphaF).toInt()
        stroke.alpha = a
        fill.alpha = a
        canvas.save()
        canvas.translate(cx - l.width / 2f, top + padY)
        // Önce kontur, sonra dolgu: açık sahnelerde de okunur
        strokeLayout?.draw(canvas)
        l.draw(canvas)
        canvas.restore()
        if (pendingDrawn >= 0) {
            val id = pendingDrawn
            pendingDrawn = -1
            onDrawn?.invoke(id)
        }
    }

    companion object {
        private val BAND = Color.parseColor("#0f1012")
        private val VERIFIED = Color.parseColor("#3fae72")
        private val ACCENT = Color.parseColor("#f2c230")
        private val SIZE_RATIO = floatArrayOf(0.034f, 0.041f, 0.049f)   // ekran yüksekliğine oran: küçük / orta / büyük
        private const val MAX_WIDTH_RATIO = 0.62f
        private const val FADE_IN_MS = 160L
        private const val FADE_OUT_MS = 220L
    }
}
