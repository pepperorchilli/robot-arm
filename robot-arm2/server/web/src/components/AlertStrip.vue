<script setup>
/**
 * 事件提示条 —— 碰撞 / 过温 / 低压。
 *
 * 为什么要有：碰撞回退和过温保护都是**自动动作**，做完了如果界面上
 * 什么都不说，用户只会看到"机械臂自己缩回去了""不动了"，
 * 第一反应是"坏了"。把原因写出来，自动保护才像个功能而不是故障。
 *
 * 只显示最近几条：这些是瞬时事件，堆一屏反而看不见最新的。
 */
defineProps({
  alerts: { type: Array, required: true },
  describe: { type: Function, required: true },
  isSevere: { type: Function, required: true },
})

function ago(ts) {
  const s = Math.floor((Date.now() - ts) / 1000)
  if (s < 5) return '刚刚'
  if (s < 60) return s + ' 秒前'
  return Math.floor(s / 60) + ' 分钟前'
}
</script>

<template>
  <div v-if="alerts.length" class="alerts">
    <!-- 倒序：最新的在最上面 -->
    <div
      v-for="(e, i) in alerts.slice(-3).reverse()"
      :key="e.at + '-' + i"
      class="alert"
      :class="{ severe: isSevere(e) }"
    >
      <span class="msg">{{ describe(e) }}</span>
      <span class="when">{{ ago(e.at) }}</span>
    </div>
  </div>
</template>

<style scoped>
.alerts {
  margin-top: 10px;
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.alert {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 10px;
  padding: 8px 11px;
  border-radius: 8px;
  font-size: 12.5px;
  background: rgba(255, 255, 255, 0.07);
  border-left: 3px solid #888;
  color: #d0d0d0;
}

/* 严重事件（会改变设备的动作）用暖色边 —— 提示类保持中性灰 */
.alert.severe {
  border-left-color: #f2705f;
  background: rgba(242, 112, 95, 0.12);
  color: #ffd9d3;
}

.when {
  flex-shrink: 0;
  font-size: 11px;
  color: #888;
}

.msg {
  min-width: 0;
}
</style>
