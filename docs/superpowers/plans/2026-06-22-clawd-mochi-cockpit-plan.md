# Clawd Mochi Cockpit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a local web dashboard (Cockpit) that controls the real Clawd Mochi hardware and visualizes daemon records (tool calls, tokens, sessions).

**Architecture:** Node/Express backend at :3000 that (a) reads daemon's SQLite read-only for stats, (b) proxies status/control to daemon FastAPI at :7878, (c) maintains its own SQLite for ritual logs & presets. Frontend is vanilla HTML/CSS/JS with 4 tab-views, polling daemon status every 2s.

**Tech Stack:** Node 20+, Express 4, better-sqlite3, vanilla HTML/CSS/JS, daemon's SQLite at `~/.local/share/clawd-daemon/clawd.db`

**Project root:** `clawd-mochi/h5-cockpit/` (inside the existing clawd-mochi git repo)

---

## File Structure

```
clawd-mochi/h5-cockpit/
├── package.json                  # npm init + express + better-sqlite3
├── server.js                     # Express app: static serve + all API routes
├── cockpit.db                    # (auto-generated) ritual_logs, presets tables
├── public/
│   ├── index.html                # 4-tab single-page app shell
│   ├── style.css                 # Global styles + Clawd pixel art + theme
│   └── app.js                    # Frontend logic: tab nav, polling, fetch
├── README.md                     # Setup & usage
```

---

### Task 1: Project scaffolding + Express skeleton

**Files:**
- Create: `clawd-mochi/h5-cockpit/package.json`
- Create: `clawd-mochi/h5-cockpit/server.js`
- Create: `clawd-mochi/h5-cockpit/public/index.html`
- Create: `clawd-mochi/h5-cockpit/public/style.css`
- Create: `clawd-mochi/h5-cockpit/public/app.js`

- [ ] **Step 1: Create package.json**

```json
{
  "name": "clawd-mochi-cockpit",
  "version": "0.1.0",
  "private": true,
  "scripts": {
    "start": "node server.js"
  },
  "dependencies": {
    "express": "^4.19.2",
    "better-sqlite3": "^11.1.2"
  }
}
```

- [ ] **Step 2: Project directory & npm install**

```bash
mkdir -p /Users/yuu/yuu-project/clawd-mochi/h5-cockpit/public
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
npm install
```

- [ ] **Step 3: Write server.js skeleton (mounts static, 404→index, starts on :3000)**

```javascript
const express = require('express');
const path = require('path');

const app = express();
const PORT = 3000;

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

app.get('*', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

app.listen(PORT, () => {
  console.log(`🦀 Clawd Mochi Cockpit → http://localhost:${PORT}`);
});
```

- [ ] **Step 4: Write index.html skeleton (4 tab nav + 4 sections)**

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>Clawd Mochi Cockpit</title>
<link rel="stylesheet" href="/style.css" />
</head>
<body>
<div id="app">
  <header id="topbar">
    <div id="title">🦀 Clawd Mochi Cockpit</div>
    <div id="status-indicator" class="status-unknown">
      <span id="status-text">connecting…</span>
    </div>
  </header>

  <nav id="tabs">
    <button class="tab-btn active" data-tab="today">📊 今日驾驶舱</button>
    <button class="tab-btn" data-tab="timeline">⏪ 时间回放</button>
    <button class="tab-btn" data-tab="ritual">🌅 仪式</button>
    <button class="tab-btn" data-tab="year">📖 年鉴</button>
  </nav>

  <section id="tab-today" class="tab-content active">  </section>
  <section id="tab-timeline" class="tab-content">     </section>
  <section id="tab-ritual" class="tab-content">       </section>
  <section id="tab-year" class="tab-content">         </section>
</div>
<script src="/app.js"></script>
</body>
</html>
```

- [ ] **Step 5: Write style.css starter (layout, tabs, themes)**

```css
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif; background:#1a1a2e; color:#eee; min-height:100vh; }

#app { max-width:1100px; margin:0 auto; padding:16px; }

#topbar { display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; }
#title { font-size:22px; font-weight:700; letter-spacing:1px; }
#status-indicator { font-size:13px; padding:4px 12px; border-radius:20px; }
.status-unknown { background:#555; }
.status-ok      { background:#2D9D5A; }
.status-daemon  { background:#E8C547; color:#333; }
.status-down    { background:#D64545; }

#tabs { display:flex; gap:4px; margin-bottom:16px; }
.tab-btn {
  flex:1; padding:10px 0; border:none; border-radius:8px 8px 0 0;
  background:#2a2a3e; color:#999; font-size:14px; cursor:pointer;
  transition:all .15s;
}
.tab-btn.active { background:#3a3a5e; color:#fff; font-weight:600; }
.tab-btn:hover   { background:#353555; }

.tab-content { display:none; }
.tab-content.active { display:block; }
```

- [ ] **Step 6: Write app.js starter (tab switching)**

```javascript
document.addEventListener('DOMContentLoaded', () => {
  // Tab switching
  document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
      btn.classList.add('active');
      document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
    });
  });
});
```

- [ ] **Step 7: Verify skeleton works**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit && node server.js
# Ctrl+C to stop after verifying it starts without error
```

- [ ] **Step 8: Initial git commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: init Cockpit project skeleton"
```

---

### Task 2: Cockpit own database (cockpit.db) + presets seeding

**Files:**
- Modify: `clawd-mochi/h5-cockpit/server.js`

- [ ] **Step 1: Add better-sqlite3 require + cockpit DB init code in server.js (before app.listen)**

```javascript
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
}
```

- [ ] **Step 2: Verify seeding works**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
node server.js &
sleep 1
sqlite3 cockpit.db "SELECT * FROM presets;" 2>/dev/null || node -e "const D=require('better-sqlite3')('./cockpit.db'); console.log(D.prepare('SELECT * FROM presets').all());"
kill %1 2>/dev/null
```

- [ ] **Step 3: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: cockpit db init with ritual_logs and presets"
```

---

### Task 3: Daemon SQLite reader + /api/today + /api/year

**Files:**
- Modify: `clawd-mochi/h5-cockpit/server.js`

- [ ] **Step 1: Add daemon SQLite reader utility at top of server.js**

```javascript
const os = require('os');
const daemonDbPath = path.join(os.homedir(), '.local', 'share', 'clawd-daemon', 'clawd.db');

function queryDaemonDb(sql, params = []) {
  try {
    if (!require('fs').existsSync(daemonDbPath)) return null;
    const db = new Database(daemonDbPath, { readonly: true });
    const rows = db.prepare(sql).all(...params);
    db.close();
    return rows;
  } catch (e) {
    console.error('daemon DB query error:', e.message);
    return null;
  }
}
```

- [ ] **Step 2: Add /api/today route (before the catch-all)**

```javascript
app.get('/api/today', (req, res) => {
  const startOfDay = new Date(); startOfDay.setHours(0,0,0,0);
  const ts = Math.floor(startOfDay.getTime() / 1000);

  const rows = queryDaemonDb(
    "SELECT tool, COUNT(*) as cnt, SUM(success=0) as errs, COALESCE(SUM(tokens),0) as toks FROM tool_events WHERE ts >= ? GROUP BY tool ORDER BY cnt DESC",
    [ts]
  );
  if (rows === null) return res.json({ empty: true, message: '暂无数据' });

  // Count all tools, sum tokens, count sessions, count errors
  const totals = queryDaemonDb(
    "SELECT COUNT(*) as total_tools, COALESCE(SUM(tokens),0) as total_tokens, COUNT(DISTINCT session) as sessions, SUM(CASE WHEN success=0 THEN 1 ELSE 0 END) as errors FROM tool_events WHERE ts >= ?",
    [ts]
  );
  const row = totals ? totals[0] : { total_tools: 0, total_tokens: 0, sessions: 0, errors: 0 };

  res.json({
    empty: false,
    tools_called: row.total_tools || 0,
    tokens_total: row.total_tokens || 0,
    sessions:     row.sessions || 0,
    errors:       row.errors || 0,
    top_tools:    (rows || []).map(r => ({ name: r.tool, count: r.cnt })),
  });
});
```

- [ ] **Step 3: Add /api/year route**

```javascript
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

  const start = startDate.toISOString().slice(0,10);

  const rows = queryDaemonDb(
    "SELECT date, tools_called, tokens_total, sessions, errors FROM daily_stats WHERE date >= ? ORDER BY date",
    [start]
  );
  if (rows === null || rows.length === 0)
    return res.json({ empty: true, range, trend: [] });

  const summary = rows.reduce((acc, r) => {
    acc.tools_called += r.tools_called;
    acc.tokens_total += r.tokens_total;
    acc.sessions += r.sessions;
    acc.errors += r.errors;
    return acc;
  }, { tools_called: 0, tokens_total: 0, sessions: 0, errors: 0 });

  // Pick highest day
  const busiest = rows.reduce((a, b) => a.tools_called >= b.tools_called ? a : b);

  res.json({
    empty: false,
    range,
    summary,
    trend: rows.map(r => ({ date: r.date, tools: r.tools_called, tokens: r.tokens_total })),
    top: { busiest_day: busiest.date, busiest_calls: busiest.tools_called },
  });
});
```

- [ ] **Step 4: Test both endpoints**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
node server.js &
sleep 1
curl -s http://localhost:3000/api/today | head -c 300
echo ""
curl -s "http://localhost:3000/api/year?range=week" | head -c 300
echo ""
kill %1 2>/dev/null
```

- [ ] **Step 5: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: daemon DB queries for /api/today and /api/year"
```

---

### Task 4: /api/status proxy + /api/control proxy

**Files:**
- Modify: `clawd-mochi/h5-cockpit/server.js`

- [ ] **Step 1: Add status + control routes (before the catch-all)**

```javascript
const http = require('http');

const DAEMON_URL = 'http://127.0.0.1:7878';

function daemonFetch(path) {
  return new Promise(resolve => {
    http.get(DAEMON_URL + path, res => {
      let data = '';
      res.on('data', c => data += c);
      res.on('end', () => {
        try { resolve(JSON.parse(data)); }
        catch { resolve(null); }
      });
    }).on('error', () => resolve(null));
  });
}

app.get('/api/status', async (req, res) => {
  // We can't directly read daemon status from FastAPI (no /status endpoint).
  // Instead: check daemon /health, daemon DB for last tool event, and infer.
  const health = await daemonFetch('/health');

  if (!health || !health.ok) {
    return res.json({ daemon: false, esp32: false, state: 'unknown' });
  }

  // Read daemon's current status from in-memory state? Not exposed via API.
  // Best approximation: last tool_events entry + daily stats.
  const lastRows = queryDaemonDb(
    "SELECT ts, tool, success, tokens, session FROM tool_events ORDER BY ts DESC LIMIT 1"
  );

  const stats = queryDaemonDb(
    "SELECT tools_called, tokens_total FROM daily_stats WHERE date = date('now','localtime')"
  );

  res.json({
    daemon: true,
    esp32: true,  // assume online (daemon tracks this but doesn't expose it)
    last_tool: lastRows && lastRows[0] ? lastRows[0].tool : null,
    last_ts:  lastRows && lastRows[0] ? lastRows[0].ts : null,
    today_tools:  stats && stats[0] ? stats[0].tools_called : 0,
    today_tokens: stats && stats[0] ? stats[0].tokens_total : 0,
  });
});

app.post('/api/control', (req, res) => {
  // Forward control to daemon's /event/ endpoint
  // Daemon expects: pre_tool, post_tool, stop, notification, prompt
  // For manual control, we use pre_tool with a synthetic payload
  const { state, tool, task } = req.body;

  if (!state) return res.status(400).json({ error: 'state required' });

  // Map Cockpit states to daemon events
  // 'thinking' → pre_tool, 'done' → stop, 'idle' → pre_tool with idle, 'error' → post_tool with error
  const endpoint = state === 'done' ? '/event/stop' : '/event/pre_tool';
  const payload = JSON.stringify({
    tool: tool || 'manual',
    cwd: process.cwd(),
    ...(state === 'done' ? {} : { task: task || '' }),
  });

  const postReq = http.request(DAEMON_URL + endpoint, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) },
  }, response => {
    let data = '';
    response.on('data', c => data += c);
    response.on('end', () => res.json({ ok: true, daemon_response: data }));
  });
  postReq.on('error', () => res.json({ ok: false, error: 'daemon unreachable' }));
  postReq.write(payload);
  postReq.end();
});
```

- [ ] **Step 2: Test endpoints**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
node server.js &
sleep 1
curl -s http://localhost:3000/api/status | head -c 400
echo ""
curl -s -X POST http://localhost:3000/api/control -H "Content-Type: application/json" -d '{"state":"thinking","tool":"manual"}' | head -c 200
echo ""
kill %1 2>/dev/null
```

- [ ] **Step 3: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: /api/status and /api/control proxy endpoints"
```

---

### Task 5: /api/timeline endpoint (time-bucketed tool_events)

**Files:**
- Modify: `clawd-mochi/h5-cockpit/server.js`

- [ ] **Step 1: Add /api/timeline route**

```javascript
app.get('/api/timeline', (req, res) => {
  const dateStr = req.query.date || new Date().toISOString().slice(0,10);
  // Prevent future dates
  if (dateStr > new Date().toISOString().slice(0,10))
    return res.json({ empty: true, date: dateStr, buckets: [] });

  const startOfDay = new Date(dateStr + 'T00:00:00');
  const startTs = Math.floor(startOfDay.getTime() / 1000);

  const rows = queryDaemonDb(
    "SELECT ts, tool, success, tokens FROM tool_events WHERE ts >= ? AND ts < ? ORDER BY ts",
    [startTs, startTs + 86400]
  );
  if (!rows || rows.length === 0)
    return res.json({ empty: true, date: dateStr, buckets: [] });

  // Bucket into 5-min intervals (288 buckets)
  const BUCKET_SEC = 300;
  const buckets = [];
  for (let i = 0; i < 288; i++) buckets.push({ ts: startTs + i * BUCKET_SEC, tools: [], errors: 0, count: 0 });

  for (const r of rows) {
    const idx = Math.min(287, Math.floor((r.ts - startTs) / BUCKET_SEC));
    buckets[idx].tools.push(r.tool);
    if (!r.success) buckets[idx].errors++;
    buckets[idx].count++;
  }

  // Compress: only return non-empty buckets
  const nonEmpty = buckets.filter(b => b.count > 0).map(b => ({
    ts: b.ts,
    count: b.count,
    errors: b.errors,
    top_tool: b.tools.length > 0
      ? b.tools.sort((a,_) => b.tools.filter(x => x === a).length - b.tools.filter(x => x === _).length)[0]
      : null,
  }));

  res.json({ empty: false, date: dateStr, buckets: nonEmpty });
});
```

- [ ] **Step 2: Test endpoint**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
node server.js &
sleep 1
curl -s "http://localhost:3000/api/timeline?date=2026-06-22" | head -c 400
echo ""
kill %1 2>/dev/null
```

- [ ] **Step 3: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: /api/timeline endpoint with 5-min buckets"
```

---

### Task 6: Ritual & Presets CRUD endpoints

**Files:**
- Modify: `clawd-mochi/h5-cockpit/server.js`

- [ ] **Step 1: Add ritual endpoints**

```javascript
app.get('/api/rituals', (req, res) => {
  const days = parseInt(req.query.days) || 90;
  const cutoff = new Date(); cutoff.setDate(cutoff.getDate() - days);
  const rows = cockpitDb.prepare(
    "SELECT type, date(created_at) as day, time(created_at) as time FROM ritual_logs WHERE created_at >= ? ORDER BY created_at"
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
  const { type } = req.body;
  const valid = ['morning', 'lunch', 'night', 'offwork'];
  if (!valid.includes(type))
    return res.status(400).json({ error: 'invalid ritual type' });

  cockpitDb.prepare('INSERT INTO ritual_logs (type) VALUES (?)').run(type);
  res.json({ ok: true, type });
});

app.get('/api/rituals/streak', (req, res) => {
  const rows = cockpitDb.prepare(
    "SELECT type, date(created_at) as day FROM ritual_logs GROUP BY type, day ORDER BY day DESC"
  ).all();

  function calcStreak(type) {
    let streak = 0;
    const today = new Date().toISOString().slice(0,10);
    const typeDays = rows.filter(r => r.type === type).map(r => r.day);
    for (let i = 0; i < 365; i++) {
      const d = new Date(); d.setDate(d.getDate() - i);
      const ds = d.toISOString().slice(0,10);
      if (typeDays.includes(ds)) streak++;
      else break;
    }
    return streak;
  }

  // combined streak: all 4 types today
  const today = new Date().toISOString().slice(0,10);
  const todayTypes = rows.filter(r => r.day === today).map(r => r.type);
  const allToday = ['morning','lunch','night','offwork'].every(t => todayTypes.includes(t));

  res.json({
    morning:  calcStreak('morning'),
    night:    calcStreak('night'),
    all_four: allToday ? 1 : 0,
    combined_streak: calcStreak('morning') + calcStreak('night'),
  });
});
```

- [ ] **Step 2: Add presets endpoints**

```javascript
app.get('/api/presets', (req, res) => {
  const rows = cockpitDb.prepare('SELECT * FROM presets ORDER BY id').all();
  res.json(rows);
});

app.post('/api/presets', (req, res) => {
  const { name, state, color, bright, auto_switch } = req.body;
  if (!name || !state || !color)
    return res.status(400).json({ error: 'name, state, color required' });

  cockpitDb.prepare(
    'INSERT INTO presets (name, state, color, bright, auto_switch) VALUES (?, ?, ?, ?, ?) ON CONFLICT(name) DO UPDATE SET state=excluded.state, color=excluded.color, bright=excluded.bright, auto_switch=excluded.auto_switch'
  ).run(name, state, color, bright ?? 255, auto_switch ?? 1);

  res.json({ ok: true });
});

app.delete('/api/presets/:id', (req, res) => {
  cockpitDb.prepare('DELETE FROM presets WHERE id = ?').run(req.params.id);
  // Don't allow deleting last preset
  const count = cockpitDb.prepare('SELECT COUNT(*) as c FROM presets').get().c;
  if (count === 0) {
    cockpitDb.prepare('INSERT INTO presets (name, state, color) VALUES (\'写代码\', \'thinking\', \'#2B6EB0\')').run();
  }
  res.json({ ok: true });
});
```

- [ ] **Step 3: Test ritual endpoints**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
node server.js &
sleep 1
curl -s -X POST http://localhost:3000/api/rituals -H "Content-Type: application/json" -d '{"type":"morning"}'
echo ""
curl -s http://localhost:3000/api/rituals | head -c 300
echo ""
curl -s http://localhost:3000/api/presets
echo ""
kill %1 2>/dev/null
```

- [ ] **Step 4: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: rituals and presets CRUD endpoints"
```

---

### Task 7: Clawd CSS pixel art + full visual theme

**Files:**
- Rewrite: `clawd-mochi/h5-cockpit/public/style.css`

- [ ] **Step 1: Replace style.css with full theme (Clawd pixel icon + layout + state colors)**

```css
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif; background:#1a1a2e; color:#eee; min-height:100vh; }

/* ── Layout ───────────────────────────── */
#app { max-width:1100px; margin:0 auto; padding:16px; }

/* ── Top bar ──────────────────────────── */
#topbar { display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; flex-wrap:wrap; gap:8px; }
#title { font-size:22px; font-weight:700; letter-spacing:1px; display:flex; align-items:center; gap:8px; }
#status-indicator { font-size:12px; padding:4px 14px; border-radius:20px; white-space:nowrap; }
.status-unknown { background:#555; color:#ccc; }
.status-ok      { background:#2D9D5A; color:#fff; }
.status-daemon  { background:#E8C547; color:#333; }
.status-down    { background:#D64545; color:#fff; }

/* ── Tab nav ──────────────────────────── */
#tabs { display:flex; gap:4px; margin-bottom:16px; }
.tab-btn {
  flex:1; padding:10px 0; border:none; border-radius:8px 8px 0 0;
  background:#2a2a3e; color:#999; font-size:14px; cursor:pointer;
  transition:all .15s; font-family:inherit;
}
.tab-btn.active { background:#3a3a5e; color:#fff; font-weight:600; }
.tab-btn:hover   { background:#353555; }

.tab-content { display:none; background:#16213e; border-radius:0 8px 8px 8px; padding:20px; min-height:400px; }
.tab-content.active { display:block; }

/* ── Error banner ─────────────────────── */
.error-banner { background:#D64545; color:#fff; padding:10px 16px; border-radius:8px; margin-bottom:12px; font-size:13px; display:none; }
.error-banner.show { display:block; }

/* ── Layout: today dashboard ──────────── */
.two-col { display:grid; grid-template-columns:1fr 1fr; gap:16px; }

.panel { background:#1a1a30; border-radius:10px; padding:16px; }
.panel h3 { font-size:15px; margin-bottom:12px; color:#aaa; letter-spacing:1px; }

/* ── Clawd pixel icon ─────────────────── */
.clawd-box { display:flex; flex-direction:column; align-items:center; margin-bottom:16px; }
.clawd-screen {
  width:140px; height:140px; border-radius:12px;
  display:flex; align-items:center; justify-content:center;
  transition:background .4s;
  background:#FF8C42;
}
.clawd-screen .clawd-face { position:relative; width:90px; height:60px; }
.eye {
  position:absolute; top:0;
  width:24px; height:30px; border-radius:4px;
  transition:all .3s;
}
.eye-l { left:10px; }
.eye-r { right:10px; }
/* Normal */
.eye-normal { background:#1a1a2e; }
/* Squish */
.eye-squish { background:transparent; width:0; height:0; border-left:14px solid transparent; border-right:14px solid transparent; border-bottom:20px solid #1a1a2e; border-radius:0; top:10px; }
/* Error (X) */
.eye-x { background:transparent; }
.eye-x::before, .eye-x::after {
  content:''; position:absolute; top:0; left:10px;
  width:4px; height:30px; background:#1a1a2e; border-radius:2px;
}
.eye-x::before { transform:rotate(45deg); }
.eye-x::after  { transform:rotate(-45deg); }
/* Tired (half closed) */
.eye-tired { height:12px; top:18px; background:#1a1a2e; }

.mouth { position:absolute; bottom:0; left:50%; transform:translateX(-50%); width:18px; height:3px; background:#1a1a2e; border-radius:2px; transition:all .3s; }
.mouth-smile { width:28px; height:6px; border-radius:0 0 14px 14px; transform:translateX(-50%) rotate(0); }

/* ── Control buttons ──────────────────── */
.btn-group { display:flex; flex-wrap:wrap; gap:6px; margin-bottom:12px; }
.btn-group .btn {
  padding:8px 14px; border:none; border-radius:8px; font-size:12px;
  font-family:inherit; cursor:pointer; transition:all .1s; color:#fff;
}
.btn-group .btn:active { transform:scale(.95); opacity:.8; }
.btn-group .btn:disabled { opacity:.4; cursor:not-allowed; }

.emo-btn-norm { background:#FF8C42; }
.emo-btn-sqsh { background:#E8C547; color:#333; }
.emo-btn-x    { background:#D64545; }
.emo-btn-tire { background:#6B6B6B; }

.color-btn { width:36px; height:36px; border-radius:50%; border:2px solid transparent; }
.color-btn.active { border-color:#fff; }
.color-orange { background:#FF8C42; }
.color-blue   { background:#2B6EB0; }
.color-yellow { background:#E8C547; }
.color-green  { background:#2D9D5A; }
.color-red    { background:#D64545; }

.preset-btn { background:#2a2a4e; border:1px solid #444; padding:8px 14px; border-radius:8px; font-size:12px; color:#ddd; font-family:inherit; cursor:pointer; }
.preset-btn:active { transform:scale(.95); }

/* ── Stat cards ───────────────────────── */
.stat-grid { display:grid; grid-template-columns:1fr 1fr; gap:8px; margin-bottom:16px; }
.stat-card { background:#2a2a4a; border-radius:8px; padding:12px; text-align:center; }
.stat-card .num { font-size:28px; font-weight:700; }
.stat-card .lbl { font-size:11px; color:#999; margin-top:2px; }
.stat-card .bar { height:8px; background:#3a3a5a; border-radius:4px; margin-top:6px; overflow:hidden; }
.stat-card .bar-fill { height:100%; border-radius:4px; transition:width .4s; }

/* ── Tool rank list ───────────────────── */
.tool-rank { list-style:none; }
.tool-rank li { display:flex; align-items:center; gap:8px; margin-bottom:6px; font-size:13px; }
.tool-rank .bar { flex:1; height:14px; background:#2a2a4a; border-radius:4px; overflow:hidden; }
.tool-rank .bar-fill { height:100%; background:#2B6EB0; border-radius:4px; min-width:4px; }
.tool-rank .cnt { color:#999; min-width:30px; text-align:right; }

/* ── Timeline ─────────────────────────── */
.date-picker { display:flex; align-items:center; gap:10px; margin-bottom:14px; }
.date-picker input, .date-picker button { padding:6px 12px; border-radius:6px; border:none; font-family:inherit; font-size:13px; }
.date-picker input { background:#2a2a4a; color:#eee; }
.date-picker button { background:#3a3a5e; color:#ccc; cursor:pointer; }
.date-picker button:active { transform:scale(.95); }

.timeline-canvas { width:100%; height:120px; background:#1a1a30; border-radius:8px; display:block; }

.info-card { background:#2a2a4a; border-radius:8px; padding:12px; margin-top:12px; }
.info-card .row { display:flex; gap:8px; margin-bottom:4px; font-size:13px; }
.info-card .label { color:#999; min-width:70px; }
.info-card .value { color:#eee; }

/* ── Rituals ──────────────────────────── */
.ritual-btns { display:flex; gap:10px; margin-bottom:20px; flex-wrap:wrap; }
.ritual-btn {
  flex:1; min-width:120px; padding:16px 8px; border:none; border-radius:12px;
  font-size:16px; cursor:pointer; text-align:center; transition:all .15s;
  background:#2a2a4a; color:#ddd; font-family:inherit;
}
.ritual-btn:hover { background:#3a3a5e; }
.ritual-btn:active { transform:scale(.95); }
.ritual-btn.done { opacity:.6; }
.ritual-btn .time { display:block; font-size:11px; color:#999; margin-top:4px; }

.heatmap { display:flex; flex-wrap:wrap; gap:2px; }
.heatmap .cell { width:12px; height:12px; border-radius:2px; }
.heatmap .cell-0 { background:#2a2a3a; }
.heatmap .cell-1 { background:#1b5e20; }
.heatmap .cell-2 { background:#2e7d32; }
.heatmap .cell-3 { background:#388e3c; }
.heatmap .cell-4 { background:#43a047; }

/* ── Yearbook ─────────────────────────── */
.year-subtabs { display:flex; gap:4px; margin-bottom:14px; }
.year-subtab {
  padding:6px 18px; border:none; border-radius:6px 6px 0 0;
  background:#2a2a3e; color:#999; font-size:13px; cursor:pointer; font-family:inherit;
}
.year-subtab.active { background:#3a3a5e; color:#fff; }

.card-row { display:grid; grid-template-columns:repeat(3,1fr); gap:10px; margin-bottom:16px; }
.trend-chart { display:flex; align-items:flex-end; gap:4px; height:120px; padding:8px 0; }
.trend-chart .bar { flex:1; border-radius:3px 3px 0 0; min-height:4px; background:#2B6EB0; }
.trend-chart .bar-label { text-align:center; font-size:9px; color:#888; }

.clawd-comment { background:#2a2a4a; border-radius:8px; padding:14px; font-size:14px; line-height:1.6; border-left:3px solid #FF8C42; }
```

- [ ] **Step 2: Verify no syntax errors**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
node -e "require('fs').readFileSync('./public/style.css','utf8')"
```

- [ ] **Step 3: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: full CSS theme with Clawd pixel icon and layouts"
```

---

### Task 8: index.html with all 4 tab layouts

**Files:**
- Rewrite: `clawd-mochi/h5-cockpit/public/index.html`

- [ ] **Step 1: Write complete index.html**

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>Clawd Mochi Cockpit</title>
<link rel="stylesheet" href="/style.css" />
</head>
<body>
<div id="app">
  <header id="topbar">
    <div id="title">🦀 Clawd Mochi Cockpit</div>
    <div id="status-indicator" class="status-unknown"><span id="status-text">connecting…</span></div>
  </header>

  <div id="error-banner" class="error-banner">⚠️ <span id="error-text"></span></div>

  <nav id="tabs">
    <button class="tab-btn active" data-tab="today">📊 今日驾驶舱</button>
    <button class="tab-btn" data-tab="timeline">⏪ 时间回放</button>
    <button class="tab-btn" data-tab="ritual">🌅 仪式</button>
    <button class="tab-btn" data-tab="year">📖 年鉴</button>
  </nav>

  <!-- ────── TAB: TODAY ────── -->
  <section id="tab-today" class="tab-content active">
    <div class="two-col">
      <!-- LEFT: Remote Control -->
      <div class="panel">
        <h3>🎮 遥控台</h3>
        <div class="clawd-box">
          <div class="clawd-screen" id="clawd-screen">
            <div class="clawd-face" id="clawd-face">
              <div class="eye eye-l eye-normal" id="eye-l"></div>
              <div class="eye eye-r eye-normal" id="eye-r"></div>
              <div class="mouth" id="mouth"></div>
            </div>
          </div>
          <div style="font-size:12px;color:#999;margin-top:6px">
            <span id="clawd-state-label">空闲</span> · <span id="clawd-project"></span>
          </div>
        </div>

        <div style="margin-bottom:4px;font-size:12px;color:#aaa;">😶 表情</div>
        <div class="btn-group" id="emote-btns">
          <button class="btn emo-btn-norm" data-emote="normal">😺 正常</button>
          <button class="btn emo-btn-sqsh" data-emote="squish">😆 笑眼</button>
          <button class="btn emo-btn-x"    data-emote="error">😵 错愕</button>
          <button class="btn emo-btn-tire" data-emote="tired">😴 困倦</button>
        </div>

        <div style="margin:8px 0 4px;font-size:12px;color:#aaa;">🎨 心情灯</div>
        <div class="btn-group" id="color-btns">
          <button class="btn color-btn color-orange active" data-color="#FF8C42"></button>
          <button class="btn color-btn color-blue"   data-color="#2B6EB0"></button>
          <button class="btn color-btn color-yellow" data-color="#E8C547"></button>
          <button class="btn color-btn color-green"  data-color="#2D9D5A"></button>
          <button class="btn color-btn color-red"    data-color="#D64545"></button>
        </div>

        <div style="margin:8px 0 4px;font-size:12px;color:#aaa;">🌅 情景模式</div>
        <div class="btn-group" id="preset-btns"></div>
      </div>

      <!-- RIGHT: Today's Data -->
      <div class="panel">
        <h3>📊 今日数据</h3>
        <div class="stat-grid" id="today-stats">
          <div class="stat-card"><div class="num" id="stat-tools">--</div><div class="lbl">工具调用</div></div>
          <div class="stat-card">
            <div class="num" id="stat-tokens">--</div><div class="lbl">Token</div>
            <div class="bar"><div class="bar-fill" id="token-bar" style="width:0;background:#2B6EB0"></div></div>
          </div>
          <div class="stat-card"><div class="num" id="stat-sessions">--</div><div class="lbl">会话数</div></div>
          <div class="stat-card"><div class="num" id="stat-errors">--</div><div class="lbl">错误数</div></div>
        </div>
        <h4 style="font-size:13px;color:#aaa;margin-bottom:8px;">🔥 工具排行</h4>
        <ul class="tool-rank" id="tool-rank"><li style="color:#666;font-size:13px;">暂无数据</li></ul>
      </div>
    </div>
  </section>

  <!-- ────── TAB: TIMELINE ────── -->
  <section id="tab-timeline" class="tab-content">
    <div class="date-picker">
      <button id="tl-prev">‹</button>
      <input type="date" id="tl-date" />
      <button id="tl-next">›</button>
      <button id="tl-today">今天</button>
    </div>
    <canvas id="tl-canvas" class="timeline-canvas" height="120"></canvas>
    <div class="info-card" id="tl-info">
      <div class="row"><span class="label">时间</span><span class="value" id="tl-ts">--</span></div>
      <div class="row"><span class="label">工具</span><span class="value" id="tl-tool">--</span></div>
      <div class="row"><span class="label">调用</span><span class="value" id="tl-count">--</span></div>
      <div class="row"><span class="label">错误</span><span class="value" id="tl-err">--</span></div>
    </div>
  </section>

  <!-- ────── TAB: RITUAL ────── -->
  <section id="tab-ritual" class="tab-content">
    <div class="ritual-btns" id="ritual-btns">
      <button class="ritual-btn" data-ritual="morning">☀️ 早安<span class="time" id="rit-morning"></span></button>
      <button class="ritual-btn" data-ritual="lunch">🍱 午休<span class="time" id="rit-lunch"></span></button>
      <button class="ritual-btn" data-ritual="night">🌙 晚安<span class="time" id="rit-night"></span></button>
      <button class="ritual-btn" data-ritual="offwork">🚀 下班<span class="time" id="rit-offwork"></span></button>
    </div>
    <div style="font-size:12px;color:#999;margin-bottom:12px;">
      🔥 早安连续: <span id="streak-morning">0</span> &nbsp; 晚安连续: <span id="streak-night">0</span>
    </div>
    <div id="heatmap" class="heatmap"></div>
  </section>

  <!-- ────── TAB: YEAR ────── -->
  <section id="tab-year" class="tab-content">
    <div class="year-subtabs">
      <button class="year-subtab active" data-range="week">📅 周报</button>
      <button class="year-subtab" data-range="month">📅 月报</button>
      <button class="year-subtab" data-range="year">📅 年报</button>
    </div>
    <div class="card-row" id="year-cards">
      <div class="stat-card"><div class="num" id="yr-tools">--</div><div class="lbl">工具调用</div></div>
      <div class="stat-card"><div class="num" id="yr-tokens">--</div><div class="lbl">Token</div></div>
      <div class="stat-card"><div class="num" id="yr-sessions">--</div><div class="lbl">会话</div></div>
    </div>
    <div class="trend-chart" id="yr-chart"></div>
    <div style="margin:12px 0 8px;font-size:12px;color:#aaa;">🏆 本期之最</div>
    <div id="yr-top" style="font-size:13px;color:#ccc;margin-bottom:14px;">暂无</div>
    <div class="clawd-comment" id="yr-comment">Clawd 看着你。</div>
  </section>
</div>
<script src="/app.js"></script>
</body>
</html>
```

- [ ] **Step 2: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: complete index.html with all 4 tab layouts"
```

---

### Task 9: Frontend JavaScript — full app logic

**Files:**
- Rewrite: `clawd-mochi/h5-cockpit/public/app.js`

- [ ] **Step 1: Write complete app.js**

```javascript
document.addEventListener('DOMContentLoaded', () => {
  // ── Tab Switching ─────────────────────────
  document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
      btn.classList.add('active');
      document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
      // Refresh tab data on switch
      if (btn.dataset.tab === 'today') loadToday();
      if (btn.dataset.tab === 'year')   loadYear('week');
      if (btn.dataset.tab === 'ritual') loadRituals();
      if (btn.dataset.tab === 'timeline') loadTimeline();
    });
  });

  // ── Status Poll (every 2s) ──────────────
  const statusEl = document.getElementById('status-indicator');
  const statusText = document.getElementById('status-text');
  const errorBanner = document.getElementById('error-banner');

  let lastStatus = null;
  let currentColor = '#FF8C42';

  async function pollStatus() {
    try {
      const res = await fetch('/api/status');
      const s = await res.json();
      lastStatus = s;
      if (s.daemon) {
        statusEl.className = 'status-ok';
        statusText.textContent = 'daemon ✅';
      } else {
        statusEl.className = 'status-down';
        statusText.textContent = 'daemon ❌';
        document.querySelectorAll('.btn-group .btn').forEach(b => b.disabled = true);
        errorBanner.classList.add('show');
        document.getElementById('error-text').textContent = 'daemon 未运行';
        return;
      }
      errorBanner.classList.remove('show');
      document.querySelectorAll('.btn-group .btn').forEach(b => b.disabled = false);
      // Update Clawd display based on status
      if (s.last_tool) {
        document.getElementById('clawd-state-label').textContent = '思考中';
        document.getElementById('clawd-screen').style.background = '#2B6EB0';
      } else {
        document.getElementById('clawd-state-label').textContent = '空闲';
        document.getElementById('clawd-screen').style.background = currentColor;
      }
    } catch {
      statusEl.className = 'status-down';
      statusText.textContent = 'daemon ❌';
    }
  }

  // ── Load Today ────────────────────────────
  async function loadToday() {
    try {
      const res = await fetch('/api/today');
      const d = await res.json();
      if (d.empty) {
        document.getElementById('stat-tools').textContent = '0';
        document.getElementById('stat-tokens').textContent = '0';
        document.getElementById('stat-sessions').textContent = '0';
        document.getElementById('stat-errors').textContent = '0';
        document.getElementById('tool-rank').innerHTML = '<li style="color:#666;font-size:13px;">暂无数据</li>';
        return;
      }
      document.getElementById('stat-tools').textContent = d.tools_called;
      document.getElementById('stat-tokens').textContent = (d.tokens_total >= 1000)
        ? Math.round(d.tokens_total / 1000) + 'k' : d.tokens_total;
      document.getElementById('stat-sessions').textContent = d.sessions;
      document.getElementById('stat-errors').textContent = d.errors;
      const pct = d.tokens_total > 0 ? Math.min(100, Math.round(d.tokens_total / 2000)) : 0;
      document.getElementById('token-bar').style.width = pct + '%';

      const max = d.top_tools.length > 0 ? d.top_tools[0].count : 1;
      document.getElementById('tool-rank').innerHTML = d.top_tools.slice(0,8).map(t =>
        `<li><span style="min-width:40px">${t.name}</span><div class="bar"><div class="bar-fill" style="width:${Math.round(t.count/max*100)}%"></div></div><span class="cnt">${t.count}</span></li>`
      ).join('');
    } catch { /* silent */ }
  }

  // ── Emote Buttons ─────────────────────────
  document.querySelectorAll('#emote-btns .btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const emote = btn.dataset.emote;
      const eyeL = document.getElementById('eye-l');
      const eyeR = document.getElementById('eye-r');
      const mouth = document.getElementById('mouth');
      // Reset classes
      eyeL.className = 'eye eye-l';
      eyeR.className = 'eye eye-r';
      mouth.className = 'mouth';

      switch (emote) {
        case 'normal': eyeL.classList.add('eye-normal'); eyeR.classList.add('eye-normal'); break;
        case 'squish': eyeL.classList.add('eye-squish'); eyeR.classList.add('eye-squish'); mouth.classList.add('mouth-smile'); break;
        case 'error':  eyeL.classList.add('eye-x');      eyeR.classList.add('eye-x'); break;
        case 'tired':  eyeL.classList.add('eye-tired');  eyeR.classList.add('eye-tired'); break;
      }

      // Push to daemon
      const state = emote === 'error' ? 'error' : 'idle';
      fetch('/api/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state, tool: 'emote:' + emote })
      });
    });
  });

  // ── Color Buttons ─────────────────────────
  document.querySelectorAll('#color-btns .btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('#color-btns .btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      currentColor = btn.dataset.color;
      document.getElementById('clawd-screen').style.background = currentColor;
    });
  });

  // ── Presets ─────────────────────────────
  async function loadPresets() {
    try {
      const res = await fetch('/api/presets');
      const presets = await res.json();
      document.getElementById('preset-btns').innerHTML = presets.map(p =>
        `<button class="preset-btn" data-id="${p.id}" data-state="${p.state}" data-color="${p.color}">${p.name}</button>`
      ).join('');
      document.querySelectorAll('#preset-btns .preset-btn').forEach(btn => {
        btn.addEventListener('click', () => {
          currentColor = btn.dataset.color;
          document.getElementById('clawd-screen').style.background = currentColor;
          // Update emote too based on preset
          const state = btn.dataset.state;
          document.querySelectorAll(`#emote-btns .btn[data-emote="${state === 'error' ? 'error' : 'normal'}"]`).forEach(b => b.click());
          fetch('/api/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ state, tool: 'preset:' + btn.textContent.trim() })
          });
        });
      });
    } catch { /* silent */ }
  }

  // ── Rituals ─────────────────────────────
  async function loadRituals() {
    try {
      const res = await fetch('/api/rituals');
      const d = await res.json();

      // Check which rituals were done today
      const today = new Date().toISOString().slice(0,10);
      const todays = d.days[today] || [];

      ['morning','lunch','night','offwork'].forEach(type => {
        const done = todays.find(r => r.type === type);
        const el = document.getElementById('rit-' + type);
        el.textContent = done ? ' ✓ ' + done.time : '';
        const btn = document.querySelector(`.ritual-btn[data-ritual="${type}"]`);
        btn.classList.toggle('done', !!done);
      });

      // Streak
      const sRes = await fetch('/api/rituals/streak');
      const s = await sRes.json();
      document.getElementById('streak-morning').textContent = s.morning;
      document.getElementById('streak-night').textContent = s.night;

      // Heatmap
      const heatmap = document.getElementById('heatmap');
      const days = Object.keys(d.days);
      const last90 = [];
      for (let i = 89; i >= 0; i--) {
        const d = new Date(); d.setDate(d.getDate() - i);
        const ds = d.toISOString().slice(0,10);
        last90.push(ds);
      }
      heatmap.innerHTML = last90.map(ds => {
        const dayRituals = d.days[ds] || [];
        const cnt = dayRituals.length;
        return `<div class="cell cell-${cnt}" title="${ds}: ${cnt}个仪式${dayRituals.map(r => r.type).join(', ')}"></div>`;
      }).join('');
    } catch { /* silent */ }
  }

  document.querySelectorAll('.ritual-btn').forEach(btn => {
    btn.addEventListener('click', async () => {
      const type = btn.dataset.ritual;
      await fetch('/api/rituals', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type })
      });
      loadRituals();
      // Push to daemon based on ritual
      const stateMap = { morning: 'idle', lunch: 'idle', night: 'idle', offwork: 'done' };
      fetch('/api/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state: stateMap[type] || 'idle', tool: 'ritual:' + type })
      });
      // Visual feedback: flash screen
      const screen = document.getElementById('clawd-screen');
      screen.style.transition = 'background .1s';
      screen.style.background = '#E8C547';
      setTimeout(() => { screen.style.background = currentColor; }, 300);
    });
  });

  // ── Timeline ─────────────────────────────
  const tlCanvas = document.getElementById('tl-canvas');
  const ctx = tlCanvas.getContext('2d');
  let tlData = [];
  let tlDate = new Date().toISOString().slice(0,10);

  function resizeCanvas() {
    tlCanvas.width = tlCanvas.clientWidth;
    tlCanvas.height = tlCanvas.clientHeight;
  }

  async function loadTimeline() {
    try {
      const res = await fetch('/api/timeline?date=' + tlDate);
      tlData = await res.json();
      resizeCanvas();
      drawTimeline(null);
    } catch { /* silent */ }
  }

  function drawTimeline(hoverTs) {
    const w = tlCanvas.width, h = tlCanvas.height;
    ctx.clearRect(0, 0, w, h);

    if (tlData.empty || !tlData.buckets) {
      ctx.fillStyle = '#555';
      ctx.font = '14px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('暂无数据', w/2, h/2);
      return;
    }

    const buckets = tlData.buckets;
    const max = Math.max(...buckets.map(b => b.count), 1);
    const barW = Math.max(4, Math.min(12, (w - 40) / buckets.length));

    buckets.forEach((b, i) => {
      const x = 20 + i * (barW + 2);
      const barH = (b.count / max) * (h - 30);
      // Color: error → red, else blue
      const color = b.errors > 0 ? '#D64545' : '#2B6EB0';
      ctx.fillStyle = color;
      ctx.fillRect(x, h - 15 - barH, barW, barH);

      // Hover highlight
      if (hoverTs === b.ts) {
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.strokeRect(x, h - 15 - barH, barW, barH);
      }
    });
  }

  // Hover on timeline
  tlCanvas.addEventListener('mousemove', e => {
    if (tlData.empty || !tlData.buckets) return;
    const rect = tlCanvas.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const w = tlCanvas.width;
    const buckets = tlData.buckets;
    const barW = Math.max(4, Math.min(12, (w - 40) / buckets.length));

    for (let i = 0; i < buckets.length; i++) {
      const x = 20 + i * (barW + 2);
      if (mx >= x && mx <= x + barW + 2) {
        drawTimeline(buckets[i].ts);
        // Update info card
        const b = buckets[i];
        const t = new Date(b.ts * 1000);
        document.getElementById('tl-ts').textContent = t.toLocaleTimeString();
        document.getElementById('tl-tool').textContent = b.top_tool || '--';
        document.getElementById('tl-count').textContent = b.count;
        document.getElementById('tl-err').textContent = b.errors;
        return;
      }
    }
  });

  tlCanvas.addEventListener('mouseleave', () => drawTimeline(null));

  // Date picker for timeline
  const tlDateInput = document.getElementById('tl-date');
  tlDateInput.value = tlDate;
  tlDateInput.addEventListener('change', () => { tlDate = tlDateInput.value; loadTimeline(); });
  document.getElementById('tl-prev').addEventListener('click', () => {
    const d = new Date(tlDate); d.setDate(d.getDate() - 1);
    tlDate = d.toISOString().slice(0,10);
    tlDateInput.value = tlDate;
    loadTimeline();
  });
  document.getElementById('tl-next').addEventListener('click', () => {
    const d = new Date(tlDate); d.setDate(d.getDate() + 1);
    if (d.toISOString().slice(0,10) > new Date().toISOString().slice(0,10)) return;
    tlDate = d.toISOString().slice(0,10);
    tlDateInput.value = tlDate;
    loadTimeline();
  });
  document.getElementById('tl-today').addEventListener('click', () => {
    tlDate = new Date().toISOString().slice(0,10);
    tlDateInput.value = tlDate;
    loadTimeline();
  });

  // ── Yearbook ─────────────────────────────
  let currentRange = 'week';

  async function loadYear(range) {
    currentRange = range || 'week';
    try {
      const res = await fetch('/api/year?range=' + currentRange);
      const d = await res.json();
      if (d.empty) {
        document.getElementById('yr-tools').textContent = '--';
        document.getElementById('yr-tokens').textContent = '--';
        document.getElementById('yr-sessions').textContent = '--';
        document.getElementById('yr-chart').innerHTML = '<span style="color:#666;font-size:13px;">暂无</span>';
        document.getElementById('yr-top').textContent = '暂无';
        document.getElementById('yr-comment').textContent = 'Clawd 看着你。';
        return;
      }

      document.getElementById('yr-tools').textContent = d.summary.tools_called;
      const tk = d.summary.tokens_total >= 1000 ? Math.round(d.summary.tokens_total / 1000) + 'k' : d.summary.tokens_total;
      document.getElementById('yr-tokens').textContent = tk;
      document.getElementById('yr-sessions').textContent = d.summary.sessions;

      // Chart
      const trend = d.trend;
      const maxVal = Math.max(...trend.map(t => t.tools), 1);
      document.getElementById('yr-chart').innerHTML = trend.map(t =>
        `<div style="flex:1;display:flex;flex-direction:column;align-items:center;">
          <div class="bar" style="height:${Math.round(t.tools / maxVal * 100)}%;background:#2B6EB0;"></div>
          <div class="bar-label">${t.date.slice(5)}</div>
         </div>`
      ).join('');

      // Top
      if (d.top) {
        document.getElementById('yr-top').innerHTML = `最忙的一天: ${d.top.busiest_day} · ${d.top.busiest_calls} 次调用`;
      }

      // Comment (rule template)
      const calls = d.summary.tools_called;
      const errs = d.summary.errors || 0;
      let comment;
      if (calls > 500) comment = `这${currentRange === 'week' ? '周' : currentRange === 'month' ? '月' : '年'}累了。${calls} 次调用，Clawd 心疼。`;
      else if (calls < 10) comment = `清闲。Clawd 也在摸鱼。`;
      else if (errs / calls > 0.05) comment = `错误率偏高。今天有点难。`;
      else if (d.summary.tokens_total > 150000) comment = `Token 烧得猛。该重启会话了。`;
      else comment = `稳稳的${currentRange === 'week' ? '一周' : currentRange === 'month' ? '一月' : '一年'}。Clawd 满意。`;
      document.getElementById('yr-comment').textContent = '🦀 ' + comment;
    } catch { /* silent */ }
  }

  // Year sub-tabs
  document.querySelectorAll('.year-subtab').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.year-subtab').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      loadYear(btn.dataset.range);
    });
  });

  // ── Init ─────────────────────────────────
  pollStatus();
  loadToday();
  loadPresets();
  loadRituals();
  loadTimeline();
  loadYear('week');
  setInterval(pollStatus, 2000);
  setInterval(loadToday, 10000);
  setInterval(loadRituals, 30000);
  window.addEventListener('resize', () => {
    if (document.getElementById('tab-timeline').classList.contains('active')) resizeCanvas();
  });
});
```

- [ ] **Step 2: Start server and verify**

```bash
cd /Users/yuu/yuu-project/clawd-mochi/h5-cockpit
node server.js &
sleep 1
curl -s http://localhost:3000/ | head -c 500
echo "... OK"
kill %1 2>/dev/null
```

- [ ] **Step 3: Full acceptance walkthrough by opening http://localhost:3000**

Open `http://localhost:3000` and check:

1. ✅ 4 tabs visible, switching works
2. ✅ Status indicator shows "daemon ✅" or "daemon ❌"
3. ✅ Today tab shows stat cards and tool ranking
4. ✅ Click emote buttons → virtual Clawd changes expression
5. ✅ Click color buttons → screen color changes
6. ✅ Preset buttons appear
7. ✅ Ritual tab: click a ritual → time label appears
8. ✅ Timeline tab: canvas draws, hover shows info card
9. ✅ Yearbook tab: 3 sub-tabs each show data + chart

- [ ] **Step 4: Commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "feat: complete frontend with all 4 tab views and polling"
```

---

### Task 10: README + final README

**Files:**
- Create: `clawd-mochi/h5-cockpit/README.md`

- [ ] **Step 1: Write README.md**

```markdown
# Clawd Mochi Cockpit 🦀

本地 Web 控制台 + 数据看板，作为 [Clawd Mochi](https://github.com/) 实体玩偶的 GUI 伴侣。

## 架构

```
浏览器 → Cockpit (:3000) → 只读 daemon SQLite (~/.local/share/clawd-daemon/clawd.db)
                         → HTTP → daemon (:7878) → ESP32
                         → 读写 cockpit.db (rituals, presets)
```

## 启动

```bash
cd clawd-mochi/h5-cockpit
npm install
npm start
# 打开 http://localhost:3000
```

## 功能

| Tab | 功能 |
|-----|------|
| 📊 今日驾驶舱 | 远程控制 (表情/颜色/预设) + 今日工具/token 统计 |
| ⏪ 时间回放 | 按日查看 5 分钟间隔的状态热力图 |
| 🌅 仪式 | 早安/午休/晚安/下班打卡, 90 天热力图, 连续天数 |
| 📖 年鉴 | 周报/月报/年度回顾, 趋势图, Clawd 评语 |

## 依赖

- daemon (`clawd-daemon`) 已配置并运行
- Node.js 20+
```

- [ ] **Step 2: Final commit**

```bash
cd /Users/yuu/yuu-project/clawd-mochi
git add h5-cockpit/
git commit -m "docs: README for Cockpit"
```

---

## Spec Coverage Check

| Spec section | Implemented in |
|---|---|
| Tab ① 今日驾驶舱 (left: remote control) | Task 9 HTML + JS (emote/color/preset buttons) |
| Tab ① 今日驾驶舱 (right: data) | Task 3 (/api/today) + Task 9 (right stat panel) |
| Tab ② 时间回放 | Task 5 (/api/timeline) + Task 9 (canvas + hover) |
| Tab ③ 仪式 (buttons + streak) | Task 6 (/api/rituals) + Task 9 (ritual buttons + daemon push) |
| Tab ③ 仪式 (heatmap) | Task 9 (heatmap rendering from /api/rituals) |
| Tab ④ 年鉴 (3 ranges + trend + comment) | Task 3 (/api/year) + Task 9 (year frontend + comment rules) |
| daemon status polling (2s) | Task 9 (setInterval pollStatus, 2s) |
| daemon control proxy | Task 4 (/api/control → daemon /event/) |
| presets CRUD | Task 6 (server routes) + Task 9 (frontend load) |
| error states (daemon down → buttons disabled) | Task 9 (status-down → btn.disabled = true) |
| Clawd pixel CSS icon | Task 7 (CSS layouts: .eye, .mouth, .clawd-screen) |
| Color theme | Task 7 (.color-orange/blue/yellow/green/red) |