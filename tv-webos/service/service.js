/*
 * Makine TV — LG webOS arka plan servisi (org.makineceviri.tv.service).
 *
 * PC'deki Makine Live ile bağlantı; protokol v1, Android TV'deki LinkServer.kt ve PC'deki tv_link.py ile aynı:
 *   Keşif  UDP 47710: {"t":"discover"} → {"t":"here","v":1,"id","name","port"}
 *   Akış   TCP 47711: hello{v,code,pc} → ok{tv} | bad_code; sonra line / clear / style / ping (satır başına bir JSON)
 * Aynı anda tek PC bağlıdır; 15 sn sessizlikte bağlantı kapanır ve altyazı temizlenir. Eşleşme kodu deneme yanılmaya
 * karşı korunur: her yanlışta 1 sn bekleme, 5 yanlıştan sonra yeni kod ve 60 sn kilit.
 *
 * Web uygulaması "start" yöntemine abone olur ve olayları alır (event: state | line | clear | style). Abone varken
 * servis activityManager ile canlı tutulur. ES5 ile yazılmıştır: webOS 4.x'te Node.js 0.12 çalışır.
 */
var Service = require('webos-service');
var net = require('net');
var dgram = require('dgram');
var fs = require('fs');
var path = require('path');
var crypto = require('crypto');

var SERVICE_ID = 'org.makineceviri.tv.service';
var PROTOCOL_V = 1;
var DISCOVERY_PORT = 47710;
var TCP_PORT = 47711;
var HELLO_TIMEOUT_MS = 5000;
var IDLE_TIMEOUT_MS = 15000;
var MAX_FAILURES = 5;
var LOCK_MS = 60000;
var FAIL_DELAY_MS = 1000;

var service = new Service(SERVICE_ID);
var subscribers = {};          // uniqueToken → message
var store = loadStore();       // {id, code}
var tvName = 'LG TV';
var tcp = null;
var udp = null;
var udpOk = false;
var port = TCP_PORT;
var current = null;            // bağlı PC soketi
var peer = null;               // bağlı PC'nin adı
var failures = 0;
var lockedUntil = 0;
var keepAlive = null;

// ── Kalıcı durum (kimlik, kod) ──

function storeCandidates() {
    var files = [path.join(__dirname, 'state.json'), '/media/developer/temp/makine-tv-state.json', '/tmp/makine-tv-state.json'];
    if (process.env.MAKINE_TV_STATE) files.unshift(process.env.MAKINE_TV_STATE);   // testler
    return files;
}

function loadStore() {
    var files = storeCandidates();
    for (var i = 0; i < files.length; i++) {
        try {
            var s = JSON.parse(fs.readFileSync(files[i], 'utf8'));
            if (s && s.id && /^\d{4}$/.test(s.code)) return s;
        } catch (e) { /* sıradaki */ }
    }
    var fresh = { id: crypto.randomBytes(16).toString('hex'), code: randomCode() };
    saveStore(fresh);
    return fresh;
}

function saveStore(s) {
    var files = storeCandidates();
    for (var i = 0; i < files.length; i++) {
        try {
            fs.writeFileSync(files[i], JSON.stringify(s));
            return;
        } catch (e) { /* yazılamıyor: sıradaki */ }
    }
}

function randomCode() {
    var n = crypto.randomBytes(4).readUInt32BE(0) % 10000;
    return ('000' + n).slice(-4);
}

function newCode() {
    store.code = randomCode();
    saveStore(store);
    kick();
    emitState();
}

// ── Abonelere olay ──

function emit(payload) {
    payload.returnValue = true;
    for (var k in subscribers) {
        if (subscribers.hasOwnProperty(k)) {
            try { subscribers[k].respond(payload); } catch (e) { delete subscribers[k]; }
        }
    }
}

function stateEvent() {
    return { event: 'state', code: store.code, pc: peer, port: port, discovery: udpOk, name: tvName };
}

function emitState() {
    emit(stateEvent());
}

// ── Sunucular ──

function startServers() {
    if (!tcp) startTcp(TCP_PORT);
    if (!udp) startUdp();
}

function stopServers() {
    kick();
    if (tcp) { try { tcp.close(); } catch (e) { /* kapalı */ } tcp = null; }
    if (udp) { try { udp.close(); } catch (e) { /* kapalı */ } udp = null; udpOk = false; }
}

function startTcp(wanted) {
    var server = net.createServer(serve);
    server.on('error', function (err) {
        tcp = null;
        if (wanted !== 0) {
            startTcp(0);                      // kapı doluysa boş bir kapı; keşif yanıtı doğrusunu söyler
        } else {
            console.error('tcp: ' + err.message);
        }
    });
    server.listen(wanted, '0.0.0.0', function () {
        port = server.address().port;
        emitState();
    });
    tcp = server;
}

function startUdp() {
    var sock;
    try {
        sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
    } catch (e) {
        sock = dgram.createSocket('udp4');
    }
    sock.on('error', function (err) {
        console.error('keşif: ' + err.message);
        udpOk = false;
        try { sock.close(); } catch (e) { /* kapalı */ }
        udp = null;
        emitState();                          // PC tarafında adresle bağlanılabilir
    });
    sock.on('message', function (buf, rinfo) {
        var msg = parse(buf.toString('utf8'));
        if (!msg || msg.t !== 'discover') return;
        var reply = utf8(JSON.stringify({ t: 'here', v: PROTOCOL_V, id: store.id, name: tvName, port: port }));
        sock.send(reply, 0, reply.length, rinfo.port, rinfo.address);
    });
    sock.bind(DISCOVERY_PORT, function () {
        udpOk = true;
        emitState();
    });
    udp = sock;
}

function kick() {
    if (current) {
        try { current.destroy(); } catch (e) { /* kapalı */ }
    }
}

// ── Bir PC bağlantısı ──

function sameCode(a, b) {
    // Sabit süreli karşılaştırma: cevap süresinden kod tahmin edilemesin
    a = String(a);
    b = String(b);
    var diff = a.length ^ b.length;
    for (var i = 0; i < b.length; i++) diff |= (a.charCodeAt(i) || 0) ^ b.charCodeAt(i);
    return diff === 0;
}

function utf8(s) {
    return Buffer.from ? Buffer.from(s, 'utf8') : new Buffer(s, 'utf8');   // Node 0.12'de Buffer.from yok
}

function checkCode(hello) {
    var now = Date.now();
    if (now < lockedUntil) return false;
    if (hello.t === 'hello' && sameCode(hello.code, store.code)) {
        failures = 0;
        return true;
    }
    failures++;
    if (failures >= MAX_FAILURES) {
        failures = 0;
        lockedUntil = now + LOCK_MS;
        newCode();                            // ekranda yeni kod görünür
    }
    return false;
}

function serve(sock) {
    var buf = '';
    var accepted = false;
    var helloTimer = setTimeout(function () { sock.destroy(); }, HELLO_TIMEOUT_MS);
    sock.setNoDelay(true);
    sock.setEncoding('utf8');

    function send(msg) {
        try { sock.write(JSON.stringify(msg) + '\n'); } catch (e) { /* kapandı */ }
    }

    function handle(msg) {
        if (!accepted) {
            clearTimeout(helloTimer);
            var locked = Date.now() < lockedUntil;
            if (!checkCode(msg)) {
                setTimeout(function () { send({ t: 'bad_code' }); sock.end(); }, locked ? 0 : FAIL_DELAY_MS);
                return false;
            }
            if (current && current !== sock) {
                try { current.destroy(); } catch (e) { /* kapalı */ }
            }
            current = sock;
            accepted = true;
            peer = String(msg.pc || 'PC').slice(0, 64);
            send({ t: 'ok', tv: tvName });
            sock.setTimeout(IDLE_TIMEOUT_MS, function () { sock.destroy(); });
            emitState();
            return true;
        }
        if (msg.t === 'line' && msg.text) {
            emit({ event: 'line', text: String(msg.text), kind: String(msg.kind || 'api') });
        } else if (msg.t === 'clear') {
            emit({ event: 'clear' });
        } else if (msg.t === 'style') {
            emit({ event: 'style', size: msg.size, pos: msg.pos, opacity: msg.opacity });
        }
        return true;
    }

    sock.on('data', function (chunk) {
        buf += chunk;
        if (buf.length > 65536) { sock.destroy(); return; }
        var nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            var line = buf.slice(0, nl);
            buf = buf.slice(nl + 1);
            var msg = parse(line);
            if (msg && handle(msg) === false) return;
        }
    });
    sock.on('error', function () { /* close izler */ });
    sock.on('close', function () {
        clearTimeout(helloTimer);
        if (accepted && current === sock) {
            current = null;
            peer = null;
            emit({ event: 'clear' });
            emitState();
        }
    });
}

function parse(s) {
    try {
        var o = JSON.parse(s);
        return (o && typeof o === 'object') ? o : null;
    } catch (e) {
        return null;
    }
}

// ── Luna yöntemleri ──

service.register('start', function (message) {
    var p = message.payload || {};
    if (p.name) tvName = String(p.name).slice(0, 64);
    if (message.isSubscription) {
        subscribers[message.uniqueToken] = message;
        if (!keepAlive) {
            keepAlive = true;
            service.activityManager.create('makine-tv-keepalive', function (activity) { keepAlive = activity; });
        }
    }
    startServers();
    var s = stateEvent();
    s.returnValue = true;
    message.respond(s);
}, function (message) {
    delete subscribers[message.uniqueToken];
    if (Object.keys(subscribers).length === 0) {
        stopServers();
        if (keepAlive && keepAlive !== true) service.activityManager.complete(keepAlive, function () {});
        keepAlive = null;
    }
});

service.register('newCode', function (message) {
    newCode();
    message.respond({ returnValue: true, code: store.code });
});

service.register('state', function (message) {
    var s = stateEvent();
    s.returnValue = true;
    s.running = !!tcp;
    message.respond(s);
});

module.exports = { _test: { checkCode: checkCode, sameCode: sameCode, store: function () { return store; } } };
