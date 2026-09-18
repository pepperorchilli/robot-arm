<script setup>
import ServoCard from './components/ServoCard.vue'
import { useArmControl, SERVO_NAMES, PRESETS } from './composables/useArmControl.js'

const {
  angles,
  status,
  online,
  sending,
  setAngle,
  setAngleNow,
  resetAll,
  applyPreset,
} = useArmControl()
</script>

<template>
  <nav class="nav">
    <a href="/" class="brand">机械臂</a>
    <div class="links">
      <a href="/">首页</a>
      <a href="/control" class="on">遥控</a>
      <a href="/messages">留言</a>
      <a href="/download">下载</a>
    </div>
  </nav>

  <div class="container">
    <h1>机械臂遥控</h1>
    <p class="subtitle" :class="{ warn: !online }">
      <span class="dot" :class="online ? 'ok' : 'bad'"></span>
      {{ status }}
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

    <div class="footer">Vue 3 + Vite · WebSocket 中转</div>
  </div>
</template>

<style scoped>
.nav {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 14px 16px;
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
  color: #8a8aa0;
  text-decoration: none;
  font-size: 14px;
  margin-left: 16px;
  transition: color 0.15s;
}

.links a:hover,
.links a.on {
  color: #00e5ff;
}

.container {
  max-width: 720px;
  margin: 0 auto;
  padding: 0 16px 40px;
}

h1 {
  font-size: 24px;
  margin-bottom: 6px;
}

.subtitle {
  font-size: 13px;
  color: #8a8aa0;
  margin-bottom: 16px;
  display: flex;
  align-items: center;
  gap: 6px;
}

.subtitle.warn {
  color: #ff6b6b;
}

.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.dot.ok {
  background: #2ee59d;
  box-shadow: 0 0 6px #2ee59d;
}

.dot.bad {
  background: #ff6b6b;
  box-shadow: 0 0 6px #ff6b6b;
}

.presets {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 8px;
  margin-bottom: 12px;
}

.presets button {
  padding: 10px 0;
  border: 1px solid rgba(0, 229, 255, 0.25);
  border-radius: 10px;
  background: rgba(0, 229, 255, 0.08);
  color: #00e5ff;
  font-size: 13px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.15s;
}

.presets button:hover:not(:disabled) {
  background: rgba(0, 229, 255, 0.2);
}

.reset {
  width: 100%;
  padding: 14px;
  border: none;
  border-radius: 12px;
  background: #ff6b6b;
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
  color: #55556a;
  margin-top: 24px;
}
</style>
