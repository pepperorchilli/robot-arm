<script setup>
import { ref, onMounted } from 'vue'
import ServoCard from './components/ServoCard.vue'
import { useArmControl, SERVO_NAMES, PRESETS } from './composables/useArmControl.js'

const {
  angles,
  status,
  online,
  sending,
  authed,
  checkingAuth,
  checkAuth,
  login,
  logout,
  setAngle,
  setAngleNow,
  resetAll,
  applyPreset,
} = useArmControl()

const password = ref('')
const loginError = ref('')
const loggingIn = ref(false)

onMounted(checkAuth)

async function doLogin() {
  if (!password.value) {
    loginError.value = '请输入密码'
    return
  }
  loggingIn.value = true
  loginError.value = ''
  try {
    await login(password.value)
    password.value = ''
  } catch (e) {
    loginError.value = e.message
  } finally {
    loggingIn.value = false
  }
}
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
    <h1>机械臂遥控</h1>

    <!-- ============ 正在检查登录状态 ============ -->
    <p v-if="checkingAuth" class="subtitle">正在检查登录状态…</p>

    <!-- ============ 未登录：显示登录框 ============ -->
    <div v-else-if="!authed" class="login-box">
      <div class="lock-icon">🔒</div>
      <h2>需要登录才能控制</h2>
      <p class="login-hint">
        机械臂对所有人可见，但控制需要密码 —— 避免被陌生人误操作。
      </p>
      <input
        type="password"
        v-model="password"
        placeholder="请输入控制密码"
        @keyup.enter="doLogin"
        :disabled="loggingIn"
      />
      <p v-if="loginError" class="login-error">{{ loginError }}</p>
      <button class="login-btn" @click="doLogin" :disabled="loggingIn">
        {{ loggingIn ? '登录中…' : '登录' }}
      </button>
    </div>

    <!-- ============ 已登录：显示控制台 ============ -->
    <template v-else>
      <p class="subtitle" :class="{ warn: !online }">
        <span class="dot" :class="online ? 'ok' : 'bad'"></span>
        {{ status }}
        <button class="logout" @click="logout">退出登录</button>
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
    </template>
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

.logout {
  margin-left: auto;
  padding: 4px 10px;
  font-size: 12px;
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 6px;
  background: transparent;
  color: #8a8aa0;
  cursor: pointer;
}

.logout:hover {
  color: #ff6b6b;
  border-color: rgba(255, 107, 107, 0.4);
}

/* ---------- 登录框 ---------- */
.login-box {
  background: rgba(255, 255, 255, 0.06);
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 16px;
  padding: 32px 24px;
  text-align: center;
  margin-top: 32px;
}

.lock-icon {
  font-size: 36px;
  margin-bottom: 12px;
}

.login-box h2 {
  font-size: 17px;
  font-weight: 600;
  margin-bottom: 10px;
}

.login-hint {
  font-size: 13px;
  color: #8a8aa0;
  margin-bottom: 22px;
  line-height: 1.6;
}

.login-box input {
  width: 100%;
  max-width: 280px;
  padding: 12px 14px;
  border-radius: 10px;
  border: 1px solid rgba(255, 255, 255, 0.15);
  background: rgba(255, 255, 255, 0.08);
  color: #fff;
  font-size: 14px;
  font-family: inherit;
  outline: none;
  text-align: center;
}

.login-box input:focus {
  border-color: rgba(0, 229, 255, 0.5);
}

.login-error {
  color: #ff6b6b;
  font-size: 13px;
  margin-top: 10px;
}

.login-btn {
  display: block;
  width: 100%;
  max-width: 280px;
  margin: 16px auto 0;
  padding: 12px;
  border: none;
  border-radius: 10px;
  background: #00e5ff;
  color: #1a1a2e;
  font-size: 15px;
  font-weight: 700;
  cursor: pointer;
  transition: opacity 0.15s;
}

.login-btn:hover:not(:disabled) {
  opacity: 0.85;
}

.login-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

/* ---------- 控制台 ---------- */
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
