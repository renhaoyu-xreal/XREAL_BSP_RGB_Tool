"""
麦克风录音工作器

功能：
- 自动检测 XREAL/Hylla USB 音频设备
- 查询设备支持的最高录音参数
- 后台线程执行 arecord 录音，跟随录制生命周期
- start() 开始录音，stop() 终止录音
- 保存为 mic_record.wav

"""

import os
import re
import logging
import subprocess
import threading
from typing import Optional

logger = logging.getLogger(__name__)


class MicRecordWorker:
    """麦克风录音工作器"""

    def __init__(self, save_dir: str):
        """
        Args:
            save_dir: 录音文件保存目录
        """
        self.save_dir = save_dir
        self._process: Optional[subprocess.Popen] = None
        self._thread: Optional[threading.Thread] = None
        self._is_running = False

    def start(self):
        """启动录音（后台线程）"""
        if self._is_running:
            return

        self._is_running = True
        self._thread = threading.Thread(target=self._record, daemon=True)
        self._thread.start()
        logger.info("[MicRecordWorker] 录音线程已启动")

    def stop(self):
        """停止录音"""
        if not self._is_running:
            return

        self._is_running = False

        # 终止 arecord 进程
        if self._process and self._process.poll() is None:
            try:
                self._process.terminate()
                self._process.wait(timeout=5)
                logger.info("[MicRecordWorker] arecord 进程已终止")
            except subprocess.TimeoutExpired:
                self._process.kill()
                logger.warning("[MicRecordWorker] arecord 进程被强制终止")
            except Exception as e:
                logger.error(f"[MicRecordWorker] 终止 arecord 失败: {e}")

        # 等待线程结束
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5.0)

        logger.info("[MicRecordWorker] 已停止")

    def _record(self):
        """录音主逻辑"""
        output_path = os.path.join(self.save_dir, "mic_record.wav")
        logger.info(f"[MicRecordWorker] 开始录制: {output_path}")

        try:
            # 1. 检测音频设备
            device, device_info = self._detect_device()
            if not device:
                logger.error("[MicRecordWorker] 未找到可用音频设备，跳过录音")
                return

            # 写入设备信息到 record_info.txt
            self._append_record_info(device_info)

            # 2. 查询最高参数
            best_format, best_rate, best_channels, params_info = self._query_best_params(device)
            self._append_record_info(params_info)

            # 3. 执行录音（不设 -d 时长，由 stop() 终止 arecord 进程）
            cmd = [
                "arecord", "-D", device,
                "-f", best_format,
                "-r", best_rate,
                "-c", best_channels,
                output_path
            ]
            logger.info(f"[MicRecordWorker] 执行录音命令: {' '.join(cmd)}")
            self._append_record_info(f"录音命令: {' '.join(cmd)}")

            self._process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )

            # 等待进程结束（会被 stop() 中的 terminate 终止）
            stdout, stderr = self._process.communicate()

            if self._process.returncode == 0 or self._process.returncode == -15:
                # 0=正常结束, -15=SIGTERM（被 stop() 终止，正常情况）
                logger.info(f"[MicRecordWorker] 录音完成: {output_path}")
            else:
                logger.error(f"[MicRecordWorker] 录音失败 (code={self._process.returncode}): {stderr.decode()}")

        except Exception as e:
            logger.error(f"[MicRecordWorker] 录音过程中发生错误: {e}", exc_info=True)

    def _detect_device(self):
        """检测 XREAL/Hylla 音频设备，返回 (device_str, info_str)"""
        try:
            result = subprocess.run(
                ["arecord", "-l"],
                capture_output=True, text=True, timeout=10
            )
            output = result.stdout
            logger.info(f"[MicRecordWorker] arecord -l:\n{output}")

            # 寻找 XREAL 或 Hylla 设备
            card_index = "1"  # 默认
            for line in output.splitlines():
                if "XREAL" in line or "Hylla" in line:
                    match = re.search(r'card (\d+):', line)
                    if match:
                        card_index = match.group(1)
                        logger.info(f"[MicRecordWorker] 检测到 XREAL/Hylla 设备: card {card_index}")
                        break

            device = f"hw:{card_index},0"
            info = f"arecord -l:\n{output}\n检测到设备: {device}"
            return device, info

        except Exception as e:
            logger.error(f"[MicRecordWorker] 检测设备失败: {e}")
            return None, f"检测设备失败: {e}"

    def _query_best_params(self, device: str):
        """查询设备支持的最高参数"""
        best_format = "S16_LE"
        best_rate = "16000"
        best_channels = "2"

        try:
            result = subprocess.run(
                ["arecord", "-D", device, "--dump-hw-params"],
                capture_output=True, text=True, timeout=10
            )
            dump_output = result.stdout + result.stderr
            logger.info(f"[MicRecordWorker] dump-hw-params:\n{dump_output}")

            # 解析格式
            if "S32_LE" in dump_output:
                best_format = "S32_LE"
            elif "S16_LE" in dump_output:
                best_format = "S16_LE"

            # 解析采样率
            rate_match = re.search(r'RATE:.*', dump_output)
            if rate_match:
                rates = re.findall(r'(\d+)', rate_match.group(0))
                if rates:
                    best_rate = str(max(map(int, rates)))

            # 解析声道
            ch_match = re.search(r'CHANNELS:.*', dump_output)
            if ch_match:
                channels = re.findall(r'(\d+)', ch_match.group(0))
                if channels:
                    best_channels = str(max(map(int, channels)))

        except Exception as e:
            logger.warning(f"[MicRecordWorker] 查询参数失败，使用默认值: {e}")

        info = f"录音参数: Format={best_format}, Rate={best_rate}, Channels={best_channels}"
        logger.info(f"[MicRecordWorker] {info}")
        return best_format, best_rate, best_channels, info

    def _append_record_info(self, text: str):
        """追加信息到 record_info.txt"""
        try:
            info_path = os.path.join(self.save_dir, "record_info.txt")
            with open(info_path, "a", encoding="utf-8") as f:
                f.write(text + "\n")
        except Exception as e:
            logger.debug(f"[MicRecordWorker] 写入 record_info.txt 失败: {e}")
