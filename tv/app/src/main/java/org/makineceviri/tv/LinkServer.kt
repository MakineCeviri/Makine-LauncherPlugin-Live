package org.makineceviri.tv

import android.util.Log
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.MessageDigest
import kotlin.concurrent.thread

/**
 * PC'deki Makine Live ile bağlantı (protokol v1, ayrıntı: tv_link.py).
 *
 *  Keşif  UDP 47710: {"t":"discover"} → {"t":"here","v":1,"id","name","port"}
 *  Akış   TCP 47711: hello{v,code,pc} → ok{tv} | bad_code; sonra line / clear / style / ping (satır başına bir JSON)
 *
 * Aynı anda tek PC bağlıdır; yeni gelen bağlantı eskisini düşürür. IDLE_TIMEOUT_MS boyunca hiçbir şey gelmezse
 * (PC kapandı, ağ koptu) bağlantı kapatılır ve altyazı temizlenir. TV, "ok"tan sonra PC'ye hiçbir şey yazmaz:
 * PC bağlantının kapandığını okunabilir hâle gelmesinden anlar.
 */
class LinkServer(private val prefs: Prefs, private val listener: Listener) {

    interface Listener {
        fun onLine(text: String, kind: String)
        fun onClear()
        fun onStyle(size: Int?, position: String?, opacity: Int?)
        /** PC 0.4+: boyut ekran yüksekliğine oran, güvenli alan, en çok satır, renkler (yoksa null). */
        fun onLook(relSize: Float?, safeArea: Float?, maxLines: Int?, textColor: String?, bandColor: String?) {}
        /** pc = bağlanan bilgisayarın adı; null = bağlantı yok */
        fun onPeer(pc: String?)
        /** Çok sayıda yanlış denemeden sonra kod yenilendi (ekrandaki kod güncellenmeli) */
        fun onCodeChanged()
    }

    @Volatile private var running = false
    private var udp: DatagramSocket? = null
    private var tcp: ServerSocket? = null
    private val lock = Any()
    private var current: Socket? = null
    private var failures = 0
    private var lockedUntil = 0L

    var port: Int = TCP_PORT
        private set

    fun start() {
        if (running) return
        running = true
        thread(name = "kesif", isDaemon = true) { discoveryLoop() }
        thread(name = "baglanti", isDaemon = true) { acceptLoop() }
    }

    fun stop() {
        running = false
        runCatching { udp?.close() }
        runCatching { tcp?.close() }
        kick()
    }

    /** Bağlı PC'yi düşürür (yeni kod üretildi). */
    fun kick() {
        synchronized(lock) {
            runCatching { current?.close() }
            current = null
        }
    }

    private fun discoveryLoop() {
        while (running) {
            try {
                DatagramSocket(null).use { s ->
                    s.reuseAddress = true
                    s.broadcast = true
                    s.bind(InetSocketAddress(DISCOVERY_PORT))
                    udp = s
                    val buf = ByteArray(2048)
                    while (running) {
                        val p = DatagramPacket(buf, buf.size)
                        s.receive(p)
                        val msg = parse(String(p.data, 0, p.length, Charsets.UTF_8)) ?: continue
                        if (msg.optString("t") != "discover") continue
                        val reply = JSONObject()
                            .put("t", "here").put("v", PROTOCOL_V)
                            .put("id", prefs.tvId).put("name", prefs.tvName).put("port", port)
                            .toString().toByteArray(Charsets.UTF_8)
                        s.send(DatagramPacket(reply, reply.size, p.address, p.port))
                    }
                }
            } catch (e: Exception) {
                if (running) {
                    Log.w(TAG, "keşif: ${e.message}")
                    Thread.sleep(2000)
                }
            }
        }
    }

    private fun acceptLoop() {
        while (running) {
            try {
                val server = ServerSocket().apply { reuseAddress = true }
                try {
                    server.bind(InetSocketAddress(TCP_PORT))
                } catch (e: Exception) {
                    server.bind(InetSocketAddress(0))   // kapı doluysa boş bir kapı; keşif yanıtı doğrusunu söyler
                }
                port = server.localPort
                tcp = server
                server.use {
                    while (running) {
                        val sock = it.accept()
                        thread(name = "pc", isDaemon = true) { serve(sock) }
                    }
                }
            } catch (e: Exception) {
                if (running) {
                    Log.w(TAG, "bağlantı: ${e.message}")
                    Thread.sleep(2000)
                }
            }
        }
    }

    private fun serve(sock: Socket) {
        var accepted = false
        try {
            sock.tcpNoDelay = true
            sock.soTimeout = HELLO_TIMEOUT_MS
            val reader = BufferedReader(InputStreamReader(sock.getInputStream(), Charsets.UTF_8))
            val out = sock.getOutputStream()
            val hello = parse(reader.readLine() ?: return) ?: return
            if (!checkCode(hello)) {
                write(out, JSONObject().put("t", "bad_code"))
                return
            }
            synchronized(lock) {
                runCatching { current?.close() }
                current = sock
            }
            accepted = true
            write(out, JSONObject().put("t", "ok").put("tv", prefs.tvName))
            listener.onPeer(hello.optString("pc").ifBlank { "PC" })
            sock.soTimeout = IDLE_TIMEOUT_MS
            while (running) {
                val line = reader.readLine() ?: break
                val msg = parse(line) ?: continue
                when (msg.optString("t")) {
                    "line" -> msg.optString("text").takeIf { it.isNotBlank() }?.let {
                        listener.onLine(it, msg.optString("kind", "api"))
                    }
                    "clear" -> listener.onClear()
                    // PC'den oyuncuya not (kaynak kayboldu, okuma durdu, kota): satır gibi gösterilir, boşsa temizlenir
                    "note" -> msg.optString("text").let { if (it.isBlank()) listener.onClear() else listener.onLine(it, "note") }
                    "style" -> listener.onStyle(
                        msg.optInt("size", -1).takeIf { it >= 0 },
                        msg.optString("pos").takeIf { it.isNotEmpty() },
                        msg.optInt("opacity", -1).takeIf { it > 0 },
                    ).also {
                        listener.onLook(
                            msg.optDouble("relSize", -1.0).takeIf { it in 0.01..0.2 }?.toFloat(),
                            msg.optDouble("safeArea", -1.0).takeIf { it in 0.0..0.2 }?.toFloat(),
                            msg.optInt("maxLines", -1).takeIf { it in 1..6 },
                            msg.optString("text").takeIf { it.startsWith("#") },
                            msg.optString("band").takeIf { it.startsWith("#") },
                        )
                    }
                }
            }
        } catch (_: SocketTimeoutException) {
            // PC sustu: bağlantı ölü sayılır
        } catch (e: Exception) {
            Log.d(TAG, "pc: ${e.message}")
        } finally {
            runCatching { sock.close() }
            if (accepted) {
                val stillCurrent = synchronized(lock) {
                    (current === sock).also { if (it) current = null }
                }
                if (stillCurrent) {
                    listener.onClear()
                    listener.onPeer(null)
                }
            }
        }
    }

    /**
     * Eşleşme kodu deneme yanılmayla bulunamasın: her yanlış denemede 1 sn beklenir; MAX_FAILURES yanlıştan sonra kod
     * yenilenir (TV ekranında yenisi görünür) ve LOCK_MS boyunca hiçbir kod kabul edilmez. Karşılaştırma sabit sürelidir.
     */
    private fun checkCode(hello: JSONObject): Boolean {
        val now = System.currentTimeMillis()
        val given = hello.optString("code").toByteArray(Charsets.UTF_8)
        val ok = hello.optString("t") == "hello" &&
            MessageDigest.isEqual(given, prefs.code.toByteArray(Charsets.UTF_8))
        synchronized(lock) {
            if (now < lockedUntil) return false
            if (ok) {
                failures = 0
                return true
            }
            failures++
            if (failures >= MAX_FAILURES) {
                failures = 0
                lockedUntil = now + LOCK_MS
                prefs.newCode()
                listener.onCodeChanged()
            }
        }
        Thread.sleep(FAIL_DELAY_MS)
        return false
    }

    private fun write(out: OutputStream, msg: JSONObject) {
        out.write((msg.toString() + "\n").toByteArray(Charsets.UTF_8))
        out.flush()
    }

    private fun parse(s: String): JSONObject? = try { JSONObject(s) } catch (_: Exception) { null }

    companion object {
        private const val TAG = "MakineTV"
        const val PROTOCOL_V = 1
        const val DISCOVERY_PORT = 47710
        const val TCP_PORT = 47711
        private const val HELLO_TIMEOUT_MS = 5000
        private const val IDLE_TIMEOUT_MS = 15000
        private const val MAX_FAILURES = 5
        private const val LOCK_MS = 60_000L
        private const val FAIL_DELAY_MS = 1000L
    }
}
