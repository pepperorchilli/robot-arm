<script setup>
/**
 * 单个舵机 —— 紧凑的一行
 *
 * 以前每个舵机占三行（滑块 / 5 个预设角度 / 4 个微调按钮），
 * 6 个轴加起来 660px，一屏放不下，必须滚动。
 * 改成一行后 6 个轴约 280px，一屏能全部看到。
 *
 * 角度调整方式保留三种：
 *   拖滑块（粗调）· ±1 / ±5 按钮（微调）· 顶部预设姿态（整体）
 */
const model = defineModel({ type: Number, required: true })

defineProps({
  index: { type: Number, required: true },
  name: { type: String, required: true },
  disabled: { type: Boolean, default: false },
})

function step(delta) {
  model.value = Math.min(180, Math.max(0, model.value + delta))
}
</script>

<template>
  <div class="row">
    <span class="name">{{ name }}</span>

    <button class="adj" :disabled="disabled" @click="step(-5)" title="减 5 度">−5</button>
    <button class="adj" :disabled="disabled" @click="step(-1)" title="减 1 度">−1</button>

    <input
      class="slider"
      type="range"
      min="0"
      max="180"
      :value="model"
      :disabled="disabled"
      @input="model = Number($event.target.value)"
    />

    <button class="adj" :disabled="disabled" @click="step(1)" title="加 1 度">+1</button>
    <button class="adj" :disabled="disabled" @click="step(5)" title="加 5 度">+5</button>

    <span class="value">{{ model }}°</span>
  </div>
</template>

<style scoped>
.row {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 7px 10px;
  margin-bottom: 6px;
  border-radius: 10px;
  background: rgba(255, 255, 255, 0.06);
}

.name {
  flex: 0 0 3.2em;
  font-size: 12.5px;
  font-weight: 600;
  color: #e8e8e8;
}

.slider {
  flex: 1;
  min-width: 0;              /* 允许收缩，否则在小屏上会把整行撑破 */
  -webkit-appearance: none;
  appearance: none;
  height: 5px;
  border-radius: 3px;
  background: rgba(255, 255, 255, 0.16);
  outline: none;
  cursor: pointer;
}

.slider::-webkit-slider-thumb {
  -webkit-appearance: none;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: #fff;
  border: 2px solid #000;
  cursor: pointer;
}

.slider:disabled {
  opacity: 0.4;
}

.adj {
  flex: 0 0 auto;
  width: 26px;
  padding: 4px 0;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 6px;
  background: rgba(255, 255, 255, 0.05);
  color: #bbb;
  font-size: 10.5px;
  cursor: pointer;
  transition: all 0.15s;
}

.adj:hover:not(:disabled) {
  background: rgba(255, 255, 255, 0.18);
  color: #fff;
}

.adj:disabled {
  opacity: 0.3;
  cursor: not-allowed;
}

.value {
  flex: 0 0 2.6em;
  text-align: right;
  font-size: 13px;
  font-weight: 700;
  color: #fff;
  font-variant-numeric: tabular-nums;   /* 数字等宽，变化时不跳动 */
}

/* 窄屏：微调按钮藏掉几个，优先保证滑块够宽 */
@media (max-width: 380px) {
  .name { flex-basis: 2.6em; font-size: 11.5px; }
  .value { flex-basis: 2.2em; font-size: 12px; }
  .adj { width: 22px; font-size: 10px; }
}
</style>
