<script setup>
/**
 * 力反馈仪表盘 —— 每个关节一行：负载条 + 温度 + 电流。
 *
 * 为什么负载用"条"而不是数字：
 *   这个面板的用途是**余光扫一眼就知道有没有顶住**，
 *   数字要读，条不用。数字放右边当精确值。
 *
 * 负载 100% = 舵机额定扭矩。长期贴着 100% 就是在烧舵机，
 * 所以 >70% 就变色提醒。
 */
import { computed } from 'vue'

const props = defineProps({
  names: { type: Array, required: true },
  joints: { type: Array, required: true },   // 元素可能是 null（还没轮到它上报）
  available: { type: Boolean, default: false },
})

// 负载等级 → 颜色。分界值参考 STS3215 的额定扭矩：
// 持续 70% 以上就已经在明显发热了。
function level(pct) {
  if (pct >= 85) return 'hot'
  if (pct >= 70) return 'warm'
  return 'ok'
}

const rows = computed(() =>
  props.names.map((name, i) => ({ name, i, d: props.joints[i] || null }))
)
</script>

<template>
  <section class="gauge">
    <div class="head">
      <h3>力反馈</h3>
      <span v-if="!available" class="muted">暂无数据</span>
    </div>

    <p v-if="!available" class="hint">
      设备未连接，或固件还没开始上报遥测。
    </p>

    <template v-else>
      <div v-for="r in rows" :key="r.i" class="grow">
        <span class="gname">{{ r.name }}</span>

        <!-- 没数据的关节显示灰条，不要显示 0% ——
             "没读到"和"负载为零"是两回事，混淆会让人误判 -->
        <div class="track">
          <div
            v-if="r.d && r.d.load"
            class="fill"
            :class="level(r.d.load.percent)"
            :style="{ width: Math.min(100, r.d.load.percent) + '%' }"
          ></div>
          <div v-else class="fill unknown"></div>
        </div>

        <span class="gval">
          <template v-if="r.d && r.d.load">{{ r.d.load.percent.toFixed(0) }}%</template>
          <template v-else>—</template>
        </span>

        <span class="gsub" :title="'电流 ' + (r.d && r.d.cur != null ? r.d.cur + ' mA' : '未知')">
          <template v-if="r.d && r.d.temp != null">{{ r.d.temp }}°C</template>
        </span>
      </div>
    </template>
  </section>
</template>

<style scoped>
.gauge {
  margin-top: 14px;
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

.muted {
  font-size: 11.5px;
  color: #777;
}

.hint {
  font-size: 12px;
  color: #777;
  margin: 0;
}

.grow {
  display: flex;
  align-items: center;
  gap: 8px;
  height: 20px;
}

.gname {
  flex: 0 0 3.2em;
  font-size: 11.5px;
  color: #a8a8a8;
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

/* 无数据：一条细灰线，表示"这个格子还是空的"而不是"值为 0" */
.fill.unknown {
  width: 100%;
  background: repeating-linear-gradient(
    90deg,
    rgba(255, 255, 255, 0.08) 0 6px,
    transparent 6px 12px
  );
}

.gval {
  flex: 0 0 2.6em;
  text-align: right;
  font-size: 11.5px;
  font-weight: 600;
  color: #ddd;
  font-variant-numeric: tabular-nums;
}

.gsub {
  flex: 0 0 2.8em;
  text-align: right;
  font-size: 11px;
  color: #808080;
  font-variant-numeric: tabular-nums;
}
</style>
