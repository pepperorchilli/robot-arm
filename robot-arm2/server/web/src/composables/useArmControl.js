import { ref, reactive, readonly } from 'vue'

// 舵机名称，顺序与固件一致（对应 SO-ARM101 的 6 个关节，自下而上）
export const SERVO_NAMES = ['底座', '肩部', '肘部', '腕俯仰', '腕旋转', '夹爪']

// 预设姿态
// ⚠️ 这些是示意值，装好机械臂后需要按实际机械限位标定
export const PRESETS = [
  { key: 'home', label: '复位', angles: [90, 90, 90, 90, 90, 90] },
  { key: 'ready', label: '预备', angles: [90, 120, 60, 90, 90, 150] },
  { key: 'grab', label: '抓取', angles: [90, 135, 45, 90, 90, 150] },
  { key: 'drop', label: '放下', angles: [90, 60, 120, 90, 90, 150] },
]

const ANGLE_MIN = 0
const ANGLE_MAX = 180

export function clamp(v) {
  return Math.min(ANGLE_MAX, Math.max(ANGLE_MIN, Math.round(v)))
}

/**
 * 机械臂控制逻辑，抽成 composable 方便复用和单元测试。
 *
 * 为什么不直接在组件里写 fetch：
 *   组件只管"长什么样"，逻辑留在这一层，将来换成 WebSocket
 *   或加手势输入时，组件一行都不用改。
 */
export function useArmControl(options = {}) {
  const throttleMs = options.throttleMs ?? 60

  // 5 个舵机的当前角度
  const angles = reactive([90, 90, 90, 90, 90])

  const status = ref('就绪')
  const online = ref(true)   // 服务器/设备是否可用
  const sending = ref(false)

  // 每个舵机的节流计时器：拖滑块会高频触发，
  // 不节流的话每个像素都发一次请求，把 ESP32 打爆
  const timers = new Array(SERVO_NAMES.length).fill(null)
  const pending = new Array(SERVO_NAMES.length).fill(null)

  async function send(index, angle) {
    sending.value = true
    try {
      const res = await fetch(`/set?servo=${index}&angle=${angle}`)
      if (!res.ok) {
        const text = await res.text()
        online.value = false
        status.value = text || '设备未连接'
        return false
      }
      online.value = true
      status.value = `${SERVO_NAMES[index]} → ${angle}°`
      return true
    } catch (e) {
      online.value = false
      status.value = '服务器无响应'
      return false
    } finally {
      sending.value = false
    }
  }

  /**
   * 设置某个舵机角度（带节流）
   * 拖动过程中只发最后一次，避免刷屏
   */
  function setAngle(index, angle) {
    const a = clamp(angle)
    angles[index] = a

    pending[index] = a
    if (timers[index]) return

    timers[index] = setTimeout(() => {
      timers[index] = null
      if (pending[index] !== null) {
        send(index, pending[index])
        pending[index] = null
      }
    }, throttleMs)
  }

  /** 立即发送（不做节流），用于按钮点击 */
  function setAngleNow(index, angle) {
    const a = clamp(angle)
    angles[index] = a
    pending[index] = null
    if (timers[index]) {
      clearTimeout(timers[index])
      timers[index] = null
    }
    return send(index, a)
  }

  /** 全部复位到指定角度 */
  async function resetAll(angle = 90) {
    for (let i = 0; i < SERVO_NAMES.length; i++) {
      angles[i] = clamp(angle)
      await send(i, clamp(angle))
      // 服务端是逐个接口，稍作间隔避免瞬时并发
      await new Promise((r) => setTimeout(r, 80))
    }
    status.value = `全部回中 ${angle}°`
  }

  /** 应用预设姿态 */
  async function applyPreset(preset) {
    for (let i = 0; i < preset.angles.length; i++) {
      angles[i] = preset.angles[i]
      await send(i, preset.angles[i])
      await new Promise((r) => setTimeout(r, 80))
    }
    status.value = `已应用「${preset.label}」`
  }

  return {
    angles,
    status,
    online: readonly(online),
    sending: readonly(sending),
    setAngle,
    setAngleNow,
    resetAll,
    applyPreset,
  }
}
