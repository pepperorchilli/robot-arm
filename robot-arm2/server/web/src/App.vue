<script setup>
import { computed, onMounted, onUnmounted } from 'vue'
import ServoCard from './components/ServoCard.vue'
import ForceGauge from './components/ForceGauge.vue'
import AlertStrip from './components/AlertStrip.vue'
import { useArmControl, SERVO_NAMES, PRESETS } from './composables/useArmControl.js'
import { useTelemetry } from './composables/useTelemetry.js'

const {
  angles,
  status,
  online,
  sending,
  account,
  authed,
  checkingAuth,
  checkAuth,
  goLogin,
  logout,
  deviceOnline,
  deviceSeconds,
  startDevicePolling,
  setAngle,
  setAngleNow,
  resetAll,
  applyPreset,
} = useArmControl()

const {
  joints,
  available: telemAvailable,
  alerts,
  torqueOn,
  packVolt,
  maxTemp,
  describe,
  isSevere,
  start: startTelemetry,
  stop: stopTelemetry,
} = useTelemetry()

// 设备在线时长的可读形式
const deviceUptime = computed(() => {
  const s = deviceSeconds.value
  if (!s) return ''
  if (s < 60) return `${s} 秒`
  const m = Math.floor(s / 60)
  if (m < 60) return `${m} 分钟`
  return `${Math.floor(m / 60)} 小时 ${m % 60} 分`
})

onMounted(async () => {
  await checkAuth()
  // 未登录直接送去登录页，登录后会自动跳回来
  if (!authed.value) goLogin()
  else {
    startDevicePolling()
    startTelemetry()
  }
})

// 离开页面就停掉轮询 —— 不然在后台还会一直打服务器
onUnmounted(stopTelemetry)
</script>

<template>
  <nav class="nav">
    <a href="/" class="brand">个人网站</a>
    <div class="links">
      <a href="/">首页</a>
      <a href="/control" class="on">机械臂</a>
      <a href="/library/">图书管理</a>
      <a href="/messages">留言板</a>
    </div>
  </nav>

  <div class="container">
    <!-- 顶栏：版本切换 + 账号，一行放完，不占竖向空间 -->
    <div class="topbar">
      <div class="verbar">
        <a href="/control" class="on">新版 6 轴</a>
        <a href="/control/v1">旧版 5 轴</a>
      </div>
      <div v-if="account" class="userbar">
        <span class="uname">{{ account.nickname }}</span>
        <span v-if="account.role === 'admin'" class="badge">管理员</span>
        <button class="logout" @click="logout">退出</button>
      </div>
    </div>

    <p v-if="checkingAuth" class="subtitle">正在检查登录状态…</p>

    <template v-else-if="authed">
      <!-- 状态行：设备在线与否 + 上一条指令结果，合成一行 -->
      <p class="statusline" :class="{ offline: !deviceOnline }">
        <span class="dot" :class="deviceOnline ? 'ok' : 'bad'"></span>
        <template v-if="deviceOnline">
          在线<span v-if="deviceUptime"> {{ deviceUptime }}</span>
        </template>
        <template v-else>离线</template>
        <span class="sep">·</span>
        <span class="stat">{{ status }}</span>
      </p>

      <div class="presets">
        <button v-for="p in PRESETS" :key="p.key"
                :disabled="sending"
                @click="applyPreset(p)">{{ p.label }}</button>
      </div>

      <ServoCard
        v-for="(name, i) in SERVO_NAMES"
        :key="i"
        :index="i"
        :name="name"
        :model-value="angles[i]"
        :disabled="sending"
        @update:model-value="(v) => setAngle(i, v)"
        @jump="(v) => setAngleNow(i, v)"
      />

      <button class="reset" :disabled="sending" @click="resetAll(90)">全部回中</button>

      <!-- 电源/保护状态：电压和峰值温度都在这里，一眼看全 -->
      <p v-if="telemAvailable" class="powerline">
        <span v-if="packVolt != null" :class="{ warnvolt: packVolt < 10.5 }">
          电池 {{ packVolt }}V
        </span>
        <span v-if="maxTemp != null" :class="{ warntemp: maxTemp >= 65 }">
          最高 {{ maxTemp }}°C
        </span>
      </p>

      <!-- 保护触发后设备已经松力，控制条虽然还能拖但不会动。
           不说清楚的话，用户只会以为"机械臂坏了" -->
      <p v-if="!torqueOn" class="torquewarn">
        ⚠️ 保护已触发，舵机已松力（过温或低压）。排除原因后重新上电恢复。
      </p>

      <AlertStrip :alerts="alerts" :describe="describe" :is-severe="isSevere" />

      <ForceGauge :names="SERVO_NAMES" :joints="joints" :available="telemAvailable" />

      <div class="footer">Vue 3 + Vite · WebSocket 中转</div>
    </template>

    <p v-else class="subtitle">正在跳转到登录页…</p>
  </div>
</template>

<style scoped>
/* 导航与内容都要压在星空画布之上 ——
   画布是 fixed + z-index:0，没有层级的普通内容会被它盖住 */
.nav {
  position: relative;
  z-index: 1;
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 16px;
  max-width: 720px;
  margin: 0 auto;
}

.brand {
  font-size: 17px;
  font-weight: 700;
  color: #fff;
  text-decoration: none;
}

.links a {
  color: #8a8a8a;
  text-decoration: none;
  font-size: 14px;
  margin-left: 16px;
  transition: color 0.15s;
}

.links a:hover,
.links a.on {
  color: #ffffff;
}

.container {
  position: relative;
  z-index: 1;                /* 压在星空之上，否则内容会被画布盖住 */
  max-width: 720px;
  margin: 0 auto;
  padding: 0 16px 40px;
}

/* 顶栏：版本切换在左，账号在右，一行放完 —— 不留大标题占竖向空间 */
.topbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  margin-bottom: 10px;
}

/* 两代控制台切换（做小一点，别抢控制面板的位置） */
.verbar {
  display: flex;
  gap: 6px;
}

.verbar a {
  padding: 7px 14px;
  border-radius: 8px;
  font-size: 12.5px;
  font-weight: 600;
  text-decoration: none;
  white-space: nowrap;
  transition: all 0.15s;
  border: 1px solid rgba(255, 255, 255, 0.12);
  background: rgba(255, 255, 255, 0.04);
  color: #8a8a8a;
}

.verbar a:hover {
  background: rgba(255, 255, 255, 0.1);
  color: #ccc;
}

.verbar a.on {
  background: rgba(255, 255, 255, 0.14);
  color: #fff;
  border-color: rgba(255, 255, 255, 0.4);
}

/* 状态行：在线状态 + 上一条指令，压成一行 */
.statusline {
  display: flex;
  align-items: center;
  font-size: 12.5px;
  color: #8a8a8a;
  margin-bottom: 10px;
  min-height: 18px;
}

.statusline .sep {
  margin: 0 8px;
  color: #444;
}

.statusline .stat {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.userbar {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

.uname {
  font-size: 13px;
  color: #c8c8c8;
}

.badge {
  font-size: 11px;
  padding: 3px 8px;
  border-radius: 999px;
  background: rgba(255, 255, 255, 0.15);
  color: #ffffff;
  border: 1px solid rgba(255, 255, 255, 0.3);
}

.logout {
  padding: 5px 10px;
  font-size: 12px;
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 6px;
  background: transparent;
  color: #8a8a8a;
  cursor: pointer;
  font-family: inherit;
}

.logout:hover {
  color: #bbbbbb;
  border-color: rgba(187, 187, 187, 0.4);
}

.subtitle {
  font-size: 13px;
  color: #8a8a8a;
  margin-bottom: 16px;
  display: flex;
  align-items: center;
  gap: 6px;
}

.subtitle.warn {
  color: #bbbbbb;
}

.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.dot.ok {
  background: #ffffff;
  box-shadow: 0 0 6px #ffffff;
}

.dot.bad {
  background: #bbbbbb;
  box-shadow: 0 0 6px #bbbbbb;
}

.presets {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 6px;
  margin-bottom: 8px;
}

.presets button {
  padding: 7px 0;
  border: 1px solid rgba(255, 255, 255, 0.25);
  border-radius: 8px;
  background: rgba(255, 255, 255, 0.08);
  color: #ffffff;
  font-size: 12.5px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.15s;
}

.presets button:hover:not(:disabled) {
  background: rgba(255, 255, 255, 0.2);
}

.reset {
  width: 100%;
  padding: 14px;
  border: none;
  border-radius: 12px;
  background: #bbbbbb;
  color: #fff;
  font-size: 15px;
  font-weight: 700;
  cursor: pointer;
  margin-top: 4px;
  transition: opacity 0.15s;
}

.reset:hover:not(:disabled) {
  opacity: 0.85;
}

button:disabled {
  opacity: 0.45;
  cursor: not-allowed;
}

.footer {
  text-align: center;
  font-size: 12px;
  color: #555555;
  margin-top: 24px;
}

/* 电池电压 / 峰值温度，压成一行放在控制区下面 */
.powerline {
  display: flex;
  gap: 14px;
  justify-content: center;
  font-size: 12px;
  color: #8a8a8a;
  margin: 10px 0 0;
  font-variant-numeric: tabular-nums;   /* 数字等宽，刷新时不左右抖 */
}

/* 10.5V 是 3S 锂电该充电的线，65°C 是舵机开始明显发热的点。
   到线了变黄，但不变红 —— 这是"注意"，真正触发保护会有 .torquewarn 那条 */
.powerline .warnvolt,
.powerline .warntemp {
  color: #f5c451;
}

/* 保护触发后舵机已经松力，滑块还能拖但机械臂不会动。
   不说清楚的话，用户只会以为"机械臂坏了" */
.torquewarn {
  margin: 10px 0 0;
  padding: 9px 12px;
  border-radius: 8px;
  font-size: 12.5px;
  background: rgba(242, 112, 95, 0.12);
  border-left: 3px solid #f2705f;
  color: #ffd9d3;
}
</style>
