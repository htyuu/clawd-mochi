'use strict';

document.addEventListener('DOMContentLoaded', () => {
  // ─── State (module-scoped, simple) ───────────────────
  let currentColor   = '#ff6b35';
  let currentEmote   = 'normal';
  let liveState      = 'idle';     // last state from daemon
  let manualOverride = false;       // true while replay/ritual is overriding live sync
  let tlData         = { empty: true, buckets: [] };
  let tlDate         = new Date().toISOString().slice(0, 10);
  let currentRange   = 'week';
  let hoverTs        = null;

  const $   = id => document.getElementById(id);
  const warn = (where, e) => console.warn('[' + where + ']', e && (e.message || e));

  // ─── State → visuals map ─────────────────────────────
  const STATE_COLORS = {
    idle:     '#ff6b35',
    thinking: '#2563eb',
    done:     '#10b981',
    awaiting: '#eab308',
    error:    '#ef4444',
    unknown:  '#64748b',
  };
  const STATE_EMOTES = {
    idle:     'normal',
    thinking: 'normal',
    done:     'squish',
    awaiting: 'normal',
    error:    'error',
    unknown:  'tired',
  };
  const STATE_LABELS = {
    idle:     '空闲',
    thinking: '思考中',
    done:     '完成',
    awaiting: '等待中',
    error:    '错误',
    unknown:  '未知',
  };

  // ─── Clawd face renderer ─────────────────────────────
  function setClawdFace(emote) {
    currentEmote = emote;
    const l = $('eye-l'), r = $('eye-r'), m = $('mouth');
    l.className = 'eye eye-l';
    r.className = 'eye eye-r';
    m.className = 'mouth';
    switch (emote) {
      case 'normal': break;
      case 'squish': l.classList.add('eye-squish'); r.classList.add('eye-squish'); m.classList.add('mouth-smile'); break;
      case 'error':  l.classList.add('eye-x');      r.classList.add('eye-x'); break;
      case 'tired':  l.classList.add('eye-tired');  r.classList.add('eye-tired'); break;
    }
  }

  function setClawdColor(color) {
    currentColor = color;
    $('clawd-screen').className = 'clawd-screen';
    $('clawd-screen').style.background = color;
  }

  function applyLiveState(state) {
    if (manualOverride) return;
    liveState = state;
    setClawdColor(STATE_COLORS[state] || STATE_COLORS.idle);
    setClawdFace(STATE_EMOTES[state] || 'normal');
    $('clawd-state-label').textContent = STATE_LABELS[state] || state;
  }

  // ─── Tab switching ───────────────────────────────────
  document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
      btn.classList.add('active');
      $('tab-' + btn.dataset.tab).classList.add('active');

      const tab = btn.dataset.tab;
      if (tab === 'today')    loadToday();
      if (tab === 'year')     loadYear(currentRange);
      if (tab === 'ritual')   loadRituals();
      if (tab === 'timeline') { resizeCanvas(); loadTimeline(); }
    });
  });

  // ─── Status polling (every 2s) ───────────────────────
  // Debounce daemon-down detection: a single slow /health (daemon busy with
  // token globbing, a git subprocess, or an ESP32 push storm) used to flip
  // the "daemon 未连接" banner on for ~1-2s then off again. We require
  // DOWN_THRESHOLD consecutive failures before declaring the daemon down,
  // and any single success resets it. Polls at 2s, so threshold of 3 ≈ 6s of
  // sustained unreachability before the banner shows.
  const DOWN_THRESHOLD = 3;
  let downStreak = 0;

  async function pollStatus() {
    try {
      const res = await fetch('/api/status');
      const s = await res.json();
      const indicator = $('status-indicator');
      const text = $('status-text');
      const banner = $('error-banner');

      if (!s.daemon) {
        downStreak++;
        if (downStreak >= DOWN_THRESHOLD) {
          indicator.className = 'status-down';
          text.textContent = 'daemon ❌';
          banner.classList.add('show');
          $('error-text').textContent = 'daemon 未连接，遥控暂时无法工作（颜色按钮仍可用于本地预览）';
          // Disable only daemon-dependent buttons (emotes/presets/rituals).
          // Color buttons stay enabled — they preview locally.
          document.querySelectorAll('.emo-btn[data-emote], .preset-btn, .ritual-btn')
            .forEach(b => { b.disabled = true; b.style.opacity = '.5'; });
        }
        return;
      }

      // Daemon reachable — reset streak, hide banner.
      downStreak = 0;
      indicator.className = 'status-ok';
      text.textContent = 'daemon ✅';
      banner.classList.remove('show');
      document.querySelectorAll('.emo-btn[data-emote], .preset-btn, .ritual-btn')
        .forEach(b => { b.disabled = false; b.style.opacity = ''; });

      // Sync virtual Clawd to inferred daemon state
      applyLiveState(s.state || 'idle');

      // Show last tool + session info in subtitle
      if (s.last_tool) {
        $('clawd-project').textContent = ' · ' + s.last_tool;
      } else {
        $('clawd-project').textContent = '';
      }
    } catch (e) {
      warn('pollStatus', e);
      // Network error to our own cockpit server — don't flip the daemon
      // banner on a single miss either; the streak stays where it was.
      downStreak++;
      if (downStreak >= DOWN_THRESHOLD) {
        $('status-indicator').className = 'status-down';
        $('status-text').textContent = 'daemon ❌';
      }
    }
  }

  // ─── Today tab data ──────────────────────────────────
  async function loadToday() {
    setLoading('tool-rank', '加载中…');
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
    } catch (e) {
      warn('loadToday', e);
      $('tool-rank').innerHTML = '<li style="color:#ef4444;font-size:13px;">加载失败</li>';
    }
  }

  function setLoading(id, text) {
    const el = $(id);
    if (el && !el.innerHTML) el.innerHTML = '<li style="color:#94a3b8;font-size:13px;">' + text + '</li>';
  }

  // ─── Emote buttons ───────────────────────────────────
  document.querySelectorAll('#emote-btns .emo-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const emote = btn.dataset.emote;
      setClawdFace(emote);
      const state = emote === 'error' ? 'error' : 'idle';
      fetch('/api/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state, tool: 'emote:' + emote }),
      }).catch(e => warn('control:emote', e));
    });
  });

  // ─── Color buttons ───────────────────────────────────
  document.querySelectorAll('#color-btns .emo-btn').forEach(btn => {
    btn.addEventListener('click', () => setClawdColor(btn.dataset.color));
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
          setClawdColor(btn.dataset.color);
          const ctlState = btn.dataset.state || 'idle';
          fetch('/api/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ state: ctlState, tool: 'preset:' + btn.textContent }),
          }).catch(e => warn('control:preset', e));
        });
      });
    } catch (e) {
      warn('loadPresets', e);
    }
  }

  // ─── Rituals ─────────────────────────────────────────
  // Beijing calendar day as YYYY-MM-DD. new Date() is local but toISOString()
  // is UTC, so we shift +8h before slicing — matches the backend's
  // date(created_at, '+8 hours') so a ritual logged at Beijing 06:00 is
  // grouped under the Beijing day the user experienced.
  function beijingDay(offsetDays = 0) {
    const t = new Date(Date.now() + 8 * 3600 * 1000 + offsetDays * 86400 * 1000);
    return t.toISOString().slice(0, 10);
  }

  async function loadRituals() {
    try {
      const [r1, r2] = await Promise.all([fetch('/api/rituals'), fetch('/api/rituals/streak')]);
      const d = await r1.json();
      const s = await r2.json();

      const today = beijingDay();
      const todays = (d.days && d.days[today]) || [];
      ['morning', 'lunch', 'night', 'offwork'].forEach(type => {
        const done = todays.find(r => r.type === type);
        $('rit-' + type).textContent = done ? '✓ ' + done.time : '';
        document.querySelector(`.ritual-btn[data-ritual="${type}"]`).classList.toggle('done', !!done);
      });

      $('streak-morning').textContent = s.morning || 0;
      $('streak-night').textContent = s.night || 0;
      $('streak-all').textContent = s.all_four || 0;

      // Heatmap (last 90 days, Beijing calendar)
      const cells = [];
      for (let i = 89; i >= 0; i--) {
        const ds = beijingDay(-i);
        const r = (d.days && d.days[ds]) || [];
        const cnt = Math.min(4, r.length);
        cells.push(`<div class="cell cell-${cnt}" title="${ds}: ${cnt} 仪式 (${r.map(x => x.type).join(', ')})"></div>`);
      }
      $('heatmap').innerHTML = cells.join('');
    } catch (e) {
      warn('loadRituals', e);
    }
  }

  /** Run a multi-step ritual sequence on the virtual Clawd + forward to daemon. */
  function runRitualSequence(type, cfg) {
    if (!cfg) return;
    manualOverride = true;

    // Apply preset
    setClawdColor(cfg.color);
    setClawdFace(cfg.emote);

    // Push state to daemon
    fetch('/api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ state: cfg.state || 'idle', tool: 'ritual:' + type }),
    }).catch(e => warn('control:ritual', e));

    // Flash effect (morning/offwork)
    if (cfg.flash) {
      let n = 0;
      const flashColor = '#fef3c7';
      const baseColor  = cfg.color;
      const id = setInterval(() => {
        $('clawd-screen').style.background = (n % 2 === 0) ? flashColor : baseColor;
        n++;
        if (n >= 6) { clearInterval(id); $('clawd-screen').style.background = baseColor; }
      }, 200);
    }

    // Release override after 5s so live polling resumes
    setTimeout(() => { manualOverride = false; }, 5000);
  }

  document.querySelectorAll('.ritual-btn').forEach(btn => {
    btn.addEventListener('click', async () => {
      const type = btn.dataset.ritual;
      try {
        const res = await fetch('/api/rituals', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ type }),
        });
        const d = await res.json();
        runRitualSequence(type, d.config);
        loadRituals();
      } catch (e) {
        warn('ritual:click', e);
      }
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
    } catch (e) {
      warn('loadTimeline', e);
    }
  }

  function bucketColor(b) {
    // Color by dominant state: error red / busy blue / idle orange (sparse)
    if (b.errors > 0) return '#ef4444';
    if (b.count >= 5) return '#2563eb';
    return '#ff6b35';
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
    const maxTokens = Math.max(...buckets.map(b => b.tokens || 0), 1);
    const barW = Math.max(4, Math.min(14, (w - 40) / Math.max(1, buckets.length)));
    buckets.forEach((b, i) => {
      const x = 20 + i * (barW + 2);
      const barH = (b.count / max) * (h - 40);
      ctx.fillStyle = bucketColor(b);
      ctx.fillRect(x, h - 15 - barH, barW, barH);
      // Token consumption cap (green) stacked on top of the count bar —
      // height proportional to this bucket's tokens vs the day's max.
      if (b.tokens > 0) {
        const tokH = (b.tokens / maxTokens) * 18;
        ctx.fillStyle = 'rgba(34, 197, 94, 0.9)';
        ctx.fillRect(x, h - 15 - barH - tokH, barW, tokH);
      }
      if (hoverTs === b.ts) {
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        const totalH = barH + (b.tokens > 0 ? (b.tokens / maxTokens) * 18 : 0);
        ctx.strokeRect(x - 1, h - 16 - totalH, barW + 2, totalH + 2);
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
        $('tl-tokens').textContent = b.tokens > 0
          ? (b.tokens / 1000).toFixed(b.tokens >= 10000 ? 0 : 1) + 'k'
          : '0';
        // Enable replay button now that we have a target bucket selected
        $('tl-replay').disabled = false;
        $('tl-replay').dataset.state = b.state || (b.errors > 0 ? 'error' : 'thinking');
        return;
      }
    }
  });

  tlCanvas.addEventListener('mouseleave', () => { hoverTs = null; drawTimeline(); });

  // Replay button: forward selected bucket's state to daemon for ~5s
  $('tl-replay').addEventListener('click', async () => {
    const state = $('tl-replay').dataset.state;
    if (!state) return;
    manualOverride = true;
    $('tl-replay').disabled = true;
    $('tl-replay').textContent = '重现中…(5s)';
    setClawdColor(STATE_COLORS[state] || STATE_COLORS.idle);
    setClawdFace(STATE_EMOTES[state] || 'normal');
    try {
      await fetch('/api/replay', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state }),
      });
    } catch (e) { warn('replay', e); }
    setTimeout(() => {
      manualOverride = false;
      $('tl-replay').disabled = false;
      $('tl-replay').textContent = '⏪ 重现此刻';
    }, 5000);
  });

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
    } catch (e) {
      warn('loadYear', e);
    }
  }

  // ── Comment pools: 4 categories × 5 templates = 20 lines, pick by sum hash ──
  const COMMENT_POOLS = {
    overworked: [
      '这%P%累了。共 %C% 次调用，Clawd 心疼。',
      '这%P% Clawd 看你按键盘按到飞起。',
      '%C% 次调用…你的手腕还好吗？',
      '这%P%燃烧得很认真。Clawd 端水来了。',
      '高强度输出。Clawd 给你别上一朵小红花。',
    ],
    chill: [
      '清闲的%P%。Clawd 也在摸鱼。',
      '这%P%安静得连螃蟹都打了个哈欠。',
      '低负载状态。Clawd 偷偷睡了。',
      '这%P%好像很佛系。也挺好。',
      'Clawd 闲到开始数自己腿了。',
    ],
    error_prone: [
      '这%P%错误率有点高。Clawd 担心。',
      '红色警报闪了好几次。要不歇会儿？',
      'Bug 比想象中顽固，Clawd 给你打气。',
      '错误也是进度的一部分。Clawd 这么觉得。',
      '失败 ≥5%。但你还在跑。Clawd 看见了。',
    ],
    token_burner: [
      'Token 烧得猛。该重启会话了。',
      '上下文已经塞得快撑爆。Clawd 提醒一下。',
      '消耗惊人。建议给 Claude 倒杯水。',
      '%T%k token …深度对话型选手。',
      'Token 用量逼近上限。压缩一下吧？',
    ],
    steady: [
      '稳稳的%P%。Clawd 满意。',
      '节奏感不错。继续保持。',
      '正常发挥。Clawd 给个 OK 手势。',
      '这%P%很 chill。',
      '平稳的输出曲线。Clawd 看得很舒服。',
    ],
  };

  function comment(range, sum) {
    const period = range === 'week' ? '周' : range === 'month' ? '月' : '年';
    const calls = sum.tools_called || 0;
    const errs  = sum.errors || 0;
    const toks  = sum.tokens_total || 0;

    let pool;
    if (calls > 500) pool = COMMENT_POOLS.overworked;
    else if (calls < 10) pool = COMMENT_POOLS.chill;
    else if (calls && errs / calls > 0.05) pool = COMMENT_POOLS.error_prone;
    else if (toks > 150000) pool = COMMENT_POOLS.token_burner;
    else pool = COMMENT_POOLS.steady;

    // Stable pick based on the period's numbers (so same week always picks same line)
    const idx = (calls + errs + Math.floor(toks / 1000)) % pool.length;
    return pool[idx]
      .replace('%P%', period)
      .replace('%C%', calls)
      .replace('%T%', Math.round(toks / 1000));
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
