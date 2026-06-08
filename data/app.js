(function() {
'use strict';

// ── Config Cache & Populate ──────────────────────────────────────
var configCache = null;

window.setUploadType = function(radio) {
  var form = document.getElementById('ota-upload-form');
  if (form) form.action = radio.value === 'fs' ? '/ota-upload-fs' : '/ota-upload';
};

function loadConfig(cb) {
  if (configCache) { cb(configCache); return; }
  if (window.__cfg) { configCache = window.__cfg; cb(configCache); return; }
  fetch('/api/config')
    .then(function(r) { return r.json(); })
    .then(function(d) { configCache = d; cb(d); })
    .catch(function(e) { console.error('loadConfig error:', e); });
}

function setVal(name, val) {
  if (val === undefined) return;
  var el = document.querySelector('[name="' + name + '"]');
  if (!el) return;
  if (el.type === 'checkbox') el.checked = !!val;
  else el.value = val !== null ? val : '';
}

function populateTiming(c) {
  setVal('coin_delay',          c.coin_delay);
  setVal('bill_isr_debounce',   c.bill_isr_debounce);
  setVal('bill_group_timeout',  c.bill_group_timeout);
  setVal('disp_time',           c.disp_time);
  setVal('keypad_time',         c.keypad_time);
  setVal('slot_sel_time',       c.slot_sel_time);
  setVal('disp_timeout',        c.disp_timeout);
  setVal('web_timeout',         c.web_timeout);
  setVal('status_enabled',      c.status_enabled);
}

function populateTelegram(c) {
  setVal('tg_enabled',             c.tg_enabled);
  setVal('tg_token',               c.tg_token);
  setVal('tg_chat_id',             c.tg_chat_id);
  setVal('notify_sale',            c.notify_sale);
  setVal('notify_almost_empty',    c.notify_almost_empty);
  setVal('almost_empty_threshold', c.almost_empty_threshold);
  setVal('notify_empty',           c.notify_empty);
  setVal('notify_crash',           c.notify_crash);
  setVal('notify_bruteforce',      c.notify_bruteforce);
}

function populateDisplay(c) {
  setVal('slogan',              c.slogan);
  setVal('footer',              c.footer);
  setVal('display_white_mode',  c.display_white_mode);
}

function populateSumUp(c) {
  setVal('enabled',    c.sumup_enabled);
  setVal('apiKey',     c.sumup_apiKey);
  setVal('merchantId', c.sumup_merchantId);
  setVal('readerId',   c.sumup_readerId);
  setVal('timeout',    c.sumup_timeout);
}

function populateNetwork(c) {
  var hasStatic = c.static_ip && c.static_ip.length > 0;
  applyNetMode(hasStatic);
  setVal('static_ip', c.static_ip);
  setVal('gateway',   c.gateway);
  setVal('subnet',    c.subnet);
  setVal('dns1',      c.dns1);
}

function populatePayment(c) {
  setVal('coinEnabled', c.coinEnabled);
  setVal('billEnabled', c.billEnabled);
  ['coin_1','coin_2','coin_5','coin_10','coin_20','coin_50','coin_100','coin_200'].forEach(function(k) {
    setVal(k, c[k] !== undefined ? c[k] : 0);
  });
  ['bill_5','bill_10','bill_20','bill_50','bill_100'].forEach(function(k) {
    setVal(k, c[k] !== undefined ? c[k] : 0);
  });
}

function populateSlots(c) {
  setVal('maxSlots', c.activeSlots);
  var priceForm = document.getElementById('priceForm');
  if (priceForm && c.slotPrices) {
    var grid = priceForm.querySelector('div');
    if (grid) {
      var h = '';
      c.slotPrices.forEach(function(cents, i) {
        var price = (cents / 100).toFixed(2);
        h += '<div style="display:flex;flex-direction:column;gap:4px;">'
           + '<label style="font-size:0.8rem;color:var(--text-sec);">Fach ' + (i+1) + '</label>'
           + '<input type="number" step="0.01" min="0" name="price_' + i + '" value="' + price + '" style="text-align:center;">'
           + '</div>';
      });
      grid.innerHTML = h;
    }
  }
}

function navTo(id) {
  document.querySelectorAll('.page').forEach(function(p) { p.style.display = 'none'; });
  var t = document.getElementById(id);
  if (t) t.style.display = 'block';
  document.querySelectorAll('.nav-btn').forEach(function(b) { b.classList.remove('active'); });
  document.querySelectorAll('.nav-btn[data-go]').forEach(function(b) {
    if (b.dataset.go === id) b.classList.add('active');
  });
  if (window.innerWidth <= 768) toggleSidebar();
  if (id === 'logs') loadLogs();
  if (id === 'saleslog-section') loadSalesLog();
  var cfgMap = {
    'timing-config':   populateTiming,
    'telegram-config': populateTelegram,
    'display-config':  populateDisplay,
    'sumup-config':    populateSumUp,
    'network-config':  populateNetwork,
    'payment-config':  populatePayment,
    'slots-config':    populateSlots
  };
  if (cfgMap[id]) loadConfig(cfgMap[id]);
}

function toggleSidebar() {
  document.querySelector('.sidebar').classList.toggle('open');
  document.querySelector('.overlay').classList.toggle('open');
}

function applyNetMode(useStatic) {
  var sf    = document.getElementById('staticFields');
  var btnD  = document.getElementById('btnDhcp');
  var btnS  = document.getElementById('btnStatic');
  var ipInp = document.getElementById('inp_static_ip');
  if (useStatic) {
    if (sf)   sf.style.display = '';
    if (btnS) { btnS.style.background='var(--brand)';   btnS.style.color='#000'; btnS.style.borderColor='var(--brand)'; }
    if (btnD) { btnD.style.background='transparent';    btnD.style.color='var(--text-sec)'; btnD.style.borderColor='var(--border)'; }
  } else {
    if (sf)    sf.style.display = 'none';
    if (ipInp) ipInp.value = '';
    if (btnD) { btnD.style.background='var(--success)'; btnD.style.color='#000'; btnD.style.borderColor='var(--success)'; }
    if (btnS) { btnS.style.background='transparent';    btnS.style.color='var(--text-sec)'; btnS.style.borderColor='var(--border)'; }
  }
}

function applyBulk() {
  var v = document.getElementById('bulkPrice').value;
  if (!v || isNaN(parseFloat(v))) return;
  var val = parseFloat(v).toFixed(2);
  document.querySelectorAll('#priceForm input[type=number]').forEach(function(el) { el.value = val; });
}

function loadLogs() {
  var con = document.getElementById('log-output');
  if (!con) return;
  fetch('/logdata').then(function(r) { return r.text(); }).then(function(t) {
    con.textContent = t; con.scrollTop = con.scrollHeight;
  });
}

function checkUpdate() {
  var status  = document.getElementById('online-update-status');
  var table   = document.getElementById('update-version-table');
  var log     = document.getElementById('update-changelog');
  var btn     = document.getElementById('full-update-btn');
  if (status) { status.style.color = 'var(--text-sec)'; status.innerText = 'Verbinde zu hanimat.at...'; }
  if (table)  table.style.display = 'none';
  if (log)    log.style.display   = 'none';
  if (btn)    btn.style.display   = 'none';

  fetch('/check-online-update')
    .then(function(r) {
      if (!r.ok) return r.text().then(function(t) { throw new Error(t); });
      return r.json();
    })
    .then(function(d) {
      if (table) {
        var tag = function(val, ok) {
          return '<span style="color:' + (ok ? 'var(--brand)' : '#4CAF50') + ';font-weight:700;">' + val + '</span>';
        };
        document.getElementById('uv-fw-inst').innerHTML   = d.fw_installed  || '-';
        document.getElementById('uv-fw-online').innerHTML = tag(d.fw_online || '-', d.fw_needs_update);
        document.getElementById('uv-fs-inst').innerHTML   = d.fs_installed  || '(unbekannt)';
        document.getElementById('uv-fs-online').innerHTML = tag(d.fs_online || '-', d.fs_needs_update);
        table.style.display = 'block';
      }
      if (log && d.changelog) {
        log.innerText      = d.changelog;
        log.style.display  = 'block';
      }
      if (d.update_available) {
        if (status) { status.style.color = 'var(--brand)'; status.innerText = 'Update verfügbar!'; }
        if (btn)    btn.style.display = 'inline-block';
      } else {
        if (status) { status.style.color = '#4CAF50'; status.innerText = 'Alles aktuell.'; }
      }
    })
    .catch(function(e) {
      if (status) { status.style.color = '#e74c3c'; status.innerText = 'Fehler: ' + e.message; }
    });
}

function setUploadType(radio) {
  var form = document.getElementById('ota-upload-form');
  if (form) form.action = radio.value === 'fs' ? '/ota-upload-fs' : '/ota-upload';
}

function startFullUpdate() {
  var status = document.getElementById('online-update-status');
  var btn    = document.getElementById('full-update-btn');
  if (btn)    btn.disabled = true;
  if (status) { status.style.color = 'var(--brand)'; status.innerText = 'Update läuft... Gerät startet neu. Bitte warten.'; }
  fetch('/start-full-update', { method: 'POST' })
    .then(function(r) { return r.text(); })
    .then(function(t) {
      if (status) status.innerText = t;
    })
    .catch(function(e) {
      if (status) { status.style.color = '#e74c3c'; status.innerText = 'Fehler: ' + e.message; }
      if (btn)    btn.disabled = false;
    });
}

function loadSalesLog() {
  fetch('/api/status')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      var el;
      el = document.getElementById('sl-revenue');
      if (el) el.textContent = (d.totalRevenueEur || '0.00') + ' €';
      el = document.getElementById('sl-total-sales');
      if (el) el.textContent = d.totalSales !== undefined ? d.totalSales : '-';
      el = document.getElementById('sl-cashbox');
      if (el) el.textContent = (d.cashBoxEur || '0.00') + ' €';
    })
    .catch(function() {});
  fetch('/saleslog')
    .then(function(r) { return r.json(); })
    .then(function(data) {
      var tbody   = document.getElementById('sales-tbody');
      var empty   = document.getElementById('sales-empty');
      var slCash  = document.getElementById('sl-cash');
      var slCard  = document.getElementById('sl-card');
      var slLabel = document.getElementById('sl-count-label');
      if (!tbody) return;
      var cashCount = 0, cardCount = 0;
      (data || []).forEach(function(e) {
        if (e.method === 'SUMUP') cardCount++; else cashCount++;
      });
      if (slCash) slCash.textContent = cashCount;
      if (slCard) slCard.textContent = cardCount;
      if (slLabel) slLabel.textContent = data && data.length ? '(' + data.length + ' Einträge)' : '';
      if (!data || !data.length) { tbody.innerHTML=''; if(empty) empty.style.display='block'; return; }
      if (empty) empty.style.display = 'none';
      var html = '';
      Array.prototype.slice.call(data).reverse().forEach(function(e, i) {
        var badge = e.method==='SUMUP'
          ? '<span style="background:rgba(0,180,120,0.15);color:#00b478;padding:2px 10px;border-radius:12px;font-size:0.75rem;font-weight:700;">Karte</span>'
          : '<span style="background:rgba(255,159,28,0.15);color:var(--brand);padding:2px 10px;border-radius:12px;font-size:0.75rem;font-weight:700;">Bar</span>';
        var bg = i%2===0?'rgba(255,255,255,0.025)':'transparent';
        html += '<tr style="border-bottom:1px solid var(--border);background:'+bg+';">'
          + '<td style="padding:8px 10px;font-size:0.83rem;color:var(--text-sec);">'+e.time+'</td>'
          + '<td style="padding:8px 10px;text-align:center;"><span style="background:rgba(255,255,255,0.08);border-radius:8px;padding:2px 9px;font-weight:700;">#'+e.slot+'</span></td>'
          + '<td style="padding:8px 10px;text-align:right;font-weight:700;color:var(--success);">'+e.price+' &euro;</td>'
          + '<td style="padding:8px 10px;text-align:center;">'+badge+'</td></tr>';
      });
      tbody.innerHTML = html;
    })
    .catch(function() {
      var tbody = document.getElementById('sales-tbody');
      if (tbody) tbody.innerHTML = '<tr><td colspan="4" style="padding:1rem;color:var(--danger);text-align:center;">Fehler beim Laden.</td></tr>';
    });
}

function loadDashboardData() {
  fetch('/api/status')
    .then(function(r) { return r.json(); })
    .then(function(data) {
      var el;
      el = document.getElementById('dyn-credit');
      if (el) el.textContent = data.creditEur || '-';
      el = document.getElementById('dyn-version');
      if (el) el.textContent = data.version || '-';
      el = document.getElementById('dyn-fs-version');
      if (el) el.textContent = data.fsVersion || '-';
      el = document.getElementById('dyn-uptime');
      if (el) el.textContent = data.uptime || '-';
      el = document.getElementById('dyn-ip');
      if (el) el.textContent = data.ip || '-';
      el = document.getElementById('dyn-ip-net');
      if (el) el.textContent = data.ip || '-';

      // Heap mit Farb-Klasse
      var heapKb = data.heapKb || 0;
      el = document.getElementById('dyn-heap');
      if (el) {
        el.textContent = heapKb;
        el.className = heapKb >= 60 ? 'stat-heap-ok' : (heapKb >= 30 ? 'stat-heap-warn' : 'stat-heap-crit');
      }
      el = document.getElementById('dyn-heap-min');
      if (el) el.textContent = data.minHeapKb || '-';

      // Absturzzähler & Reset-Grund
      var cc = data.crashCount || 0;
      el = document.getElementById('dyn-crash-count');
      if (el) el.textContent = cc;
      el = document.getElementById('dyn-reset-val');
      if (el) {
        el.textContent = data.resetReason || '-';
        el.className = 'stat-val ' + (data.unexpectedReset ? 'stat-heap-crit' : 'stat-heap-ok');
      }
      el = document.getElementById('dyn-crash-reset');
      if (el) el.style.display = cc > 0 ? 'block' : 'none';

      // Kassenstand
      el = document.getElementById('dyn-cashbox');
      if (el) el.textContent = data.cashBoxEur || '0.00';

      // Verfügbare Fächer
      if (data.slots) {
        var avail = 0;
        data.slots.forEach(function(s) { if (s.available && !s.locked) avail++; });
        el = document.getElementById('dyn-slots');
        if (el) el.textContent = avail + '/' + (data.activeSlots || data.slots.length);
      }

      // Slot-Karten rendern
      var grid = document.getElementById('slots-grid');
      if (grid && data.slots) {
        var h = '';
        data.slots.forEach(function(s, i) {
          var bc = s.locked ? 'b-lock' : (s.available ? 'b-ok' : 'b-empty');
          var st = s.locked ? 'Gesperrt' : (s.available ? 'Bereit' : 'Leer');
          var li = s.locked ? '&#128274;' : '&#128275;';
          h += '<div class="slot-card">'
             + '<div class="slot-header">'
             + '<div><div class="slot-title">Fach #' + (i + 1) + '</div>'
             + '<div class="slot-price">' + s.price + ' €</div></div>'
             + '<span class="badge ' + bc + '">' + st + '</span>'
             + '</div>'
             + '<div class="slot-controls">'
             + '<form action="/toggleslotlock" method="post" style="display:contents;">'
             + '<input type="hidden" name="slot" value="' + i + '">'
             + '<button type="submit" class="icon-btn" title="Sperren/Entsperren">' + li + '</button></form>'
             + '<form action="/triggerrelay" method="post" style="display:contents;">'
             + '<input type="hidden" name="slot" value="' + i + '">'
             + '<button type="submit" class="icon-btn btn-test" title="Relais Test">&#9889;</button></form>'
             + '<form action="/refill" method="post" style="display:contents;">'
             + '<input type="hidden" name="slot" value="' + i + '">'
             + '<button type="submit" class="icon-btn btn-refill" title="Auffüllen">&#128260;</button></form>'
             + '</div></div>';
        });
        grid.innerHTML = h;
      }
    })
    .catch(function(e) { console.error('loadDashboardData error:', e); });
}

function applyTheme(light) {
  document.documentElement.classList.toggle('light', light);
  var btn = document.getElementById('theme-toggle');
  if (btn) btn.textContent = light ? '🌙 Dunkel' : '☀️ Hell';
}

function toggleTheme() {
  var next = !document.documentElement.classList.contains('light');
  try { localStorage.setItem('hanimat-theme', next ? 'light' : 'dark'); } catch(e) {}
  applyTheme(next);
}

function bindAll() {
  document.querySelectorAll('[data-go]').forEach(function(btn) {
    btn.onclick = function() { navTo(btn.dataset.go); };
  });
  document.querySelectorAll('form[data-confirm]').forEach(function(form) {
    form.onsubmit = function(e) {
      if (!confirm(form.dataset.confirm)) { e.preventDefault(); return false; }
    };
  });
  document.querySelectorAll('[data-action]').forEach(function(el) {
    el.onclick = function() {
      var act = el.dataset.action;
      if (act === 'toggle-menu')       toggleSidebar();
      else if (act === 'apply-bulk')   applyBulk();
      else if (act === 'set-net')      applyNetMode(el.dataset.mode === 'static');
      else if (act === 'check-update') checkUpdate();
      else if (act === 'full-update')  startFullUpdate();
    };
  });
  document.querySelectorAll('.info-icon').forEach(function(icon) {
    icon.onclick = function(e) { if (e) e.stopPropagation(); icon.classList.toggle('open'); };
  });

  try { applyTheme(localStorage.getItem('hanimat-theme') === 'light'); } catch(e) {}
  var themeBtn = document.getElementById('theme-toggle');
  if (themeBtn) themeBtn.onclick = toggleTheme;

  document.querySelectorAll('form').forEach(function(f) {
    f.addEventListener('submit', function() { configCache = null; });
  });

  var hash = window.location.hash.substring(1);
  navTo(hash || 'dashboard');
  if (document.getElementById('log-output')) setInterval(loadLogs, 3000);
  loadDashboardData();
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', bindAll);
} else {
  bindAll();
}
})();
