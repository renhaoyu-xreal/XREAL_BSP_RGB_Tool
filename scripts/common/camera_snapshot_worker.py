"""
CameraSnapshotWorker - 双目摄像头快照工作器

在录数据期间：
1. 保存双目摄像头快照（开始/每分钟/结束）
2. 持续记录双目摄像头 RGB 均值到 CSV（每1秒）

保存目录：
- cam0/snapshots/
- cam1/snapshots/
- camera_rgb.csv
"""

import os
import time
import threading
import csv
import logging
import numpy as np
from typing import Optional, Dict, Tuple

logger = logging.getLogger(__name__)

try:
    from PySide6.QtGui import QImage
except ImportError:
    QImage = None


class CameraSnapshotWorker:
    """
    双目摄像头快照工作器

    在独立线程中运行：
    - 按指定时机保存摄像头快照
    - 持续记录双目摄像头 RGB 均值到 CSV
    """

    # CSV 表头
    CSV_HEADER = ["timestamps_ns", "r_left_mean", "g_left_mean", "b_left_mean",
                  "r_right_mean", "g_right_mean", "b_right_mean"]

    def __init__(self, save_dir: str):
        """
        初始化 CameraSnapshotWorker

        Args:
            save_dir: 保存数据的目录路径
        """
        self.save_dir = save_dir

        self._is_running = False
        self._thread: Optional[threading.Thread] = None

        # 配置参数
        self.snapshot_interval_s = 60.0  # 每分钟保存一次快照
        self.rgb_log_interval_s = 1.0    # 每1秒记录一次 RGB 均值

        # 快照目录
        self.cam0_snapshots_dir = os.path.join(save_dir, "cam0", "snapshots")
        self.cam1_snapshots_dir = os.path.join(save_dir, "cam1", "snapshots")

        # RGB CSV 文件路径
        self.csv_path = os.path.join(save_dir, "camera_rgb.csv")
        self._csv_file = None
        self._csv_writer = None

        # 快照计数和时间追踪
        self._snapshot_count = 0
        self._last_snapshot_time = 0
        self._last_rgb_log_time = 0

        # 缓存最新帧（由外部调用 update_frame 更新）
        self._latest_frames: Dict[int, Optional[object]] = {0: None, 1: None}
        self._frame_lock = threading.Lock()

    def update_frame(self, cam_idx: int, qimage):
        """
        更新缓存的最新摄像头帧

        由 _image_data_callback 调用，更新最新帧以供快照使用。

        Args:
            cam_idx: 摄像头索引 (0 或 1)
            qimage: QImage 图片对象
        """
        if cam_idx not in [0, 1]:
            return

        with self._frame_lock:
            # 复制一份避免原始数据被修改
            self._latest_frames[cam_idx] = qimage.copy() if qimage else None

    def _calculate_rgb_mean(self, qimage) -> Tuple[float, float, float]:
        """
        计算 QImage 的 RGB 均值

        Args:
            qimage: QImage 图片对象

        Returns:
            Tuple[float, float, float]: (R均值, G均值, B均值)
        """
        try:
            # 将 QImage 转换为 numpy 数组
            width = qimage.width()
            height = qimage.height()

            # 转换为 RGBA 格式以确保一致性
            from PySide6.QtGui import QImage as QImg
            qimage_rgba = qimage.convertToFormat(QImg.Format.Format_RGBA8888)
            ptr = qimage_rgba.bits()

            # 创建 numpy 数组
            arr = np.array(ptr).reshape((height, width, 4))

            # 计算 RGB 均值（忽略 Alpha 通道）
            r_mean = float(np.mean(arr[:, :, 0]))
            g_mean = float(np.mean(arr[:, :, 1]))
            b_mean = float(np.mean(arr[:, :, 2]))

            return r_mean, g_mean, b_mean

        except Exception as e:
            logger.error(f"计算 RGB 均值失败: {e}")
            return -1.0, -1.0, -1.0

    def _init_csv(self):
        """初始化 CSV 文件"""
        try:
            self._csv_file = open(self.csv_path, 'w', newline='', encoding='utf-8')
            self._csv_writer = csv.writer(self._csv_file)
            self._csv_writer.writerow(self.CSV_HEADER)
            self._csv_file.flush()
            logger.info(f"[CameraSnapshotWorker] CSV 文件已创建: {self.csv_path}")
        except Exception as e:
            logger.error(f"[CameraSnapshotWorker] 创建 CSV 文件失败: {e}")

    def _close_csv(self):
        """关闭 CSV 文件"""
        if self._csv_file:
            try:
                self._csv_file.close()
            except Exception:
                pass
            self._csv_file = None
            self._csv_writer = None

    def _write_rgb_row(self, timestamp_ns: int,
                       r_left: float, g_left: float, b_left: float,
                       r_right: float, g_right: float, b_right: float):
        """写入一行 RGB 数据到 CSV"""
        if self._csv_writer:
            try:
                self._csv_writer.writerow([
                    timestamp_ns,
                    f"{r_left:.2f}" if r_left >= 0 else "-1",
                    f"{g_left:.2f}" if g_left >= 0 else "-1",
                    f"{b_left:.2f}" if b_left >= 0 else "-1",
                    f"{r_right:.2f}" if r_right >= 0 else "-1",
                    f"{g_right:.2f}" if g_right >= 0 else "-1",
                    f"{b_right:.2f}" if b_right >= 0 else "-1",
                ])
                self._csv_file.flush()
            except Exception as e:
                logger.error(f"[CameraSnapshotWorker] 写入 CSV 失败: {e}")

    def _log_rgb_values(self):
        """记录当前帧的 RGB 均值"""
        timestamp_ns = time.time_ns()

        with self._frame_lock:
            frame_left = self._latest_frames[0].copy() if self._latest_frames[0] else None
            frame_right = self._latest_frames[1].copy() if self._latest_frames[1] else None

        # 计算左摄像头 RGB 均值
        if frame_left:
            r_left, g_left, b_left = self._calculate_rgb_mean(frame_left)
        else:
            r_left, g_left, b_left = -1.0, -1.0, -1.0

        # 计算右摄像头 RGB 均值
        if frame_right:
            r_right, g_right, b_right = self._calculate_rgb_mean(frame_right)
        else:
            r_right, g_right, b_right = -1.0, -1.0, -1.0

        # 写入 CSV
        self._write_rgb_row(timestamp_ns, r_left, g_left, b_left, r_right, g_right, b_right)

        logger.debug(f"[CameraSnapshotWorker] RGB: L=({r_left:.1f},{g_left:.1f},{b_left:.1f}) "
                     f"R=({r_right:.1f},{g_right:.1f},{b_right:.1f})")

    def start(self):
        """启动快照工作器"""
        if self._is_running:
            logger.warning("[CameraSnapshotWorker] 已在运行中")
            return

        self._is_running = True

        # 创建快照目录
        os.makedirs(self.cam0_snapshots_dir, exist_ok=True)
        os.makedirs(self.cam1_snapshots_dir, exist_ok=True)

        # 初始化 CSV 文件
        self._init_csv()

        # 启动工作线程
        self._thread = threading.Thread(
            target=self._main_loop,
            name="CameraSnapshotWorker",
            daemon=True
        )
        self._thread.start()

        logger.info(f"[CameraSnapshotWorker] 已启动, 保存目录: {self.save_dir}")

    def stop(self):
        """停止快照工作器"""
        if not self._is_running:
            return

        self._is_running = False

        # 保存结束快照
        self._save_end_snapshot()

        # 记录最后一次 RGB 值
        self._log_rgb_values()

        # 关闭 CSV 文件
        self._close_csv()

        # 等待线程结束
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5.0)

        logger.info("[CameraSnapshotWorker] 已停止")

    def _save_snapshot(self, name: str) -> bool:
        """保存当前缓存帧为快照"""
        success_count = 0

        with self._frame_lock:
            frames_copy = {
                0: self._latest_frames[0].copy() if self._latest_frames[0] else None,
                1: self._latest_frames[1].copy() if self._latest_frames[1] else None
            }

        for cam_idx, frame in frames_copy.items():
            if frame is None:
                logger.warning(f"[CameraSnapshotWorker] cam{cam_idx} 没有可用帧，跳过快照 {name}")
                continue

            # 选择保存目录
            snapshots_dir = self.cam0_snapshots_dir if cam_idx == 0 else self.cam1_snapshots_dir
            filepath = os.path.join(snapshots_dir, f"{name}.png")

            try:
                # 保存为 PNG 格式
                if frame.save(filepath, "PNG"):
                    logger.info(f"[CameraSnapshotWorker] cam{cam_idx} 快照已保存: {filepath}")
                    success_count += 1
                else:
                    logger.error(f"[CameraSnapshotWorker] cam{cam_idx} 快照保存失败: {filepath}")
            except Exception as e:
                logger.error(f"[CameraSnapshotWorker] cam{cam_idx} 保存快照出错: {e}")

        return success_count > 0

    def _save_start_snapshot(self):
        """保存开始时的快照"""
        logger.info("[CameraSnapshotWorker] 正在保存开始快照...")

        # 等待一小段时间让帧缓存填充
        max_wait = 5.0
        wait_interval = 0.2
        waited = 0

        while waited < max_wait:
            with self._frame_lock:
                has_frame = self._latest_frames[0] is not None or self._latest_frames[1] is not None

            if has_frame:
                break

            time.sleep(wait_interval)
            waited += wait_interval

        if self._save_snapshot("cam_start"):
            self._last_snapshot_time = time.time()
            logger.info("[CameraSnapshotWorker] 开始快照已保存")
        else:
            logger.warning("[CameraSnapshotWorker] 开始快照保存失败（可能没有摄像头数据）")

    def _save_end_snapshot(self):
        """保存结束时的快照"""
        logger.info("[CameraSnapshotWorker] 正在保存结束快照...")

        if self._save_snapshot("cam_end"):
            logger.info("[CameraSnapshotWorker] 结束快照已保存")
        else:
            logger.warning("[CameraSnapshotWorker] 结束快照保存失败")

    def _main_loop(self):
        """主工作循环"""
        # 首先保存开始快照
        self._save_start_snapshot()

        # 记录第一次 RGB 值
        self._log_rgb_values()
        self._last_rgb_log_time = time.time()

        while self._is_running:
            time.sleep(1.0)  # 每秒检查一次

            if not self._is_running:
                break

            current_time = time.time()

            # 检查是否需要记录 RGB 均值（每1秒）
            if current_time - self._last_rgb_log_time >= self.rgb_log_interval_s:
                self._log_rgb_values()
                self._last_rgb_log_time = current_time

            # 检查是否需要保存定期快照（每分钟）
            if current_time - self._last_snapshot_time >= self.snapshot_interval_s:
                self._snapshot_count += 1
                snapshot_name = f"cam_{self._snapshot_count}min"

                if self._save_snapshot(snapshot_name):
                    self._last_snapshot_time = current_time
                    logger.info(f"[CameraSnapshotWorker] 定期快照已保存: {snapshot_name}")

    def is_running(self) -> bool:
        """检查工作器是否正在运行"""
        return self._is_running
