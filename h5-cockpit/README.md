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
     │  └─ /event/pre_tool, /event/stop, /health
     └─ 读写 cockpit.db (本目录)
        └─ ritual_logs, presets
```

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
| 📊 今日驾驶舱 | 远程控制 (表情/颜色/预设) + 今日工具/Token/会话/错误 统计 + 工具排行 |
| ⏪ 时间回放 | 按日查看 5 分钟间隔的工具调用热力图，悬停查看详情 |
| 🌅 仪式 | 早安/午休/晚安/下班打卡, 90 天热力图, 连续天数追踪 |
| 📖 年鉴 | 周报/月报/年报, 趋势图, Clawd 评语 |

## 依赖

- Node.js 20+
- daemon (`clawd-daemon`) 已配置并运行（可选；不运行时只能看数据，遥控功能禁用）
- ESP32 实体玩偶（可选；可以纯软件运行）

## 数据流

1. **数据查询**：Cockpit 只读 daemon 的 SQLite 文件，不修改任何 daemon 数据
2. **状态推送**：Cockpit 调 daemon `/event/*` 端点触发玩偶状态变化
3. **仪式 & 预设**：存在 Cockpit 自有的 `cockpit.db`，与 daemon 数据隔离

## 文件结构

```
h5-cockpit/
├── server.js          # Express 后端，所有路由集中此处
├── public/
│   ├── index.html     # 4 Tab 单页应用
│   ├── style.css      # 深色主题 + Clawd CSS 像素图标
│   └── app.js         # 前端逻辑：tab 切换/轮询/Canvas 时间轴
├── package.json
├── cockpit.db         # 自动生成；ritual_logs + presets
└── README.md
```

## 端点速查

| 方法 | 路径 | 来源 | 说明 |
|------|------|------|------|
| GET | /api/today | daemon DB | 今日聚合 |
| GET | /api/year?range=week\|month\|year | daemon DB | 周/月/年聚合 |
| GET | /api/timeline?date=YYYY-MM-DD | daemon DB | 5 分钟桶 |
| GET | /api/status | daemon HTTP + DB | 健康检查 + 最近活动 |
| POST | /api/control `{state, tool, task}` | daemon HTTP | 转发到 daemon event |
| GET | /api/rituals?days=N | cockpit DB | 仪式打卡历史 |
| POST | /api/rituals `{type}` | cockpit DB | 记录仪式 |
| GET | /api/rituals/streak | cockpit DB | 连续天数 |
| GET | /api/presets | cockpit DB | 预设列表 |
| POST | /api/presets `{name, state, color, ...}` | cockpit DB | 新增/更新（upsert） |
| DELETE | /api/presets/:id | cockpit DB | 删除 |

## 开发

```bash
# 启动开发服务器
npm start

# 数据库检查
node -e "console.log(require('better-sqlite3')('./cockpit.db').prepare('SELECT * FROM presets').all())"

# 重置预设（清空+重启即重新 seed）
rm cockpit.db && npm start
```
