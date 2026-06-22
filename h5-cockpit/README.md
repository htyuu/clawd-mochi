# Clawd Mochi Cockpit 🦀

本地 Web 控制台 + 数据看板，作为 Clawd Mochi 实体玩偶的 GUI 伴侣。

## 架构

```
浏览器 (localhost:3000)
     ↓
Cockpit Server (Express + SQLite)
     ├─ 只读 daemon SQLite (~/.local/share/clawd-daemon/clawd.db)
     │  └─ 来源: tool_events / daily_stats
     ├─ HTTP → daemon (:7878)
     │  ├─ /event/pre_tool      → 转发控制指令
     │  ├─ /event/stop          → 标记完成
     │  └─ /health              → 健康检查
     └─ 读写 cockpit.db (本目录)
        └─ ritual_logs, presets
```

**状态推断机制**：由于 daemon 未暴露 HTTP `/status` 端点，Cockpit 通过分析 `tool_events` 表的最近记录来推断当前状态：
- 最近 5s 内有工具调用 → **thinking** 🟦
- 最近 15s 内有调用失败 → **error** 🟥
- 最近 30s 内有调用 → **done** 🟩
- 超过 30s 无活动 → **idle** 🟧

每 2s 轮询一次，虚拟 Clawd 的表情和颜色同步跟随推断状态。

## 启动

```bash
cd clawd-mochi/h5-cockpit
npm install
npm start
```

打开 http://localhost:3000

## 功能

| Tab | 功能 |
|-----|------|
| 📊 今日驾驶舱 | 远程控制 (4 种表情 / 5 色心情灯 / 情景模式预设) + 今日工具/Token/会话/错误 统计 + 条形排行 |
| ⏪ 时间回放 | 按日期查看 5 分钟间隔的工具调用热力图（红=错误 / 蓝=活跃 / 橙=轻量）；悬停查看详情并**重现**那一刻的状态到玩偶 |
| 🌅 仪式 | 早安/午休/晚安/下班打卡，打卡同时触发玩偶状态序列（早安闪烁 3 次），90 天热力图，连续天数追踪 |
| 📖 年鉴 | 周报/月报/年报，每日趋势柱状图，最肝一天，Clawd 评语（25 条模板库，按数据哈希稳定选取） |

## 依赖

- Node.js 20+
- daemon (`clawd-daemon`) 已配置并运行（可选；不运行时只能看数据，遥控功能禁用）
- ESP32 实体玩偶（可选；可以纯软件运行）

## 数据流

1. **数据查询**：Cockpit 只读 daemon 的 SQLite 文件，不修改任何 daemon 数据
2. **状态推送**：Cockpit 调 daemon `/event/*` 端点触发玩偶状态变化
3. **状态同步**：每 2s 从 daemon 数据库推断当前状态，虚拟 Clawd 同步显示
4. **仪式 & 预设**：存在 Cockpit 自有的 `cockpit.db`，与 daemon 数据隔离

## 文件结构

```
h5-cockpit/
├── server.js          # Express 后端（~440 行，全部 API 路由）
├── public/
│   ├── index.html     # 4 Tab 单页应用（~140 行语义 HTML）
│   ├── style.css      # 深色主题 + Clawd CSS 像素图标（5 色状态 / 4 种表情）
│   └── app.js         # 前端逻辑：轮询 / Canvas 时间轴 / 仪式序列 / 评论引擎
├── package.json
├── cockpit.db         # 自动生成；ritual_logs + presets
└── README.md
```

## 端点速查

| 方法 | 路径 | 来源 | 说明 | 新增 |
|------|------|------|------|------|
| GET | /api/today | daemon DB | 今日聚合 (tools/tokens/sessions/errors/top_tools) |
| GET | /api/year?range=week\|month\|year | daemon DB | 周/月/年聚合 + 趋势 + 最肝一天 |
| GET | /api/timeline?date=YYYY-MM-DD | daemon DB | 5-min 桶 (含 state/errors/top_tool) |
| GET | /api/status | daemon HTTP + DB | 健康检查 + 推断状态 + 最近工具 |
| POST | /api/control `{state, tool, task}` | daemon HTTP | 转发到 daemon event |
| POST | /api/replay `{state}` | daemon HTTP | 重现历史状态，5s 自动恢复 | ✅ |
| GET | /api/rituals?days=N | cockpit DB | 仪式打卡历史 (按天分组) |
| POST | /api/rituals `{type}` | cockpit DB | 打卡 + 返回 config (color/emote/flash) |
| GET | /api/rituals/streak | cockpit DB | 连续天数 (早安/晚安/全勤) |
| GET | /api/presets | cockpit DB | 预设列表 |
| POST | /api/presets `{name, state, color, ...}` | cockpit DB | 新增/更新 (upsert) |
| DELETE | /api/presets/:id | cockpit DB | 删除 (最少保留 1 条) |

## 状态 → 颜色映射

| 状态 | 背景色 | CSS class | 表情 | 含义 |
|------|--------|-----------|------|------|
| idle | `#ff6b35` 暖橙 | `.state-idle` | 😺 正常 | 闲置等待中 |
| thinking | `#2563eb` 深蓝 | `.state-thinking` | 😺 正常 | 正在干活 |
| done | `#10b981` 翠绿 | `.state-done` | 😆 笑眼 | 任务完成 |
| awaiting | `#eab308` 姜黄 | `.state-awaiting` | 😺 正常 | 等待用户 |
| error | `#ef4444` 珊瑚红 | `.state-error` | 😵 X 眼 | 出错了 |

## 时间轴状态着色

每个 5-min bucket 根据内容染色：
- **红色** `#ef4444` — bucket 内至少 1 次工具调用失败
- **蓝色** `#2563eb` — bucket 内 ≥5 次工具调用（密集活跃）
- **橙色** `#ff6b35` — bucket 内 1-4 次调用（轻量）

## 仪式序列

点击仪式按钮时，除记录打卡外还会触发玩偶状态：

| 仪式 | 背景色 | 表情 | 特殊效果 |
|------|--------|------|----------|
| ☀️ 早安 | 暖橙 | 正常 | 闪烁 3 次 (200ms 间隔) |
| 🍱 午休 | 姜黄 | 困倦 | — |
| 🌙 晚安 | 暗色 | 困倦 | — |
| 🚀 下班 | 翠绿 | 笑眼 | 3s 后自动复位 |

## 年鉴评论池

25 条评论模板，分为 5 个类别，按 (calls + errors + tokens/1000) 哈希稳定选取：

| 条件 | 模板数 | 示例 |
|------|--------|------|
| 本周 >500 次 | 5 | "这把键盘按到飞起。" |
| 本周 <10 次 | 5 | "清闲的一周。Clawd 也在摸鱼。" |
| 错误率 >5% | 5 | "Bug 比想象中顽固。" |
| Token >150k | 5 | "上下文已经快撑爆。" |
| 其他 | 5 | "平稳的输出曲线。Clawd 看得很舒服。" |

## 离线降级

- daemon 未启动 → 状态栏红色 "daemon ❌"；表情/预设/仪式按钮置灰禁用；**颜色按钮保持可用**（本地预览）；数据面板正常显示历史记录
- daemon 运行中 ESP32 不在线 → 状态栏绿色 "daemon ✅"；控制指令静默失败但本地预览正常

## 开发

```bash
# 启动开发服务器
npm start

# 数据库检查
node -e "console.log(require('better-sqlite3')('./cockpit.db').prepare('SELECT * FROM presets').all())"

# 重置预设（清空+重启即重新 seed）
rm cockpit.db && npm start
```
