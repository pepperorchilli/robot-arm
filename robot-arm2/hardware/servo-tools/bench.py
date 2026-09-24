#!/usr/bin/env python3
"""
STS3215 台面测试工具（阶段一：用微雪转接板 + Mac 配置舵机）

不用改这个文件 —— 串口号和舵机 ID 都从命令行传。

用法：
    python3 bench.py scan                       列出所有串口 + 扫描 ID 0~9
    python3 bench.py scan <port>                只扫描某串口上的 ID 0~9
    python3 bench.py info <port> <id>           读位置/电压/温度/负载/电流
    python3 bench.py id   <port> <旧ID> <新ID>  改 ID（写进 EEPROM）
    python3 bench.py move <port> <id> <角度>    转到指定角度（0~360 度）
    python3 bench.py torque <port> <id> <0|1>   松力 / 使能

例子：
    python3 bench.py scan
    python3 bench.py scan /dev/cu.usbmodem1234
    python3 bench.py info /dev/cu.usbmodem1234 1
    python3 bench.py id   /dev/cu.usbmodem1234 1 3
    python3 bench.py move /dev/cu.usbmodem1234 3 90

为什么不直接用官方的 ping.py / change_id.py：
官方脚本把端口和 ID 硬编码在文件里，而且 ping.py 里写的是 ID 254
（总线上不存在这个 ID），原样跑一定失败。这个脚本把这些都变成参数，
省得每换一个舵机就回去改一次文件。
"""

import os
import sys
import glob

# SDK 在 ./scservo_sdk/，和本脚本同级，所以把脚本所在目录加进搜索路径
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scservo_sdk import PortHandler, sms_sts, COMM_SUCCESS, BROADCAST_ID  # noqa: E402

BAUDRATE = 1000000          # STS3215 出厂默认，别改

# STS3215 寄存器地址
REG_ID          = 5
REG_TORQUE_EN   = 40
REG_GOAL_POS    = 42
REG_LOCK        = 55
REG_PRESENT_POS = 56
REG_PRESENT_SPD = 58
REG_PRESENT_LOAD = 60
REG_PRESENT_VOLT = 62
REG_PRESENT_TEMP = 63
REG_PRESENT_CUR  = 69


def list_ports():
    """列出 Mac 上所有可能是 USB 串口的设备"""
    pats = [
        "/dev/cu.usbserial*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.usbmodem*",
        "/dev/cu.SLAB_USBtoUART*",
    ]
    found = []
    for p in pats:
        found.extend(glob.glob(p))
    return sorted(found)


def open_port(dev):
    ph = PortHandler(dev)
    if not ph.openPort():
        print(f"❌ 打不开串口 {dev}")
        print("   检查：转接板插好了吗？驱动装了吗？端口名对吗？（跑 scan 看）")
        return None, None
    if not ph.setBaudRate(BAUDRATE):
        print("❌ 波特率设置失败")
        ph.closePort()
        return None, None
    print(f"✓ 串口已打开：{dev} @ {BAUDRATE} baud")
    return ph, sms_sts(ph)


def ping_one(ph, pk, sid):
    """请求一个 ID，返回 (是否回应, 型号号)"""
    model, result, error = pk.ping(sid)
    if result == COMM_SUCCESS:
        return True, model
    return False, None


def cmd_scan(args):
    ports = [args[0]] if args else list_ports()
    if not ports:
        print("❌ 没找到任何 USB 串口。")
        print("   1. 转接板的 USB-C 插上了吗？")
        print("   2. macOS 14 可能不认 CH343P，需要装 WCH 驱动：")
        print("      https://github.com/WCHSoftGroup/ch34xser_macos")
        return 1

    print(f"找到 {len(ports)} 个串口：")
    for p in ports:
        print(f"  {p}")
    print()

    hit = 0
    for dev in ports:
        ph, pk = open_port(dev)
        if not ph:
            continue
        print("  扫描 ID 0~9 ...")
        for sid in range(10):
            ok, model = ping_one(ph, pk, sid)
            if ok:
                print(f"    ✅ 发现舵机：ID = {sid}，型号号 = {model}")
                hit += 1
        ph.closePort()
        print()

    if hit == 0:
        print("⚠️  一个舵机都没回应。逐条排查：")
        print("   · 舵机的 3 针线插对了吗？GND / DATA / DC_IN0 有没有插反")
        print("   · DC 电源接了吗？是多少伏？（C001 要 5~7.4V，不能 12V）")
        print("   · 舵机线是否插在转接板的总线口上（不是 UART 口）")
        print("   · USB 和 DC 是不是都插上了")
    return 0


def cmd_info(args):
    if len(args) < 2:
        print("用法：python3 bench.py info <port> <id>")
        return 1
    dev, sid = args[0], int(args[1])
    ph, pk = open_port(dev)
    if not ph:
        return 1

    ok, model = ping_one(ph, pk, sid)
    if not ok:
        print(f"❌ ID {sid} 没有回应")
        ph.closePort()
        return 1
    print(f"✓ ID {sid} 在线，型号号 {model}\n")

    def rd2(reg, name, scale=1.0, unit=""):
        val, res, _ = pk.read2ByteTxRx(sid, reg)
        if res != COMM_SUCCESS:
            print(f"  {name:<8} 读取失败")
            return None
        v = val * scale
        print(f"  {name:<8} {v:g}{unit}")
        return v

    def rd1(reg, name, scale=1.0, unit=""):
        val, res, _ = pk.read1ByteTxRx(sid, reg)
        if res != COMM_SUCCESS:
            print(f"  {name:<8} 读取失败")
            return None
        v = val * scale
        print(f"  {name:<8} {v:g}{unit}")
        return v

    print("寄存器原始值：")
    pos = rd2(REG_PRESENT_POS, "位置", 360 / 4096, "°")
    spd = rd2(REG_PRESENT_SPD, "速度")
    load = rd2(REG_PRESENT_LOAD, "负载")
    rd1(REG_PRESENT_VOLT, "电压", 0.1, "V")
    rd1(REG_PRESENT_TEMP, "温度", 1, "°C")
    cur = rd2(REG_PRESENT_CUR, "电流", 6.5, "mA")

    if pos is not None:
        print(f"\n  换算后：角度 {pos:.1f}°")
    if load is not None:
        # rd2 按 scale 换算后返回的是 float，位运算前必须先转回 int
        raw = int(load)
        # 低 10 位是大小（1000 = 额定扭矩的 100%），bit10 是方向位
        mag = raw & 0x3FF
        direction = "正转" if (raw & 0x400) else "反转"
        print(f"          负载 {mag / 10:.1f}%（原始 {raw:#06x}，方向位 {direction}）")
    if cur is not None:
        print(f"          电流 {cur:.0f} mA")

    ph.closePort()
    return 0


def cmd_id(args):
    if len(args) < 3:
        print("用法：python3 bench.py id <port> <旧ID> <新ID>")
        return 1
    dev, old, new = args[0], int(args[1]), int(args[2])
    if not (0 <= new <= 253):
        print("❌ 新 ID 必须在 0~253 之间")
        return 1

    ph, pk = open_port(dev)
    if not ph:
        return 1

    ok, _ = ping_one(ph, pk, old)
    if not ok:
        print(f"❌ 旧 ID {old} 没有回应，中止（别乱改，先确认旧 ID 是对的）")
        ph.closePort()
        return 1

    print(f"  解锁 EEPROM ...")
    res, err = pk.unLockEprom(old)
    if res != COMM_SUCCESS or err != 0:
        print(f"❌ 解锁失败：{pk.getTxRxResult(res)}")
        ph.closePort()
        return 1

    print(f"  写入新 ID {new} ...")
    res, err = pk.write1ByteTxRx(old, REG_ID, new)
    if res != COMM_SUCCESS:
        print(f"❌ 写入失败：{pk.getTxRxResult(res)}")
        ph.closePort()
        return 1

    # 注意：舵机现在已经改名了，要用新 ID 去锁
    # （官方 change_id.py 这里用的是旧 ID，锁不上 —— 所以下面用新 ID）
    res, _ = pk.LockEprom(new)
    if res != COMM_SUCCESS:
        print("  ⚠️  加锁没成功（不影响使用，断电重启会自动锁上）")
    else:
        print("  ✓ 已重新上锁")

    print(f"\n✅ ID 已从 {old} 改成 {new}")
    print("   ⚠️  请拔掉 DC 电源再重新插上，让新 ID 生效")
    print("   然后跑：python3 bench.py info", dev, new, "  验证")

    ph.closePort()
    return 0


def cmd_torque(args):
    if len(args) < 3:
        print("用法：python3 bench.py torque <port> <id> <0|1>")
        return 1
    dev, sid, on = args[0], int(args[1]), int(args[2])
    ph, pk = open_port(dev)
    if not ph:
        return 1
    res, _ = pk.write1ByteTxRx(sid, REG_TORQUE_EN, 1 if on else 0)
    if res != COMM_SUCCESS:
        print(f"❌ 失败：{pk.getTxRxResult(res)}")
        ph.closePort()
        return 1
    print(f"✓ ID {sid} 扭矩已{'使能' if on else '关闭（可以自由转动舵机了）'}")
    ph.closePort()
    return 0


def cmd_move(args):
    if len(args) < 3:
        print("用法：python3 bench.py move <port> <id> <角度 0~360>")
        return 1
    dev, sid, deg = args[0], int(args[1]), float(args[2])
    pos = int(round(deg * 4096 / 360)) & 0xFFF

    ph, pk = open_port(dev)
    if not ph:
        return 1
    pk.write1ByteTxRx(sid, REG_TORQUE_EN, 1)          # 先使能，否则不动
    res, _ = pk.WritePosEx(sid, pos, 1000, 50)        # 速度 1000，加速度 50
    if res != COMM_SUCCESS:
        print(f"❌ 失败：{pk.getTxRxResult(res)}")
        ph.closePort()
        return 1
    print(f"✓ ID {sid} 转向 {deg}°（寄存器值 {pos}）")
    ph.closePort()
    return 0


CMDS = {
    "scan": cmd_scan,
    "info": cmd_info,
    "id": cmd_id,
    "move": cmd_move,
    "torque": cmd_torque,
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd = sys.argv[1]
    if cmd not in CMDS:
        print(f"未知命令：{cmd}\n可用：{', '.join(CMDS)}")
        return 1
    return CMDS[cmd](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main() or 0)
