<script setup>
/**
 * 单个舵机的控制卡片
 *
 * 用 defineModel() 实现 v-model：父组件写 v-model="angles[i]" 即可双向绑定，
 * 子组件不需要知道父组件怎么存数据。
 */
const model = defineModel({ type: Number, required: true })

const props = defineProps({
  index: { type: Number, required: true },
  name: { type: String, required: true },
  disabled: { type: Boolean, default: false },
})

const emit = defineEmits(['jump'])

function step(delta) {
  model.value = Math.min(180, Math.max(0, model.value + delta))
}

function jump(angle) {
  model.value = angle
  emit('jump', angle)   // 按钮点击要立即发送，不走节流
}
</script>

<template>
  <div class="card">
    <div class="head">
      <span class="name">{{ name }}</span>
      <span class="value">{{ model }}°</span>
    </div>

    <input
      class="slider"
      type="range"
      min="0"
      max="180"
      :value="model"
      :disabled="disabled"
      @input="model = Number($event.target.value)"
    />

    <div class="row">
      <button v-for="a in [0, 45, 90, 135, 180]" :key="a"
              :class="{ on: model === a }"
              :disabled="disabled"
              @click="jump(a)">{{ a }}°</button>
    </div>

    <div class="row fine">
      <button :disabled="disabled" @click="step(-5)">-5</button>
      <button :disabled="disabled" @click="step(-1)">-1</button>
      <button :disabled="disabled" @click="step(1)">+1</button>
      <button :disabled="disabled" @click="step(5)">+5</button>
    </div>
  </div>
</template>

<style scoped>
.card {
  background: rgba(255, 255, 255, 0.06);
  border-radius: 12px;
  padding: 10px 12px;
  margin-bottom: 7px;
}

.head {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  margin-bottom: 5px;
}

.name {
  font-size: 14px;
  font-weight: 600;
}

.value {
  font-size: 18px;
  font-weight: 700;
  color: #ffffff;
  font-variant-numeric: tabular-nums;  /* 数字等宽，变化时不跳动 */
}

.slider {
  -webkit-appearance: none;
  appearance: none;
  width: 100%;
  height: 6px;
  border-radius: 3px;
  background: rgba(255, 255, 255, 0.15);
  outline: none;
  margin-bottom: 7px;
}

.slider::-webkit-slider-thumb {
  -webkit-appearance: none;
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background: #ffffff;
  border: 2px solid #fff;
  cursor: pointer;
}

.slider:disabled {
  opacity: 0.4;
}

.row {
  display: grid;
  grid-template-columns: repeat(5, 1fr);
  gap: 6px;
}

.row.fine {
  grid-template-columns: repeat(4, 1fr);
  margin-top: 5px;
}

button {
  padding: 6px 0;
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 7px;
  background: rgba(255, 255, 255, 0.05);
  color: #c8c8c8;
  font-size: 11.5px;
  cursor: pointer;
  transition: all 0.15s;
}

button:hover:not(:disabled) {
  background: rgba(255, 255, 255, 0.15);
  color: #ffffff;
  border-color: rgba(255, 255, 255, 0.4);
}

button.on {
  background: rgba(255, 255, 255, 0.2);
  color: #ffffff;
  border-color: rgba(255, 255, 255, 0.5);
}

button:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}
</style>
