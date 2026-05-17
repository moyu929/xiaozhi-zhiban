#!/usr/bin/env python3
"""
sair重启脚本 - 完整版

问题原因：sair启动时创建共享内存、信号量、消息队列等IPC资源，退出时不会清理
解决方案：手动清理这些残留资源后重启

需要清理的资源：
1. 共享内存: /dev/shm/sair_*, /dev/shm/smart_player_*, /dev/shm/applib_*, /dev/shm/app_running_list, /dev/shm/configpart_shm
2. 信号量: /dev/shm/sem.applib_sem_lock, /dev/shm/sem.configpart_sem
3. Socket: /tmp/service/sair*, /tmp/service/smart_player*
4. 消息队列: /sair_mq, /smart_player_mq

用法: python restart_sair.py
"""
import telnetlib
import time
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

HOST = cfg["device_ip"]
PORT = cfg["device_telnet_port"]

def run_cmd(tn, cmd, wait=1.0):
    tn.write((cmd + "\n").encode())
    time.sleep(wait)
    return tn.read_very_eager().decode('utf-8', errors='ignore')

def restart_sair():
    print("连接设备...")
    tn = telnetlib.Telnet(HOST, PORT)
    time.sleep(0.5)
    
    print("1. 停止sair相关进程...")
    run_cmd(tn, "killall sair smart_player 2>/dev/null; sleep 1")
    
    print("2. 清理共享内存...")
    run_cmd(tn, "rm -f /dev/shm/sair_*_sync_shm /dev/shm/smart_player_*_sync_shm")
    run_cmd(tn, "rm -f /dev/shm/applib_dir_cfg /dev/shm/app_running_list /dev/shm/configpart_shm")
    
    print("3. 清理信号量...")
    run_cmd(tn, "rm -f /dev/shm/sem.applib_sem_lock /dev/shm/sem.configpart_sem")
    
    print("4. 清理socket...")
    run_cmd(tn, "rm -f /tmp/service/sair* /tmp/service/smart_player*")
    
    print("5. 清理消息队列...")
    run_cmd(tn, "rm -f /sair_mq /smart_player_mq 2>/dev/null")
    
    print("6. 启动sair...")
    run_cmd(tn, "/usr/bin/sair &", wait=4)
    
    print("7. 验证启动...")
    time.sleep(1)
    output = run_cmd(tn, "ps | grep sair | grep -v grep")
    
    if 'sair' in output:
        print("\n✓ sair重启成功!")
        print(output)
        
        # 检查唤醒词检测
        output = run_cmd(tn, "dmesg | grep 'wakeup' | tail -3")
        if output.strip():
            print("\n唤醒词检测状态:")
            print(output)
    else:
        print("\n✗ sair启动失败，请检查日志")
        output = run_cmd(tn, "dmesg | grep -iE 'sair|error' | tail -10")
        print(output)
    
    tn.close()

if __name__ == "__main__":
    try:
        restart_sair()
    except Exception as e:
        print(f"错误: {e}")
        sys.exit(1)
