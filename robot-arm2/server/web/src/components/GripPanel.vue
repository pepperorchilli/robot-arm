<script setup>
/**
 * 夹爪夹持 —— 加力 / 退档 / 松力，配上实时的力与角度。
 *
 * 为什么单独做一个面板，而不是继续用夹爪那行滑块：
 *   夹住东西以后，滑块（＝目标角度）就没意义了 —— 你拖到哪它都不会到。
 *   这里真正要看的两个量是**负载有多大**和**实际停在了多少度**，
 *   而且加力是"在当前位置上再顶一点"，不是"去某个绝对角度"。
 *
 * 角度一律显示遥测里的实测值，不做任何平滑或预测：
 *   物体被压扁、手指真的合拢了，数字就跟着动；
 *   顶住不动就是不动 —— 这正是判断"有没有夹住"最直接的依据。
 */
import { computed } from 'vue'

const props = defineProps({
  joint: { type: Object, default: null },     // 遥测里第 6 个关节，可能是 null
  available: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  active: { type: Boolean, default: false },   // 是否正在夹持
  step: { type: Number, default: 0 },          // 已加档数
})

defineEmits(['force', 'release'])

// 45% 是固件里的加力上限（GRIP_MAX_LOAD），到了就不再接受加档
const GRIP_MAX_LOAD = 450

const angle = computed(() =>
  typeof props.joint?.angle === 'number' ? props.joint.angle : null
)

const loadPct = computed(() =>
  props.joint?.load ? props.joint.load.percent : null
)

const atLimit = computed(
  () => props.joint?.load != null && props.joint.load.magnitude >= GRIP_MAX_LOAD
)

const level = computed(() => {
  const p = loadPct.value
  if (p == null) return 'unknown'
  if (p >= 70) return 'hot'
  if (p >= 45) return 'warm'
  return 'ok'
})
</script>

<template>
  <section class="grip">
    <div class="head">
      <h3>夹爪夹持</h3>
      <span class="stat">
        <template v-if="active">已加 {{ step }} 档</template>
        <template v-else>未夹持</template>
      </span>
    </div>

    <div class="readout">
      <div class="cell">
        <span class="k">实际角度</span>
        <span class="v">
          <template v-if="angle != null">{{ angle }}°</template>
          <template v-else>—</template>
        </span>
      </div>

      <div class="cell wide">
        <span class="k">夹持力</span>
        <div class="track">
          <div
            v-if="loadPct != null"
            class="fill"
            :class="level"
            :style="{ width: Math.min(100, loadPct) + '%' }"
          ></div>
          <div v-else class="fill unknown"></div>
        </div>
        <span class="v small">
          <template v-if="loadPct != null">{{ loadPct.toFixed(0) }}%</template>
          <template v-else>—</template>
        </span>
      </div>
    </div>

    <div class="btns">
      <button class="minus" :disabled="disabled || !active" @click="$emit('force', -1)">−1 档</button>
      <button class="plus" :disabled="disabled" @click="$emit('force', 1)">加力</button>
      <button class="release" :disabled="disabled || !active" @click="$emit('release')">松力</button>
    </div>

    <p v-if="!available" class="hint">设备未连接，或固件还没开始上报遥测。</p>
    <p v-else-if="atLimit" class="hint warn">
      已到负载上限（45%），再加也不会更紧。要更用力就先松力，重新夹。
    </p>
  </section>
</template>

<style scoped>
.grip {
  margin-top: 12px;
  padding: 12px;
  border-radius: 12px;
  background: rgba(255, 255, 255, 0.05);
}

.head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  margin-bottom: 10px;
}

h3 {
  margin: 0;
  font-size: 13px;
  font-weight: 700;
  color: #e8e8e8;
}

.stat {
  font-size: 11.5px;
  color: #777;
  font-variant-numeric: tabular-nums;
}

.readout {
  display: flex;
  align-items: center;
  gap: 14px;
  margin-bottom: 10px;
}

.cell {
  display: flex;
  align-items: center;
  gap: 6px;
}

.cell.wide {
  flex: 1;
  min-width: 0;
}

.k {
  font-size: 11.5px;
  color: #8a8a8a;
  flex-shrink: 0;
}

.v {
  font-size: 14px;
  font-weight: 700;
  color: #fff;
  font-variant-numeric: tabular-nums;
  flex-shrink: 0;
}

.v.small {
  font-size: 12px;
  font-weight: 600;
  color: #ddd;
  min-width: 2.8em;
  text-align: right;
}

.track {
  flex: 1;
  min-width: 0;
  height: 7px;
  border-radius: 4px;
  background: rgba(255, 255, 255, 0.1);
  overflow: hidden;
}

.fill {
  height: 100%;
  border-radius: 4px;
  transition: width 0.25s ease, background 0.25s ease;
}

.fill.ok { background: #6ee7a8; }
.fill.warm { background: #f5c451; }
.fill.hot { background: #f2705f; }

.fill.unknown {
  width: 100%;
  background: repeating-linear-gradient(
    90deg,
    rgba(255, 255, 255, 0.08) 0 6px,
    transparent 6px 12px
  );
}

.btns {
  display: grid;
  grid-template-columns: 1fr 1.4fr 1fr;
  gap: 6px;
}

.btns button {
  padding: 9px 0;
  border-radius: 8px;
  font-size: 13px;
  font-weight: 700;
  font-family: inherit;
  cursor: pointer;
  transition: all 0.15s;
  border: 1px solid rgba(255, 255, 255, 0.22);
  background: rgba(255, 255, 255, 0.08);
  color: #fff;
}

.btns button:hover:not(:disabled) {
  background: rgba(255, 255, 255, 0.2);
}

/* 加力是这里唯一"会让舵机更使劲"的动作，做成实心，别的都是描边 */
.btns .plus {
  background: #fff;
  color: #111;
  border-color: #fff;
}

.btns .plus:hover:not(:disabled) {
  background: #ddd;
}

.btns .release {
  color: #bbb;
}

.btns button:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.hint {
  font-size: 11.5px;
  color: #777;
  margin: 9px 0 0;
}

.hint.warn {
  color: #f5c451;
}
</style>
