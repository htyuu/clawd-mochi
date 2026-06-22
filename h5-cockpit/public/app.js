'use strict';

document.addEventListener('DOMContentLoaded', () => {
  // ─── State (module-scoped, simple) ───────────────────
  let currentColor = '#ff6b35';
  let tlData = { empty: true, buckets: [] };
  let tlDate = new Date().toISOString().slice(0, 10);
  let currentRange = 'week';
  let hoverTs = null;

  const $ = id => document.getElementById(id);

  // ─── Tab switching ───────────────────────────────────
  document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
      btn.classList.add('active');
      $('tab-' + btn.dataset.tab).classList.add('active');

      // Refresh data on tab activation
      const tab = btn.dataset.tab;
      if (tab === 'today')    loadToday();
      if (tab === 'year')     loadYear(currentRange);
      if (tab === 'ritual')   loadRituals();
      if (tab === 'timeline') { resizeCanvas(); loadTimeline(); }
    });
  });

  // ─── Status polling (every 2s) ───────────────────────
  async function pollStatus() {
    try {
      const res = await fetch('/api/status');
      const s = await res.json();
      const indicator = $('status-indicator');
      const text = $('status-text');
      const banner = $('error-banner');

      if (!s.daemon) {
        indicator.className = 'status-down';
        text.textContent = 'daemon ❌';
        banner.classList.add('show');
        $('error-text').textContent = 'daemon 未连接，遥控暂时无法工作';
        document.querySelectorAll('.emo-btn, .preset-btn').forEach(b => { b.disabled = true; b.style.opacity = '.5'; });
        return;
      }

      indicator.className = 'status-ok';
      text.textContent = 'daemon ✅';
      banner.classList.remove('show');
      document.querySelectorAll('.emo-btn, .preset-btn').forEach(b => { b.disabled = false; b.style.opacity = ''; });

      // Reflect activity in virtual Clawd
      const recentlyActive = s.last_ts && (Date.now() / 1000 - s.last_ts) < 60;
      if (recentlyActive) {
        $('clawd-state-label').textContent = '思考中';
        $('clawd-screen').className = 'clawd-screen state-thinking';
      } else {
        $('clawd-state-label').textContent = '空闲';
        $('clawd-screen').className = 'clawd-screen';
        $('clawd-screen').style.background = currentColor;
      }
    } catch {
      $('status-indicator').className = 'status-down';
      $('status-text').textContent = 'daemon ❌';
    }
  }

  // ─── Today tab data ──────────────────────────────────
  async function loadToday() {
    try {
      const res = await fetch('/api/today');
      const d = await res.json();
      if (d.empty) {
        $('stat-tools').textContent = '0';
        $('stat-tokens').textContent = '0';
        $('stat-sessions').textContent = '0';
        $('stat-errors').textContent = '0';
        $('tool-rank').innerHTML = '<li style="color:#64748b;font-size:13px;">暂无数据</li>';
        return;
      }
      $('stat-tools').textContent = d.tools_called;
      $('stat-tokens').textContent = d.tokens_total >= 1000
        ? Math.round(d.tokens_total / 1000) + 'k' : d.tokens_total;
      $('stat-sessions').textContent = d.sessions;
      $('stat-errors').textContent = d.errors;
      const pct = Math.min(100, Math.round(d.tokens_total / 2000));
      $('token-bar').style.width = pct + '%';

      const top = d.top_tools || [];
      if (top.length === 0) {
        $('tool-rank').innerHTML = '<li style="color:#64748b;font-size:13px;">暂无数据</li>';
      } else {
        const max = top[0].count || 1;
        $('tool-rank').innerHTML = top.slice(0, 8).map(t => `
          <li>
            <span style="min-width:60px">${escapeHtml(t.name || '?')}</span>
            <div class="bar"><div class="bar-fill" style="width:${Math.round(t.count / max * 100)}%"></div></div>
            <span class="cnt">${t.count}</span>
          </li>`).join('');
      }
    } catch { /* silent */ }
  }

  // ─── Emote buttons ───────────────────────────────────
  function setClawdFace(emote) {
    const l = $('eye-l'), r = $('eye-r'), m = $('mouth');
    l.className = 'eye eye-l';
    r.className = 'eye eye-r';
    m.className = 'mouth';
    switch (emote) {
      case 'normal': break; // default
      case 'squish': l.classList.add('eye-squish'); r.classList.add('eye-squish'); m.classList.add('mouth-smile'); break;
      case 'error':  l.classList.add('eye-x');      r.classList.add('eye-x'); break;
      case 'tired':  l.classList.add('eye-tired');  r.classList.add('eye-tired'); break;
    }
  }

  document.querySelectorAll('#emote-btns .emo-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const emote = btn.dataset.emote;
      setClawdFace(emote);
      const state = emote === 'error' ? 'error' : 'idle';
      fetch('/api/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state, tool: 'emote:' + emote }),
      });
    });
  });

  // ─── Color buttons ───────────────────────────────────
  document.querySelectorAll('#color-btns .emo-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      currentColor = btn.dataset.color;
      $('clawd-screen').className = 'clawd-screen';
      $('clawd-screen').style.background = currentColor;
    });
  });

  // ─── Presets ─────────────────────────────────────────
  async function loadPresets() {
    try {
      const res = await fetch('/api/presets');
      const presets = await res.json();
      $('preset-btns').innerHTML = presets.map(p =>
        `<button class="preset-btn" data-id="${p.id}" data-state="${escapeAttr(p.state)}" data-color="${escapeAttr(p.color)}">${escapeHtml(p.name)}</button>`
      ).join('');
      document.querySelectorAll('#preset-btns .preset-btn').forEach(btn => {
        btn.addEventListener('click', () => {
          currentColor = btn.dataset.color;
          $('clawd-screen').className = 'clawd-screen';
          $('clawd-screen').style.background = currentColor;
          const stateMap = { error: 'error', awaiting: 'awaiting', done: 'done' };
          const ctlState = stateMap[btn.dataset.state] || btn.dataset.state || 'idle';
          fetch('/api/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ state: ctlState, tool: 'preset:' + btn.textContent }),
          });
        });
      });
    } catch { /* silent */ }
  }

  // ─── Rituals ─────────────────────────────────────────
  async function loadRituals() {
    try {
      const [r1, r2] = await Promise.all([fetch('/api/rituals'), fetch('/api/rituals/streak')]);
      const d = await r1.json();
      const s = await r2.json();

      const today = new Date().toISOString().slice(0, 10);
      const todays = (d.days && d.days[today]) || [];
      ['morning', 'lunch', 'night', 'offwork'].forEach(type => {
        const done = todays.find(r => r.type === type);
        $('rit-' + type).textContent = done ? '✓ ' + done.time : '';
        document.querySelector(`.ritual-btn[data-ritual="${type}"]`).classList.toggle('done', !!done);
      });

      $('streak-morning').textContent = s.morning || 0;
      $('streak-night').textContent = s.night || 0;
      $('streak-all').textContent = s.all_four || 0;

      // Heatmap (last 90 days)
      const cells = [];
      for (let i = 89; i >= 0; i--) {
        const dt = new Date();
        dt.setDate(dt.getDate() - i);
        const ds = dt.toISOString().slice(0, 10);
        const r = (d.days && d.days[ds]) || [];
        const cnt = Math.min(4, r.length);
        cells.push(`<div class="cell cell-${cnt}" title="${ds}: ${cnt} 仪式 (${r.map(x => x.type).join(', ')})"></div>`);
      }
      $('heatmap').innerHTML = cells.join('');
    } catch { /* silent */ }
  }

  document.querySelectorAll('.ritual-btn').forEach(btn => {
    btn.addEventListener('click', async () => {
      const type = btn.dataset.ritual;
      try {
        await fetch('/api/rituals', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ type }),
        });
        // Push to daemon
        const stateMap = { morning: 'idle', lunch: 'idle', night: 'idle', offwork: 'done' };
        fetch('/api/control', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ state: stateMap[type] || 'idle', tool: 'ritual:' + type }),
        });
        loadRituals();
        // Flash effect on Clawd screen
        const scr = $('clawd-screen');
        const orig = scr.style.background;
        scr.style.background = '#eab308';
        setTimeout(() => { scr.style.background = orig || currentColor; }, 300);
      } catch { /* silent */ }
    });
  });

  // ─── Timeline ────────────────────────────────────────
  const tlCanvas = $('tl-canvas');
  const ctx = tlCanvas.getContext('2d');

  function resizeCanvas() {
    tlCanvas.width = tlCanvas.clientWidth;
    tlCanvas.height = tlCanvas.clientHeight;
  }

  async function loadTimeline() {
    try {
      const res = await fetch('/api/timeline?date=' + tlDate);
      tlData = await res.json();
      hoverTs = null;
      resizeCanvas();
      drawTimeline();
    } catch { /* silent */ }
  }

  function drawTimeline() {
    const w = tlCanvas.width, h = tlCanvas.height;
    ctx.clearRect(0, 0, w, h);
    if (tlData.empty || !tlData.buckets || tlData.buckets.length === 0) {
      ctx.fillStyle = '#64748b';
      ctx.font = '14px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('暂无数据', w / 2, h / 2);
      return;
    }
    const buckets = tlData.buckets;
    const max = Math.max(...buckets.map(b => b.count), 1);
    const barW = Math.max(4, Math.min(14, (w - 40) / Math.max(1, buckets.length)));
    buckets.forEach((b, i) => {
      const x = 20 + i * (barW + 2);
      const barH = (b.count / max) * (h - 30);
      ctx.fillStyle = b.errors > 0 ? '#ef4444' : '#2563eb';
      ctx.fillRect(x, h - 15 - barH, barW, barH);
      if (hoverTs === b.ts) {
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.strokeRect(x - 1, h - 16 - barH, barW + 2, barH + 2);
      }
    });
  }

  tlCanvas.addEventListener('mousemove', e => {
    if (tlData.empty || !tlData.buckets || tlData.buckets.length === 0) return;
    const rect = tlCanvas.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const w = tlCanvas.width;
    const buckets = tlData.buckets;
    const barW = Math.max(4, Math.min(14, (w - 40) / Math.max(1, buckets.length)));
    for (let i = 0; i < buckets.length; i++) {
      const x = 20 + i * (barW + 2);
      if (mx >= x && mx <= x + barW + 2) {
        hoverTs = buckets[i].ts;
        drawTimeline();
        const b = buckets[i];
        const t = new Date(b.ts * 1000);
        $('tl-ts').textContent = t.toLocaleString();
        $('tl-tool').textContent = b.top_tool || '--';
        $('tl-count').textContent = b.count;
        $('tl-err').textContent = b.errors;
        return;
      }
    }
  });

  tlCanvas.addEventListener('mouseleave', () => { hoverTs = null; drawTimeline(); });

  const tlInput = $('tl-date');
  tlInput.value = tlDate;
  tlInput.max = new Date().toISOString().slice(0, 10);
  tlInput.addEventListener('change', () => { tlDate = tlInput.value; loadTimeline(); });
  $('tl-prev').addEventListener('click', () => { const d = new Date(tlDate); d.setDate(d.getDate() - 1); tlDate = d.toISOString().slice(0, 10); tlInput.value = tlDate; loadTimeline(); });
  $('tl-next').addEventListener('click', () => {
    const d = new Date(tlDate); d.setDate(d.getDate() + 1);
    const ds = d.toISOString().slice(0, 10);
    if (ds > new Date().toISOString().slice(0, 10)) return;
    tlDate = ds; tlInput.value = tlDate; loadTimeline();
  });
  $('tl-today').addEventListener('click', () => { tlDate = new Date().toISOString().slice(0, 10); tlInput.value = tlDate; loadTimeline(); });

  // ─── Yearbook ────────────────────────────────────────
  async function loadYear(range) {
    currentRange = range || 'week';
    try {
      const res = await fetch('/api/year?range=' + currentRange);
      const d = await res.json();
      if (d.empty || !d.summary) {
        $('yr-tools').textContent = '--';
        $('yr-tokens').textContent = '--';
        $('yr-sessions').textContent = '--';
        $('yr-chart').innerHTML = '<div style="color:#64748b;font-size:13px;align-self:center;">暂无</div>';
        $('yr-top').textContent = '暂无数据';
        $('yr-comment').textContent = '🦀 Clawd 正看着你。';
        return;
      }
      const sum = d.summary;
      $('yr-tools').textContent = sum.tools_called;
      $('yr-tokens').textContent = sum.tokens_total >= 1000
        ? Math.round(sum.tokens_total / 1000) + 'k' : sum.tokens_total;
      $('yr-sessions').textContent = sum.sessions;

      // Trend chart
      const trend = d.trend || [];
      const maxVal = Math.max(...trend.map(t => t.tools), 1);
      $('yr-chart').innerHTML = trend.map(t =>
        `<div style="flex:1;display:flex;flex-direction:column;align-items:center;gap:3px;height:100%;">
           <div class="bar" style="width:80%;height:${Math.max(4, Math.round(t.tools / maxVal * 100))}%"></div>
           <div style="font-size:9px;color:#64748b;">${t.date.slice(5)}</div>
         </div>`
      ).join('');

      $('yr-top').innerHTML = d.top
        ? `🏆 最肝的一天：<span>${d.top.busiest_day}</span> · <span>${d.top.busiest_calls}</span> 次调用`
        : '暂无数据';

      $('yr-comment').textContent = '🦀 ' + comment(currentRange, sum);
    } catch { /* silent */ }
  }

  function comment(range, sum) {
    const period = range === 'week' ? '周' : range === 'month' ? '月' : '年';
    const calls = sum.tools_called || 0;
    const errs = sum.errors || 0;
    if (calls > 500) return `这${period}累了。共 ${calls} 次调用，Clawd 心疼。`;
    if (calls < 10) return `清闲的${period}。Clawd 也在摸鱼。`;
    if (calls && errs / calls > 0.05) return `这${period}错误率有点高。Clawd 担心。`;
    if (sum.tokens_total > 150000) return `Token 烧得猛。该重启会话了。`;
    return `稳稳的${period}。Clawd 满意。`;
  }

  document.querySelectorAll('.year-subtab').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.year-subtab').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      loadYear(btn.dataset.range);
    });
  });

  // ─── Utilities ───────────────────────────────────────
  function escapeHtml(s) { return String(s).replace(/[&<>"']/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c])); }
  function escapeAttr(s) { return String(s).replace(/"/g, '&quot;'); }

  // ─── Init ───────────────────────────────────────────
  pollStatus();
  loadToday();
  loadPresets();
  loadRituals();
  resizeCanvas();
  loadTimeline();
  loadYear('week');

  setInterval(pollStatus, 2000);
  setInterval(loadToday, 15000);
  setInterval(loadRituals, 60000);

  window.addEventListener('resize', () => {
    if ($('tab-timeline').classList.contains('active')) {
      resizeCanvas();
      drawTimeline();
    }
  });
});