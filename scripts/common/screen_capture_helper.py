"""
ScreenCaptureHelper - 眼镜屏幕捕获辅助模块

用于执行 display_debug capture 命令，获取眼镜屏幕截图，
将 YUV 转换为 RGB，计算 RGB 均值，保存截图。
"""

import logging
import os
import shutil
import subprocess
import time
from typing import Any, Optional, Tuple

from PIL import Image, ImageStat

try:
    import numpy as np
except ModuleNotFoundError:  # pragma: no cover - 取决于运行环境
    np = None

try:
    import paramiko
except ModuleNotFoundError:  # pragma: no cover - 取决于运行环境
    paramiko = None

logger = logging.getLogger(__name__)

# SSH 连接常量
XRLINUX_HOSTNAME = "169.254.2.1"
XRLINUX_PORT = 22
XRLINUX_USERNAME = "root"
XRLINUX_PASSWORD = "xreal2017"


class ScreenCaptureHelper:
    """
    屏幕捕获辅助类

    功能：
    1. 通过 SSH 执行 display_debug capture 命令
    2. 通过 SSH cat 读取 YUV 文件
    3. YUV420 转 RGB
    4. 计算 RGB 通道均值
    5. 保存截图为 PNG
    """

    # 眼镜屏幕分辨率
    WIDTH = 3840
    HEIGHT = 1200

    # display_debug 命令的完整路径
    DISPLAY_DEBUG_CMD = "/usr/usrdata/bin/display_debug capture"
    EXPECTED_YUV_SIZE = int(WIDTH * HEIGHT * 1.5)
    YUV_FILE_READY_TIMEOUT_S = 3.0
    YUV_FILE_POLL_INTERVAL_S = 0.1
    SSH_COMPAT_OPTIONS = [
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        "-o", "HostKeyAlgorithms=+ssh-rsa,ssh-dss",
        "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa,ssh-dss",
    ]

    def __init__(self, persistent_connection: bool = True):
        self._ssh: Optional[Any] = None
        self._persistent_connection = persistent_connection and paramiko is not None
        self._use_paramiko = paramiko is not None

    def _get_ssh(self):
        """获取 SSH 连接（复用或新建）"""
        if not self._use_paramiko:
            raise RuntimeError("paramiko unavailable")
        if self._persistent_connection:
            if self._ssh is not None:
                try:
                    transport = self._ssh.get_transport()
                    if transport is not None and transport.is_active():
                        return self._ssh
                except Exception:
                    pass
                self._close_ssh()

            self._ssh = self._create_ssh_connection()
            return self._ssh
        return self._create_ssh_connection()

    def _create_ssh_connection(self):
        """创建新的 SSH 连接"""
        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        ssh.connect(
            XRLINUX_HOSTNAME,
            port=XRLINUX_PORT,
            username=XRLINUX_USERNAME,
            password=XRLINUX_PASSWORD,
            timeout=10,
        )
        logger.debug("[ScreenCaptureHelper] SSH 连接已建立")
        return ssh

    def _close_ssh(self):
        """关闭 SSH 连接"""
        if self._ssh is not None:
            try:
                self._ssh.close()
            except Exception:
                pass
            self._ssh = None

    def close(self):
        """关闭所有连接"""
        self._close_ssh()

    def _run_ssh_subprocess(self, command: str, timeout: int = 10) -> Tuple[int, str, str]:
        sshpass = shutil.which("sshpass")
        ssh = shutil.which("ssh")
        if not sshpass or not ssh:
            return 127, "", "sshpass or ssh not found"

        try:
            result = subprocess.run(
                [
                    sshpass,
                    "-p",
                    XRLINUX_PASSWORD,
                    ssh,
                    "-p",
                    str(XRLINUX_PORT),
                    *self.SSH_COMPAT_OPTIONS,
                    "-o",
                    f"ConnectTimeout={max(1, int(timeout))}",
                    f"{XRLINUX_USERNAME}@{XRLINUX_HOSTNAME}",
                    command,
                ],
                capture_output=True,
                text=True,
                timeout=timeout + 2,
                check=False,
            )
        except Exception as exc:
            return 1, "", str(exc)

        return result.returncode, result.stdout.strip(), result.stderr.strip()

    def _run_ssh_command(self, command: str, timeout: int = 10) -> Tuple[int, str, str]:
        if self._use_paramiko:
            ssh = self._get_ssh()
            stdin, stdout, stderr = ssh.exec_command(command, timeout=timeout)
            exit_status = stdout.channel.recv_exit_status()
            return (
                exit_status,
                stdout.read().decode(errors="ignore").strip(),
                stderr.read().decode(errors="ignore").strip(),
            )

        return self._run_ssh_subprocess(command, timeout=timeout)

    def _cleanup_remote_yuv_files(self):
        """清理历史 dump，避免读到旧文件。"""
        try:
            self._run_ssh_command("rm -f /usrdata/dump_vi_*.yuv 2>/dev/null", timeout=5)
        except Exception as exc:
            logger.debug(f"[ScreenCaptureHelper] 清理历史 YUV 文件失败: {exc}")

    def _find_latest_yuv_file(self) -> Optional[str]:
        """查找最新的 YUV dump 文件。"""
        try:
            _, stdout_text, _ = self._run_ssh_command(
                "ls -t /usrdata/dump_vi_*.yuv 2>/dev/null | head -1",
                timeout=5,
            )
            return stdout_text or None
        except Exception as exc:
            logger.debug(f"[ScreenCaptureHelper] 查找 YUV 文件失败: {exc}")
            return None

    def _get_remote_file_size(self, file_path: str) -> Optional[int]:
        """读取远端文件大小。"""
        size_commands = [
            f"wc -c < '{file_path}' 2>/dev/null",
            f"ls -ln '{file_path}' 2>/dev/null | awk '{{print $5}}'",
        ]

        for command in size_commands:
            try:
                _, stdout_text, _ = self._run_ssh_command(command, timeout=5)
                if not stdout_text:
                    continue

                first_token = stdout_text.split()[0]
                return int(first_token)
            except Exception as exc:
                logger.debug(f"[ScreenCaptureHelper] 获取远端文件大小失败 ({command}): {exc}")

        return None

    def _wait_for_new_yuv_file(self) -> Optional[str]:
        """等待新生成的 YUV 文件出现。"""
        deadline = time.time() + self.YUV_FILE_READY_TIMEOUT_S
        while time.time() < deadline:
            yuv_file_path = self._find_latest_yuv_file()
            if yuv_file_path:
                return yuv_file_path
            time.sleep(self.YUV_FILE_POLL_INTERVAL_S)
        return None

    def _wait_for_yuv_file_ready(self, file_path: str) -> bool:
        """等待 YUV 文件写完整。"""
        deadline = time.time() + self.YUV_FILE_READY_TIMEOUT_S
        last_size = None

        while time.time() < deadline:
            current_size = self._get_remote_file_size(file_path)
            if current_size == self.EXPECTED_YUV_SIZE:
                return True
            last_size = current_size
            time.sleep(self.YUV_FILE_POLL_INTERVAL_S)

        logger.error(
            f"[ScreenCaptureHelper] YUV 文件未写完整: {file_path}, "
            f"期望 {self.EXPECTED_YUV_SIZE}, 最后观测 {last_size}"
        )
        return False

    def capture_screen(self) -> Tuple[Optional[bytes], Optional[int]]:
        """
        执行屏幕捕获命令并下载 YUV 文件

        Returns:
            Tuple[Optional[bytes], Optional[int]]: (YUV 数据, 进程ID)
        """
        try:
            self._cleanup_remote_yuv_files()

            exit_status, _, error = self._run_ssh_command(self.DISPLAY_DEBUG_CMD, timeout=10)
            if error:
                logger.warning(f"[ScreenCaptureHelper] display_debug stderr: {error}")
            if exit_status != 0:
                logger.error(f"[ScreenCaptureHelper] display_debug capture 执行失败: code={exit_status}")
                return None, None

            yuv_file_path = self._wait_for_new_yuv_file()
            if not yuv_file_path:
                logger.error("[ScreenCaptureHelper] 未找到 YUV dump 文件")
                return None, None

            if not self._wait_for_yuv_file_ready(yuv_file_path):
                self._run_ssh_command(f"rm -f '{yuv_file_path}'", timeout=5)
                return None, None

            try:
                pid = int(yuv_file_path.split("dump_vi_")[1].replace(".yuv", ""))
            except (IndexError, ValueError):
                pid = None

            logger.debug(f"[ScreenCaptureHelper] 找到 YUV 文件: {yuv_file_path}, PID: {pid}")

            if self._use_paramiko:
                ssh = self._get_ssh()
                stdin, stdout, stderr = ssh.exec_command(f"cat '{yuv_file_path}'")
                yuv_data = stdout.read()
                if len(yuv_data) == 0:
                    error_msg = stderr.read().decode(errors="ignore").strip()
                    logger.error(f"[ScreenCaptureHelper] 读取 YUV 文件失败: {error_msg}")
                    return None, None
            else:
                sshpass = shutil.which("sshpass")
                ssh = shutil.which("ssh")
                if not sshpass or not ssh:
                    logger.error("[ScreenCaptureHelper] sshpass 或 ssh 不可用，无法读取屏幕截图")
                    return None, None

                result = subprocess.run(
                    [
                        sshpass,
                        "-p",
                        XRLINUX_PASSWORD,
                        ssh,
                        "-p",
                        str(XRLINUX_PORT),
                        *self.SSH_COMPAT_OPTIONS,
                        f"{XRLINUX_USERNAME}@{XRLINUX_HOSTNAME}",
                        f"cat '{yuv_file_path}'",
                    ],
                    capture_output=True,
                    timeout=12,
                    check=False,
                )
                yuv_data = result.stdout
                if len(yuv_data) == 0:
                    logger.error(
                        f"[ScreenCaptureHelper] 读取 YUV 文件失败: "
                        f"{result.stderr.decode(errors='ignore').strip()}"
                    )
                    return None, None

            self._run_ssh_command(f"rm -f '{yuv_file_path}'", timeout=5)
            logger.debug(f"[ScreenCaptureHelper] 下载 YUV 数据 {len(yuv_data)} bytes")
            return yuv_data, pid

        except Exception as exc:
            logger.error(f"[ScreenCaptureHelper] 屏幕捕获失败: {exc}")
            if self._use_paramiko and self._persistent_connection:
                self._close_ssh()
            return None, None

    def _yuv420_to_rgb_with_numpy(self, yuv_data: bytes):
        """优先使用 numpy 路径，和旧版计算方式保持一致。"""
        if np is None:
            return None

        y_size = self.WIDTH * self.HEIGHT
        uv_size = y_size // 4

        y = np.frombuffer(yuv_data[:y_size], dtype=np.uint8).reshape((self.HEIGHT, self.WIDTH))
        u = np.frombuffer(yuv_data[y_size:y_size + uv_size], dtype=np.uint8).reshape((self.HEIGHT // 2, self.WIDTH // 2))
        v = np.frombuffer(yuv_data[y_size + uv_size:], dtype=np.uint8).reshape((self.HEIGHT // 2, self.WIDTH // 2))

        u_upsampled = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1)
        v_upsampled = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1)

        y = y.astype(np.float32)
        u = u_upsampled.astype(np.float32) - 128
        v = v_upsampled.astype(np.float32) - 128

        r = np.clip(y + 1.402 * v, 0, 255).astype(np.uint8)
        g = np.clip(y - 0.344136 * u - 0.714136 * v, 0, 255).astype(np.uint8)
        b = np.clip(y + 1.772 * u, 0, 255).astype(np.uint8)

        return np.stack([r, g, b], axis=-1)

    def _yuv420_to_rgb_with_pillow(self, yuv_data: bytes) -> Optional[Image.Image]:
        """numpy 不可用时走 Pillow 降级路径，保证默认 BSP 链路仍可录制。"""
        try:
            y_size = self.WIDTH * self.HEIGHT
            uv_size = y_size // 4
            y_plane = yuv_data[:y_size]
            u_plane = yuv_data[y_size:y_size + uv_size]
            v_plane = yuv_data[y_size + uv_size:]

            y_img = Image.frombytes("L", (self.WIDTH, self.HEIGHT), y_plane)
            resample = Image.Resampling.NEAREST if hasattr(Image, "Resampling") else Image.NEAREST
            u_img = Image.frombytes("L", (self.WIDTH // 2, self.HEIGHT // 2), u_plane).resize(
                (self.WIDTH, self.HEIGHT), resample=resample
            )
            v_img = Image.frombytes("L", (self.WIDTH // 2, self.HEIGHT // 2), v_plane).resize(
                (self.WIDTH, self.HEIGHT), resample=resample
            )
            return Image.merge("YCbCr", (y_img, u_img, v_img)).convert("RGB")
        except Exception as exc:
            logger.error(f"[ScreenCaptureHelper] Pillow YUV 转 RGB 失败: {exc}")
            return None

    def yuv420_to_rgb(self, yuv_data: bytes) -> Optional[Any]:
        """将 YUV420 数据转换为 RGB 图像对象。"""
        try:
            if len(yuv_data) != self.EXPECTED_YUV_SIZE:
                logger.error(
                    f"[ScreenCaptureHelper] YUV 数据大小不匹配: "
                    f"期望 {self.EXPECTED_YUV_SIZE}, 实际 {len(yuv_data)}"
                )
                return None

            rgb_array = self._yuv420_to_rgb_with_numpy(yuv_data)
            if rgb_array is not None:
                return rgb_array
            return self._yuv420_to_rgb_with_pillow(yuv_data)
        except Exception as exc:
            logger.error(f"[ScreenCaptureHelper] YUV 转 RGB 失败: {exc}")
            return None

    def calculate_rgb_mean(self, rgb_data: Any) -> Tuple[float, float, float]:
        """计算 RGB 三通道的均值。"""
        if np is not None and isinstance(rgb_data, np.ndarray):
            r_mean = float(np.mean(rgb_data[:, :, 0]))
            g_mean = float(np.mean(rgb_data[:, :, 1]))
            b_mean = float(np.mean(rgb_data[:, :, 2]))
            return r_mean, g_mean, b_mean

        if isinstance(rgb_data, Image.Image):
            stat = ImageStat.Stat(rgb_data)
            mean = stat.mean[:3]
            return float(mean[0]), float(mean[1]), float(mean[2])

        raise TypeError(f"Unsupported rgb data type: {type(rgb_data)}")

    def save_screenshot(self, rgb_data: Any, filepath: str) -> bool:
        """将 RGB 数据保存为 PNG 图片。"""
        try:
            os.makedirs(os.path.dirname(filepath), exist_ok=True)
            if isinstance(rgb_data, Image.Image):
                image = rgb_data
            else:
                image = Image.fromarray(rgb_data, mode="RGB")
            image.save(filepath, "PNG")
            logger.debug(f"[ScreenCaptureHelper] 截图已保存: {filepath}")
            return True
        except Exception as exc:
            logger.error(f"[ScreenCaptureHelper] 保存截图失败: {exc}")
            return False

    def capture_and_process(self) -> Tuple[Optional[Tuple[float, float, float]], Optional[Any]]:
        """
        执行完整的捕获和处理流程

        Returns:
            Tuple: ((r_mean, g_mean, b_mean), rgb_data) 或 (None, None)
        """
        yuv_data, _pid = self.capture_screen()
        if yuv_data is None:
            return None, None

        rgb_data = self.yuv420_to_rgb(yuv_data)
        if rgb_data is None:
            return None, None

        rgb_mean = self.calculate_rgb_mean(rgb_data)
        return rgb_mean, rgb_data
