import { ref, computed, readonly } from 'vue'
import { SERVO_NAMES } from './useArmControl.js'

/**
 * 力反馈：从 /api/telemetry 拉每个关节的实时状态。
 *
 * 为什么是轮询而不是 WebSocket：
 *   浏览器这边只需要"当前值"，不需要事件流的时序保证。
 *   轮询写起来简单、断线自愈、也不用为仪表盘再维护一条 WS 通道。
 *   500ms 的间隔对"看手臂使多大劲"这个用途完全够。
 *
 * 固件是一轮上报一个关节的（读寄存器是阻塞的，一次读 6 个会
 * 卡断 WebSocket 心跳）。所以刚打开页面时某些关节还是 null ——
 * 那是"还没轮到它"，不是故障，界面别当错误显示。
 */

const POLL_MS = 500

export function useTelemetry(options = {}) {
  const pollMs = options.pollMs ?? POLL_MS

  // 每个关节：{ pos, deg, load: {magnitude, percent, clockwise}, volt, temp, cur }
  // 没数据的关节保持 null，别填 0 —— 0% 负载和"不知道"是两回事。
  const joints = ref(Array(SERVO_NAMES.length).fill(null))

  const available = ref(false)   // 遥测是否新鲜（设备在线且数据没超时）
  const ageMs = ref(null)        // 最后一帧数据有多旧
  const alerts = ref([])         // 最近的事件（碰撞/过温/低压…）
  const torqueOn = ref(true)     // 舵机是否还带着力

  // 整机电压：所有舵机读的是同一块电池，取第一个有效值就行
  const packVolt = computed(() => {
    const j = joints.value.find((x) => x && x.volt)
    return j ? j.volt : null
  })

  // 最烫的那个关节 —— 过温保护真正关心的是峰值，不是平均值
  const maxTemp = computed(() => {
    const temps = joints.value.filter((x) => x && x.temp).map((x) => x.temp)
    return temps.length ? Math.max(...temps) : null
  })

  // 当前总负载百分比（各关节取最大）。用来一眼看出"是不是顶住了"。
  const peakLoad = computed(() => {
    const loads = joints.value.filter((x) => x && x.load).map((x) => x.load.percent)
    return loads.length ? Math.max(...loads) : null
  })

  const hasAlert = computed(() => alerts.value.length > 0)

  async function fetchTelemetry() {
    try {
      const res = await fetch('/api/telemetry')
      if (res.status === 401) return          // 登录过期交给 useArmControl 处理
      const data = await res.json()

      available.value = data.available === true
      if (!data.available) return

      ageMs.value = data.ageMs
      joints.value = data.joints
      alerts.value = data.events || []
      if (typeof data.torqueOn === 'boolean') torqueOn.value = data.torqueOn
    } catch {
      // 网络抖动就跳过这一轮，不要把 available 设成 false ——
      // 一次请求失败不代表设备掉线，下一轮大概率就恢复了。
    }
  }

  let timer = null

  function start() {
    fetchTelemetry()
    if (timer) clearInterval(timer)
    timer = setInterval(fetchTelemetry, pollMs)
  }

  function stop() {
    if (timer) clearInterval(timer)
    timer = null
  }

  // 事件类型 → 中文说明。
  // 放这里而不是模板里，是为了让"新增一种事件"只改一个地方。
  const EVENT_TEXT = {
    collision: (e) => `碰撞：${SERVO_NAMES[e.joint] ?? '关节' + e.joint}`,
    overtemp: (e) => `过温：${SERVO_NAMES[e.joint] ?? '关节' + e.joint} ${e.value}°C，已松力`,
    overtemp_warn: (e) => `温度偏高：${SERVO_NAMES[e.joint] ?? '关节' + e.joint} ${e.value}°C`,
    lowbat: (e) => `电压低：${(e.value / 10).toFixed(1)}V，请充电`,
    stall: (e) => `堵转：${SERVO_NAMES[e.joint] ?? '关节' + e.joint}`,
  }

  function describe(e) {
    const fn = EVENT_TEXT[e.type]
    return fn ? fn(e) : `${e.type}${e.joint != null ? ' · ' + e.joint : ''}`
  }

  /** 事件是否算"警报"（红色），还是只是提示 */
  function isSevere(e) {
    return e.type === 'collision' || e.type === 'overtemp' || e.type === 'lowbat' || e.type === 'stall'
  }

  return {
    joints: readonly(joints),
    available: readonly(available),
    ageMs: readonly(ageMs),
    alerts: readonly(alerts),
    hasAlert,
    torqueOn: readonly(torqueOn),
    packVolt,
    maxTemp,
    peakLoad,
    describe,
    isSevere,
    start,
    stop,
  }
}
