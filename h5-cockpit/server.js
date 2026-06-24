const express = require('express');
const path = require('path');

const app = express();
const PORT = 3000;

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

const http = require('http');

const DAEMON_URL = 'http://127.0.0.1:7878';

function daemonFetch(path) {
  return new Promise(resolve => {
    // 2.5s gives headroom for daemon event-loop hitches (token globbing,
    // git subprocess, ESP32 push storms) without making a single poll
    // take longer than the 2s frontend poll interval by too much. The
    // frontend debounces (consecutive-failure threshold) so a lone slow
    // /health no longer flips the "daemon down" banner.
    const req = http.get(DAEMON_URL + path, { timeout: 2500 }, res => {
      let data = '';
      res.on('data', c => data += c);
      res.on('end', () => {
        try { resolve(JSON.parse(data)); }
        catch { resolve(null); }
      });
    });
    req.on('error', () => resolve(null));
    req.on('timeout', () => { req.destroy(); resolve(null); });
  });
}

/**
 * POST a JSON payload to a daemon event endpoint and reply to the cockpit
 * client exactly once.
 *
 * Guards against the double-response crash where a request timeout's
 * postReq.destroy() also fires the 'error' handler — without the guard both
 * would call res.json() and throw ERR_HTTP_HEADERS_SENT, killing the process.
 * reply() runs before destroy() so the client sees "daemon timeout" rather
 * than the less-accurate "daemon unreachable".
 */
function forwardToDaemon(res, endpoint, payload) {
  const url = new URL(DAEMON_URL + endpoint);
  let replied = false;
  const reply = (obj) => {
    if (replied || res.headersSent) return;
    replied = true;
    res.json(obj);
  };
  const postReq = http.request({
    hostname: url.hostname,
    port:     url.port,
    path:     url.pathname,
    method:   'POST',
    headers:  { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) },
    timeout:  1000,
  }, response => {
    let data = '';
    response.on('data', c => data += c);
    response.on('end', () => reply({ ok: true, daemon_response: data }));
  });
  postReq.on('error',   () => reply({ ok: false, error: 'daemon unreachable' }));
  postReq.on('timeout', () => { reply({ ok: false, error: 'daemon timeout' }); postReq.destroy(); });
  postReq.write(payload);
  postReq.end();
}

const Database = require('better-sqlite3');

// --- Cockpit own DB ---
const DB_PATH = path.join(__dirname, 'cockpit.db');
let cockpitDb;
try {
  cockpitDb = new Database(DB_PATH);
  cockpitDb.exec(`
    CREATE TABLE IF NOT EXISTS ritual_logs (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      type       TEXT NOT NULL,
      created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
    CREATE INDEX IF NOT EXISTS idx_ritual_created ON ritual_logs(created_at);

    CREATE TABLE IF NOT EXISTS presets (
      id          INTEGER PRIMARY KEY AUTOINCREMENT,
      name        TEXT NOT NULL UNIQUE,
      state       TEXT NOT NULL,
      color       TEXT NOT NULL,
      bright      INTEGER DEFAULT 255,
      auto_switch INTEGER DEFAULT 1
    );
  `);
  // Seed default presets if table is empty
  const count = cockpitDb.prepare('SELECT COUNT(*) as c FROM presets').get().c;
  if (count === 0) {
    const seed = cockpitDb.prepare(
      'INSERT INTO presets (name, state, color, bright, auto_switch) VALUES (?, ?, ?, ?, ?)'
    );
    seed.run('写代码', 'thinking', '#2B6EB0', 255, 1);
    seed.run('开会',   'idle',     '#6B6B6B', 128, 0);
    seed.run('摸鱼',   'idle',     '#FF8C42', 200, 1);
    console.log('🌱 Seeded 3 default presets');
  }
} catch (e) {
  console.error('⚠️  cockpit.db init failed:', e.message);
  process.exit(1);
}

const os = require('os');
const fs = require('fs');
const daemonDbPath = path.join(os.homedir(), '.local', 'share', 'clawd-daemon', 'clawd.db');

function queryDaemonDb(sql, params = []) {
  try {
    if (!fs.existsSync(daemonDbPath)) return null;
    const db = new Database(daemonDbPath, { readonly: true, fileMustExist: true });
    const rows = db.prepare(sql).all(...params);
    db.close();
    return rows;
  } catch (e) {
    console.error('daemon DB query error:', e.message);
    return null;
  }
}

app.get('/api/today', (req, res) => {
  const startOfDay = new Date();
  startOfDay.setHours(0, 0, 0, 0);
  const ts = Math.floor(startOfDay.getTime() / 1000);

  const totals = queryDaemonDb(
    "SELECT COUNT(*) as total_tools, COALESCE(SUM(tokens),0) as total_tokens, COUNT(DISTINCT session) as sessions, SUM(CASE WHEN success=0 THEN 1 ELSE 0 END) as errors FROM tool_events WHERE ts >= ?",
    [ts]
  );
  if (totals === null) return res.json({ empty: true, message: '暂无数据' });

  const row = totals[0] || { total_tools: 0, total_tokens: 0, sessions: 0, errors: 0 };

  const rows = queryDaemonDb(
    "SELECT tool, COUNT(*) as cnt FROM tool_events WHERE ts >= ? GROUP BY tool ORDER BY cnt DESC",
    [ts]
  ) || [];

  res.json({
    empty: false,
    tools_called: row.total_tools || 0,
    tokens_total: row.total_tokens || 0,
    sessions:     row.sessions || 0,
    errors:       row.errors || 0,
    top_tools:    rows.map(r => ({ name: r.tool, count: r.cnt })),
  });
});

app.get('/api/year', (req, res) => {
  const range = req.query.range || 'week';
  const now = new Date();

  let startDate;
  switch (range) {
    case 'week':  startDate = new Date(now); startDate.setDate(now.getDate() - 6); break;
    case 'month': startDate = new Date(now); startDate.setMonth(now.getMonth() - 1); break;
    case 'year':
    default:      startDate = new Date(now); startDate.setFullYear(now.getFullYear() - 1); break;
  }

  const start = startDate.toISOString().slice(0, 10);

  const rows = queryDaemonDb(
    "SELECT date, tools_called, tokens_total, sessions, errors FROM daily_stats WHERE date >= ? ORDER BY date",
    [start]
  );
  if (rows === null || rows.length === 0)
    return res.json({ empty: true, range, trend: [] });

  const summary = rows.reduce((acc, r) => {
    acc.tools_called += r.tools_called || 0;
    acc.tokens_total += r.tokens_total || 0;
    acc.sessions += r.sessions || 0;
    acc.errors += r.errors || 0;
    return acc;
  }, { tools_called: 0, tokens_total: 0, sessions: 0, errors: 0 });

  const busiest = rows.reduce((a, b) => (a.tools_called >= b.tools_called ? a : b));

  res.json({
    empty: false,
    range,
    summary,
    trend: rows.map(r => ({ date: r.date, tools: r.tools_called || 0, tokens: r.tokens_total || 0 })),
    top: { busiest_day: busiest.date, busiest_calls: busiest.tools_called || 0 },
  });
});

/**
 * Infer Clawd state from the most recent tool_event.
 *  - Last event failed within 15s   → error
 *  - Last event within 5s           → thinking (active)
 *  - Last event within 30s          → done (just finished)
 *  - Otherwise                       → idle
 */
function inferState(lastRow) {
  if (!lastRow) return 'idle';
  const ageSec = Date.now() / 1000 - lastRow.ts;
  if (lastRow.success === 0 && ageSec < 15) return 'error';
  if (ageSec < 5)  return 'thinking';
  if (ageSec < 30) return 'done';
  return 'idle';
}

app.get('/api/status', async (req, res) => {
  const health = await daemonFetch('/health');

  if (!health || !health.ok) {
    return res.json({ daemon: false, esp32: false, state: 'unknown' });
  }

  const lastRows = queryDaemonDb(
    "SELECT ts, tool, success, tokens, session FROM tool_events ORDER BY ts DESC LIMIT 1"
  );

  const stats = queryDaemonDb(
    "SELECT tools_called, tokens_total FROM daily_stats WHERE date = date('now','localtime')"
  );

  const last = lastRows && lastRows[0] ? lastRows[0] : null;
  const state = inferState(last);

  res.json({
    daemon: true,
    esp32: true,
    state,
    last_tool: last ? last.tool : null,
    last_ts:   last ? last.ts : null,
    last_session: last ? last.session : null,
    today_tools:  stats && stats[0] ? stats[0].tools_called : 0,
    today_tokens: stats && stats[0] ? stats[0].tokens_total : 0,
  });
});

/**
 * Replay a historical Clawd state for ~5s, then daemon returns to live.
 * Body: { state: 'thinking'|'done'|'error'|'idle', duration_ms?: 5000 }
 * Cockpit forwards the requested state to daemon; after duration_ms the
 * daemon picks back up the real-time signal on its own (no clear-state API
 * to call, but the next hook event overrides it).
 */
app.post('/api/replay', (req, res) => {
  const { state } = req.body || {};
  if (!state) return res.status(400).json({ error: 'state required' });

  const endpoint = state === 'done' ? '/event/stop' : '/event/pre_tool';
  const payload = state === 'done'
    ? '{}'
    : JSON.stringify({ tool: 'replay:' + state, cwd: process.cwd() });

  forwardToDaemon(res, endpoint, payload);
});

app.post('/api/control', (req, res) => {
  const { state, tool, task } = req.body || {};

  if (!state) return res.status(400).json({ error: 'state required' });

  // Map state to daemon event endpoint
  const endpoint = state === 'done' ? '/event/stop' : '/event/pre_tool';
  const payload = state === 'done'
    ? '{}'
    : JSON.stringify({
        tool: tool || 'manual',
        cwd: process.cwd(),
        task: task || '',
      });

  forwardToDaemon(res, endpoint, payload);
});

app.get('/api/timeline', (req, res) => {
  const dateStr = req.query.date || new Date().toISOString().slice(0, 10);

  // Prevent future dates
  if (dateStr > new Date().toISOString().slice(0, 10))
    return res.json({ empty: true, date: dateStr, buckets: [] });

  const startOfDay = new Date(dateStr + 'T00:00:00');
  const startTs = Math.floor(startOfDay.getTime() / 1000);

  const rows = queryDaemonDb(
    "SELECT ts, tool, success, tokens FROM tool_events WHERE ts >= ? AND ts < ? ORDER BY ts",
    [startTs, startTs + 86400]
  );
  if (rows === null || rows.length === 0)
    return res.json({ empty: true, date: dateStr, buckets: [] });

  // Bucket into 5-min intervals (288 buckets)
  const BUCKET_SEC = 300;
  const buckets = [];
  for (let i = 0; i < 288; i++) {
    buckets.push({ ts: startTs + i * BUCKET_SEC, tools: [], errors: 0, count: 0, tokens: 0 });
  }

  for (const r of rows) {
    const idx = Math.min(287, Math.floor((r.ts - startTs) / BUCKET_SEC));
    buckets[idx].tools.push(r.tool);
    if (!r.success) buckets[idx].errors++;
    buckets[idx].count++;
    buckets[idx].tokens += r.tokens || 0;
  }

  // Compress: only return non-empty buckets
  const nonEmpty = buckets.filter(b => b.count > 0).map(b => {
    // Find the most frequent tool in this bucket
    let topTool = null;
    if (b.tools.length > 0) {
      const counts = {};
      for (const t of b.tools) counts[t] = (counts[t] || 0) + 1;
      topTool = Object.keys(counts).reduce((a, x) => counts[x] > counts[a] ? x : a);
    }
    // Dominant state: error if any failure in bucket, else "thinking" (busy bucket = blue),
    // matches the design doc's mapping (空闲橙 / 工具调用蓝 / 错误红).
    const state = b.errors > 0 ? 'error' : 'thinking';
    return {
      ts:       b.ts,
      count:    b.count,
      errors:   b.errors,
      tokens:   b.tokens,
      top_tool: topTool,
      state,
    };
  });

  res.json({ empty: false, date: dateStr, buckets: nonEmpty });
});

app.get('/api/rituals', (req, res) => {
  const days = parseInt(req.query.days) || 90;
  // Cutoff in Beijing time (UTC+8). created_at is stored in UTC (SQLite
  // CURRENT_TIMESTAMP), so we shift +8h before date() so a ritual logged at
  // Beijing 06:00 (UTC 22:00 prev day) is grouped under the Beijing calendar
  // day the user actually experienced, not the UTC day.
  const cutoff = new Date();
  cutoff.setDate(cutoff.getDate() - days);

  const rows = cockpitDb.prepare(
    "SELECT type, date(created_at, '+8 hours') as day, time(created_at, '+8 hours') as time FROM ritual_logs WHERE created_at >= ? ORDER BY created_at"
  ).all(cutoff.toISOString());

  // Group by day
  const byDay = {};
  for (const r of rows) {
    if (!byDay[r.day]) byDay[r.day] = [];
    byDay[r.day].push({ type: r.type, time: r.time });
  }

  res.json({ days: byDay });
});

app.post('/api/rituals', (req, res) => {
  const { type } = req.body || {};
  const valid = ['morning', 'lunch', 'night', 'offwork'];
  if (!valid.includes(type))
    return res.status(400).json({ error: 'invalid ritual type' });

  cockpitDb.prepare('INSERT INTO ritual_logs (type) VALUES (?)').run(type);
  const configs = {
    morning:  { state: 'idle',  color: '#ff6b35', emote: 'normal', flash: true },
    lunch:    { state: 'idle',  color: '#eab308', emote: 'tired',  flash: false },
    night:    { state: 'idle',  color: '#1e293b', emote: 'tired',  flash: false },
    offwork:  { state: 'done',  color: '#10b981', emote: 'squish', flash: false },
  };

  res.json({ ok: true, type, config: configs[type] });
});

app.get('/api/rituals/streak', (req, res) => {
  // Group by Beijing calendar day (created_at is UTC; +8h shifts to CST).
  const rows = cockpitDb.prepare(
    "SELECT DISTINCT type, date(created_at, '+8 hours') as day FROM ritual_logs ORDER BY day DESC"
  ).all();

  // Beijing "today" as YYYY-MM-DD. toISOString() is UTC, so we build the
  // Beijing date manually from getUTC* + 8h rollover instead.
  function beijingToday() {
    const d = new Date();
    const t = new Date(d.getTime() + 8 * 3600 * 1000);
    return t.toISOString().slice(0, 10);
  }

  function beijingDayOffset(offsetDays) {
    const d = new Date();
    const t = new Date(d.getTime() + 8 * 3600 * 1000 + offsetDays * 86400 * 1000);
    return t.toISOString().slice(0, 10);
  }

  function calcStreak(type) {
    let streak = 0;
    const typeDays = new Set(rows.filter(r => r.type === type).map(r => r.day));
    for (let i = 0; i < 365; i++) {
      if (typeDays.has(beijingDayOffset(-i))) streak++;
      else break;
    }
    return streak;
  }

  const today = beijingToday();
  const todayTypes = rows.filter(r => r.day === today).map(r => r.type);
  const allToday = ['morning', 'lunch', 'night', 'offwork'].every(t => todayTypes.includes(t));

  res.json({
    morning:  calcStreak('morning'),
    night:    calcStreak('night'),
    all_four: allToday ? 1 : 0,
  });
});

app.get('/api/presets', (req, res) => {
  const rows = cockpitDb.prepare('SELECT * FROM presets ORDER BY id').all();
  res.json(rows);
});

app.post('/api/presets', (req, res) => {
  const { name, state, color, bright, auto_switch } = req.body || {};
  if (!name || !state || !color)
    return res.status(400).json({ error: 'name, state, color required' });

  cockpitDb.prepare(
    `INSERT INTO presets (name, state, color, bright, auto_switch)
     VALUES (?, ?, ?, ?, ?)
     ON CONFLICT(name) DO UPDATE SET
       state = excluded.state,
       color = excluded.color,
       bright = excluded.bright,
       auto_switch = excluded.auto_switch`
  ).run(name, state, color, bright ?? 255, auto_switch ?? 1);

  res.json({ ok: true });
});

app.delete('/api/presets/:id', (req, res) => {
  cockpitDb.prepare('DELETE FROM presets WHERE id = ?').run(req.params.id);
  // Don't allow empty presets list
  const count = cockpitDb.prepare('SELECT COUNT(*) as c FROM presets').get().c;
  if (count === 0) {
    cockpitDb.prepare(
      "INSERT INTO presets (name, state, color) VALUES ('写代码', 'thinking', '#2B6EB0')"
    ).run();
  }
  res.json({ ok: true });
});

app.get('*', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

process.on('SIGINT', () => {
  if (cockpitDb) cockpitDb.close();
  process.exit(0);
});

app.listen(PORT, () => {
  console.log(`🦀 Clawd Mochi Cockpit → http://localhost:${PORT}`);
});
