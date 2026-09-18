# 机械臂遥控页（Vue 3 + Vite）

机械臂网页遥控端的 Vue 3 重写版，替代原来的单文件 `public/control.html`。

---

## 技术栈

| | |
|---|---|
| 框架 | Vue 3.5（Composition API + `<script setup>`） |
| 构建 | Vite 8 |
| 组件 | 单文件组件（SFC），`<style scoped>` 样式隔离 |
| 状态 | `reactive` / `ref` / `defineModel` |
| 通信 | Fetch API，节流发送 |

---

## 目录结构

```
web/
├── index.html                     # SPA 入口
├── vite.config.js                 # 构建配置（输出到 ../public/control）
└── src/
    ├── main.js                    # 应用入口
    ├── style.css                  # 全局样式
    ├── App.vue                    # 主页面：布局 + 预设 + 组合子组件
    ├── components/
    │   └── ServoCard.vue          # 单个舵机控制卡片
    └── composables/
        └── useArmControl.js       # 控制逻辑（角度状态、节流、API 调用）
```

---

## 设计要点

### 1. 逻辑与视图分离

控制逻辑全在 `composables/useArmControl.js`，组件只管渲染。

**好处**：将来把 HTTP 换成 WebSocket、或接入手势输入时，组件一行都不用改。
这也是 Vue 3 Composition API 相比 Options API 最大的价值 —— **按功能组织代码，而不是按选项类型堆在一起**。

### 2. `defineModel()` 实现组件双向绑定

子组件 `ServoCard.vue` 里：

```js
const model = defineModel({ type: Number, required: true })
```

父组件直接写 `v-model` 就能双向绑定，子组件不需要知道父组件怎么存数据。
这是 Vue 3.4+ 的语法糖，等价于手写 `props` + `emits('update:modelValue')`。

### 3. 节流，避免打爆 ESP32

拖动滑块每秒会触发几十次事件。每次请求都会经服务器转发给 ESP32，
不节流的话舵机会收到大量冗余指令。

`useArmControl.js` 里做了 60ms 节流：**拖动过程中只保留最后一次，松手才真正发送**。
但按钮点击（精确跳转）走 `setAngleNow()`，不做节流 —— 用户点了就应该立刻响应。

### 4. 构建产物不入库

`vite.config.js` 把产物输出到 `../public/control`，由 Express 的 `express.static` 托管。
该目录已在 `.gitignore` 中排除，部署时需要先构建。

---

## 开发与构建

```bash
# 首次：安装依赖（前后端一起）
npm run setup

# 开发模式（热更新，API 请求自动代理到 :3000）
npm run dev:web

# 构建（产物输出到 ../public/control/）
npm run build

# 启动服务器
npm start
```

开发模式下 Vite 跑在 5173，`vite.config.js` 里配了代理：

```js
proxy: {
  '/set': 'http://localhost:3000',
  '/api': 'http://localhost:3000',
}
```

所以开发时不需要处理跨域。

---

## 部署

```bash
git pull
npm install
npm run build      # ← 别漏了这步，产物不入库
npm start
```

---

## 和手势模块的关系

两者是平级的客户端，都通过 HTTP 调同一个接口：

```
Vue 网页遥控 ──┐
               ├──HTTP /set?servo=i&angle=a──> Node 服务器 ──WebSocket──> ESP32
Python 手势 ───┘
```

详见 `gesture-control/README.md`。
