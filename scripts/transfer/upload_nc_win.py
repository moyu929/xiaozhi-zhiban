r"""
NC反向上传 - Windows原生运行版本
PC监听端口, 设备用BusyBox nc(客户端模式)连接取文件

用法 (Windows PowerShell/CMD):
    python scripts\transfer\upload_nc_win.py <本地文件> <远程路径>
"""
import telnetlib
import time
import socket
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

DEVICE_HOST = cfg["device_ip"]
DEVICE_PORT = cfg["device_telnet_port"]
NC_PORT = cfg["nc_upload_port"]

def get_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    ip = s.getsockname()[0]
    s.close()
    return ip

def main():
    if len(sys.argv) < 3:
        print("用法: python upload_nc_win.py <本地文件> <远程路径>")
        print(f"示例: python upload_nc_win.py {cfg['project_dir']}\\build\\sair {cfg['remote_sair_path']}")
        sys.exit(1)

    local_file = sys.argv[1]
    remote_file = sys.argv[2].replace("\\", "/")

    if not os.path.exists(local_file):
        print(f"错误: 文件不存在: {local_file}")
        sys.exit(1)

    file_size = os.path.getsize(local_file)
    lan_ip = get_lan_ip()

    print(f"本机LAN IP: {lan_ip}")
    print(f"文件: {local_file} ({file_size/1024:.1f} KB)")
    print(f"目标: {remote_file}")

    with open(local_file, 'rb') as f:
        data = f.read()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", NC_PORT))
    sock.listen(1)
    sock.settimeout(60)

    print(f"\n监听端口 {NC_PORT} (0.0.0.0)...")

    tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT)
    time.sleep(0.5)

    def run(cmd, wait=1.0):
        tn.write((cmd + "\n").encode())
        time.sleep(wait)
        return tn.read_very_eager().decode("utf-8", errors="ignore")

    run(f"rm -f {remote_file}")

    cmd = f"nc {lan_ip} {NC_PORT} > {remote_file} &"
    print(f"设备执行: {cmd}")
    tn.write((cmd + "\n").encode())
    time.sleep(2)

    print("等待设备连接...")
    try:
        conn, addr = sock.accept()
        print(f"设备已连接: {addr}")

        sent = 0
        chunk_size = 8192
        while sent < len(data):
            chunk = data[sent:sent+chunk_size]
            conn.sendall(chunk)
            sent += len(chunk)
            pct = sent * 100 // len(data)
            if sent % (100*1024) < chunk_size or sent == len(data):
                print(f"  进度: {sent}/{len(data)} ({pct}%)")

        conn.close()
        print("\n数据发送完毕, 等待设备写入...")

        time.sleep(2)

        output = run(f"ls -lh {remote_file}")
        print(f"\n远程文件:\n{output}")

        if "No such" not in output and file_size > 0:
            run(f"chmod +x {remote_file}")
            print("\n✅ 上传成功!")
        else:
            print("\n❌ 上传失败!")

    except socket.timeout:
        print("超时! 设备未能连接")
    finally:
        sock.close()
        tn.close()

if __name__ == "__main__":
    main()
