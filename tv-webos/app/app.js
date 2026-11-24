/*
 * Makine TV — LG webOS uygulaması.
 *
 * LG'de bir uygulama TV'nin kendi HDMI ekranının üstüne çizemez; bu yüzden konsolun HDMI görüntüsü uygulamanın İÇİNDE
 * açılır (<video> + ext://hdmi:N) ve altyazı onun üstüne çizilir. Satırlar arka plan servisinden gelir
 * (service/service.js; PC'deki Makine Live ile bağlantı). Kumanda: OK menü, ok tuşları gezinme, 1–4 giriş, Geri çıkış.
 * ES5 ve CSS değişkensiz: webOS 3.x tarayıcısı Chromium 38'dir (gerçek LG TV'de webOS 3.4.3 doğrulandı).
 */
(function () {
    'use strict';

    var SVC = 'luna://org.makineceviri.tv.service/';
    var SIZE_PX = [37, 44, 53];                 // 1080 yüksekliğe oran 0,034 / 0,041 / 0,049 (Android sürümüyle aynı)
    var SIZE_LABELS = ['Küçük', 'Orta', 'Büyük'];
    var OPACITY_STEPS = [60, 78, 90];
    var KEY = { LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, OK: 13, BACK: 461, ESC: 27 };
    var onTv = !!window.PalmServiceBridge;

    var prefs = loadPrefs();
    var state = { code: '', pc: null, discovery: true };
    var menuOpen = true;
    var focus = 0;
    var bridges = {};
    var hideTimer = null;
    var samples = ['Savaş. Savaş asla değişmez.'];
    var tvName = 'LG TV';
    var tvIp = '';

    var $ = function (id) { return document.getElementById(id); };
    var rows = Array.prototype.slice.call(document.querySelectorAll('.row'));

    // ── Tercihler ──

    function loadPrefs() {
        var p = { input: 1, size: 1, pos: 'alt', opacity: 78 };
        try {
            var s = JSON.parse(localStorage.getItem('makine-tv') || '{}');
            if (s.input >= 1 && s.input <= 4) p.input = s.input;
            if (s.size >= 0 && s.size <= 2) p.size = s.size;
            if (s.pos === 'ust') p.pos = 'ust';
            if (s.opacity >= 40 && s.opacity <= 100) p.opacity = s.opacity;
        } catch (e) { /* varsayılanlar */ }
        return p;
    }

    function savePrefs() {
        try { localStorage.setItem('makine-tv', JSON.stringify(prefs)); } catch (e) { /* yazılamadı */ }
    }

    // ── Luna çağrıları (PalmServiceBridge; tarayıcıda denerken window.MakineMock) ──

    function call(name, uri, params, cb) {
        if (onTv) {
            var b = new window.PalmServiceBridge();
            bridges[name] = b;                      // abonelikler çöp toplayıcıya gitmesin
            b.onservicecallback = function (raw) {
                var r;
                try { r = JSON.parse(raw); } catch (e) { return; }
                if (cb) cb(r);
            };
            b.call(uri, JSON.stringify(params || {}));
        } else if (window.MakineMock) {
            window.MakineMock.call(uri, params || {}, cb);
        }
    }

    // ── HDMI görüntüsü ──

    function showHdmi() {
        var box = $('hdmi');
        box.innerHTML = '';
        $('panel').classList.toggle('solo', !onTv);
        if (!onTv) return;
        var v = document.createElement('video');
        v.autoplay = true;
        var s = document.createElement('source');
        s.setAttribute('type', 'service/webos-external');
        s.setAttribute('src', 'ext://hdmi:' + prefs.input);
        v.appendChild(s);
        box.appendChild(v);
    }

    function exitToHdmi() {
        // Uygulamadan çıkınca TV'nin kendi HDMI ekranına dön (kendiliğinden dönüş bazı modellerde siyah kalıyor)
        call('exit', 'luna://com.webos.applicationManager/launch', { id: 'com.webos.app.hdmi' + prefs.input }, function () {
            window.close();
        });
        if (!onTv) window.close();
    }

    // ── Altyazı ──

    // PC 0.4+ stili (yoksa eski kural): boyut ekran yüksekliğine oran, kenar güvenli alanı, en çok satır, renkler
    var look = { relSize: 0, safeArea: -1, maxLines: 4, text: '', band: '' };

    function rgb(hex) {
        var m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || '');
        return m ? [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16)] : null;
    }

    function applyLook() {
        var band = document.querySelector('.band');
        var c = rgb(look.band) || [15, 16, 18];
        band.style.backgroundColor = 'rgba(' + c.join(', ') + ', ' + (prefs.opacity / 100) + ')';   // CSS değişkeni yok (Chromium 38)
        band.style.color = rgb(look.text) ? look.text : '';
        var sub = $('sub');
        sub.classList.toggle('ust', prefs.pos === 'ust');
        sub.classList.toggle('alt', prefs.pos !== 'ust');
        sub.style.top = look.safeArea >= 0 && prefs.pos === 'ust' ? (look.safeArea * 100) + '%' : '';
        sub.style.bottom = look.safeArea >= 0 && prefs.pos !== 'ust' ? (look.safeArea * 100) + '%' : '';
        fitText();
    }

    function fitText() {
        // En fazla 4 satır: sığmazsa yazı %74'e kadar küçülür (PC ve Android ile aynı kural)
        var band = document.querySelector('.band');
        var base = look.relSize > 0 ? Math.round(window.innerHeight * look.relSize) : SIZE_PX[prefs.size];
        var scale = 1;
        band.style.fontSize = base + 'px';
        while (scale > 0.74) {
            var lineH = base * scale * 1.24;
            var textH = band.querySelector('.text').offsetHeight;
            if (textH <= lineH * look.maxLines + 2) break;
            scale -= 0.06;
            band.style.fontSize = Math.round(base * scale) + 'px';
        }
    }

    function showSub(text, kind) {
        var band = document.querySelector('.band');
        band.querySelector('.text').textContent = text;
        band.className = 'band ' + (kind === 'verified' || kind === 'pending' ? kind : 'api');
        fitText();
        $('sub').classList.add('show');
    }

    function hideSub() {
        $('sub').classList.remove('show');
    }

    function showSample() {
        clearTimeout(hideTimer);
        showSub(samples[Math.floor(Math.random() * samples.length)], 'verified');
        hideTimer = setTimeout(hideSub, 4000);
    }

    // ── Panel ──

    function render() {
        $('code').textContent = state.code || '····';
        var on = !!state.pc;
        $('dot').className = 'dot' + (on ? ' on' : '');
        $('status').textContent = on ? 'Bağlı · ' + state.pc : 'PC bekleniyor';
        var note = $('note');
        if (!state.discovery && tvIp) {
            note.hidden = false;
            note.textContent = 'Bu TV ağda otomatik bulunamıyor. Bilgisayarda TV bul › Adresle bağlan: ' + tvIp;
        } else {
            note.hidden = true;
        }
        var values = {
            input: 'HDMI ' + prefs.input,
            size: SIZE_LABELS[prefs.size],
            pos: prefs.pos === 'ust' ? 'Üst' : 'Alt',
            opacity: '%' + prefs.opacity,
            sample: '',
            code: ''
        };
        rows.forEach(function (r, i) {
            r.querySelector('b').textContent = values[r.getAttribute('data-key')];
            r.classList.toggle('focus', i === focus);
        });
        $('panel').classList.toggle('hidden', !menuOpen);
    }

    function setMenu(open) {
        menuOpen = open;
        render();
    }

    function activate(key) {
        if (key === 'input') {
            prefs.input = prefs.input % 4 + 1;
            savePrefs();
            showHdmi();
        } else if (key === 'size') {
            prefs.size = (prefs.size + 1) % 3;
            savePrefs();
            applyLook();
            showSample();
        } else if (key === 'pos') {
            prefs.pos = prefs.pos === 'ust' ? 'alt' : 'ust';
            savePrefs();
            applyLook();
            showSample();
        } else if (key === 'opacity') {
            var i = OPACITY_STEPS.indexOf(prefs.opacity);
            prefs.opacity = OPACITY_STEPS[(i + 1) % OPACITY_STEPS.length];
            savePrefs();
            applyLook();
            showSample();
        } else if (key === 'sample') {
            showSample();
        } else if (key === 'code') {
            call('newCode', SVC + 'newCode', {}, null);
        }
        render();
    }

    document.addEventListener('keydown', function (e) {
        var k = e.keyCode;
        if (k >= 49 && k <= 52) {                     // 1–4: konsolun girişi
            prefs.input = k - 48;
            savePrefs();
            showHdmi();
            render();
            return;
        }
        if (k === KEY.BACK || k === KEY.ESC) {
            e.preventDefault();
            if (menuOpen && state.pc) setMenu(false);
            else exitToHdmi();
            return;
        }
        if (!menuOpen) {
            if (k === KEY.OK || k === KEY.UP || k === KEY.DOWN) setMenu(true);
            return;
        }
        if (k === KEY.UP) focus = Math.max(0, focus - 1);
        else if (k === KEY.DOWN) focus = Math.min(rows.length - 1, focus + 1);
        else if (k === KEY.OK) activate(rows[focus].getAttribute('data-key'));
        else return;
        e.preventDefault();
        render();
    });

    rows.forEach(function (r, i) {
        r.addEventListener('click', function () { focus = i; activate(r.getAttribute('data-key')); });
    });

    // ── Servis ──

    function onEvent(e) {
        if (e.returnValue === false) {
            $('status').textContent = 'Bağlantı servisi başlatılamadı · yeniden deneniyor';
            setTimeout(subscribe, 3000);
            return;
        }
        if (e.event === 'state') {
            var wasOn = !!state.pc;
            state.code = e.code;
            state.pc = e.pc || null;
            state.discovery = e.discovery !== false;
            if (state.pc && !wasOn) setMenu(false);         // PC bağlandı: oyuna yer aç
            if (!state.pc && wasOn) { hideSub(); setMenu(true); }
            render();
        } else if (e.event === 'line') {
            clearTimeout(hideTimer);
            showSub(e.text, e.kind);
        } else if (e.event === 'clear') {
            hideSub();
        } else if (e.event === 'style') {
            if (e.size >= 0 && e.size <= 2) prefs.size = e.size;
            if (e.pos === 'alt' || e.pos === 'ust') prefs.pos = e.pos;
            if (e.opacity >= 40 && e.opacity <= 100) prefs.opacity = e.opacity;
            if (e.relSize > 0.01 && e.relSize < 0.2) look.relSize = e.relSize;
            if (e.safeArea >= 0 && e.safeArea < 0.2) look.safeArea = e.safeArea;
            if (e.maxLines >= 1 && e.maxLines <= 6) look.maxLines = e.maxLines;
            if (rgb(e.text)) look.text = e.text;
            if (rgb(e.band)) look.band = e.band;
            savePrefs();
            applyLook();
            render();
        }
    }

    function subscribe() {
        call('start', SVC + 'start', { subscribe: true, name: tvName }, onEvent);
    }

    function watchdog() {
        // Servis kapanmışsa (TV uykudan döndü vb.) yeniden abone ol
        call('state', SVC + 'state', {}, function (r) {
            if (r.returnValue === false || r.running === false) subscribe();
        });
    }

    function keepScreenOn() {
        // PS5 kolu TV için "kullanıcı hareketi" sayılmaz: ekran koruyucu isteğini her seferinde reddet
        call('ss', 'luna://com.webos.service.tvpower/power/registerScreenSaverRequest',
            { subscribe: true, clientName: 'makine-tv' }, function (r) {
                if (r.timestamp) {
                    call('ssr', 'luna://com.webos.service.tvpower/power/responseScreenSaverRequest',
                        { clientName: 'makine-tv', ack: false, timestamp: r.timestamp }, null);
                }
            });
    }

    function loadSamples() {
        var x = new XMLHttpRequest();
        x.onload = function () {
            try {
                var list = JSON.parse(x.responseText).quotes.map(function (q) { return q.tr; }).filter(Boolean);
                if (list.length) samples = list;
            } catch (e) { /* yerleşik örnek kalır */ }
        };
        x.open('GET', 'samples.json');
        x.send();
    }

    function init() {
        showHdmi();
        applyLook();
        render();
        loadSamples();
        keepScreenOn();
        call('sysinfo', 'luna://com.webos.service.tv.systemproperty/getSystemInfo', { keys: ['modelName'] }, function (r) {
            if (r.modelName) tvName = 'LG ' + r.modelName;
            subscribe();
        });
        if (!onTv) subscribe();
        call('net', 'luna://com.palm.connectionmanager/getStatus', { subscribe: true }, function (r) {
            var ip = (r.wired && r.wired.ipAddress) || (r.wifi && r.wifi.ipAddress) || '';
            if (ip) { tvIp = ip; render(); }
        });
        setInterval(watchdog, 10000);
        document.addEventListener('webOSRelaunch', function () { setMenu(true); });
    }

    init();
}());
