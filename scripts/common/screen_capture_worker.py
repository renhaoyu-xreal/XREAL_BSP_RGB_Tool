"""
ScreenCaptureWorker - 镜片屏幕捕获工作器

在录数据期间持续捕获眼镜镜片屏幕状态：
- 保存镜片截图（开始/每分钟/结束）
- 每 ~7 秒记录 RGB 均值到 CSV 文件

保存目录：
- screenshots/
- record_screen_rgb_info.csv
"""

import os
import csv
import time
import logging
import threading
from typing import Optional
from scripts.common.screen_capture_helper import ScreenCaptureHelper

logger = logging.getLogger(__name__)


class ScreenCaptureWorker:
    """
    镜片屏幕捕获工作器

    在独立线程中运行，持续捕获镜片屏幕并记录 RGB 均值。
    """

    def __init__(self, save_dir: str, capture_helper: Optional[ScreenCaptureHelper] = None):
        """
        初始化 ScreenCaptureWorker

        Args:
            save_dir: 保存数据的目录路径
        """
        self.save_dir = save_dir
        self.capture_helper = capture_helper or ScreenCaptureHelper()

        self._is_running = False
        self._thread: Optional[threading.Thread] = None

        # 配置参数
        self.capture_interval_s = 10.0   # 每 10 秒采集一次 RGB
        self.screenshot_interval_s = 60.0  # 每分钟保存一张截图
        self.capture_retry_count = 3
        self.capture_retry_delay_s = 0.35

        # CSV 文件路径
        self.csv_filepath = os.path.join(save_dir, "record_screen_rgb_info.csv")

        # 截图目录
        self.screenshots_dir = os.path.join(save_dir, "screenshots")

        # CSV 写入相关
        self._csv_file = None
        self._csv_writer = None

        # 截图计数
        self._screenshot_count = 0
        self._last_screenshot_time = 0

    def start(self):
        """启动屏幕捕获工作器"""
        if self._is_running:
            logger.warning("[ScreenCaptureWorker] 已在运行中")
            return

        self._is_running = True

        # 创建截图目录
        os.makedirs(self.screenshots_dir, exist_ok=True)

        # 初始化 CSV 文件
        self._init_csv()

        # 启动工作线程
        self._thread = threading.Thread(
            target=self._capture_loop,
            name="ScreenCaptureWorker",
            daemon=True
        )
        self._thread.start()

        logger.info("[ScreenCaptureWorker] 已启动")

    def stop(self):
        """停止屏幕捕获工作器"""
        if not self._is_running:
            return

        self._is_running = False

        # 先等待循环线程退出，避免与结束截图同时使用 SSH 连接
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=15.0)

        # 线程已退出，独占 SSH 连接拍摄结束截图
        self._capture_end_screenshot()

        # 关闭 CSV 文件
        self._close_csv()

        # 关闭 SSH 连接
        self.capture_helper.close()

        logger.info("[ScreenCaptureWorker] 已停止")

    def _init_csv(self):
        """初始化 CSV 文件"""
        try:
            self._csv_file = open(self.csv_filepath, 'w', newline='', encoding='utf-8')
            self._csv_writer = csv.writer(self._csv_file)
            self._csv_writer.writerow(['timestamps_ns', 'r_mean', 'g_mean', 'b_mean'])
            logger.debug(f"[ScreenCaptureWorker] CSV 文件已创建: {self.csv_filepath}")
        except Exception as e:
            logger.error(f"[ScreenCaptureWorker] 创建 CSV 文件失败: {e}")

    def _close_csv(self):
        """关闭 CSV 文件"""
        if self._csv_file:
            try:
                self._csv_file.close()
                self._csv_file = None
                self._csv_writer = None
            except Exception as e:
                logger.error(f"[ScreenCaptureWorker] 关闭 CSV 文件失败: {e}")

    def _write_csv_row(self, timestamp_ns: int, r_mean: float, g_mean: float, b_mean: float):
        """写入一行 CSV 数据"""
        if self._csv_writer:
            try:
                self._csv_writer.writerow([timestamp_ns, f"{r_mean:.4f}", f"{g_mean:.4f}", f"{b_mean:.4f}"])
                self._csv_file.flush()
            except Exception as e:
                logger.error(f"[ScreenCaptureWorker] 写入 CSV 失败: {e}")

    def _save_screenshot(self, rgb_array, name: str) -> bool:
        """保存截图"""
        if rgb_array is None:
            return False

        filepath = os.path.join(self.screenshots_dir, f"{name}.png")
        return self.capture_helper.save_screenshot(rgb_array, filepath)

    def _capture_with_retry(self, purpose: str):
        """执行带重试的屏幕抓取，覆盖瞬时的 YUV 不完整问题。"""
        last_result = (None, None)
        for attempt in range(1, self.capture_retry_count + 1):
            rgb_mean, rgb_array = self.capture_helper.capture_and_process()
            if rgb_mean and rgb_array is not None:
                return rgb_mean, rgb_array

            last_result = (rgb_mean, rgb_array)
            if attempt < self.capture_retry_count:
                logger.warning(
                    f"[ScreenCaptureWorker] {purpose}失败，{self.capture_retry_delay_s:.2f}s 后重试 "
                    f"({attempt}/{self.capture_retry_count})"
                )
                time.sleep(self.capture_retry_delay_s)

        return last_result

    def _capture_start_screenshot(self):
        """捕获并保存开始时的截图"""
        logger.info("[ScreenCaptureWorker] 正在捕获开始截图...")
        rgb_mean, rgb_array = self._capture_with_retry("开始截图捕获")

        if rgb_mean and rgb_array is not None:
            timestamp_ns = time.time_ns()
            self._write_csv_row(timestamp_ns, *rgb_mean)
            self._save_screenshot(rgb_array, "screen_start")
            self._last_screenshot_time = time.time()
            logger.info(f"[ScreenCaptureWorker] 开始截图已保存, RGB均值: R={rgb_mean[0]:.2f}, G={rgb_mean[1]:.2f}, B={rgb_mean[2]:.2f}")
        else:
            logger.warning("[ScreenCaptureWorker] 开始截图捕获失败")

    def _capture_end_screenshot(self):
        """捕获并保存结束时的截图"""
        logger.info("[ScreenCaptureWorker] 正在捕获结束截图...")
        rgb_mean, rgb_array = self._capture_with_retry("结束截图捕获")

        if rgb_mean and rgb_array is not None:
            timestamp_ns = time.time_ns()
            self._write_csv_row(timestamp_ns, *rgb_mean)
            self._save_screenshot(rgb_array, "screen_end")
            logger.info(f"[ScreenCaptureWorker] 结束截图已保存, RGB均值: R={rgb_mean[0]:.2f}, G={rgb_mean[1]:.2f}, B={rgb_mean[2]:.2f}")
        else:
            logger.warning("[ScreenCaptureWorker] 结束截图捕获失败")

    def _capture_loop(self):
        """主捕获循环"""
        # 首先捕获开始截图
        self._capture_start_screenshot()

        while self._is_running:
            loop_start_time = time.time()

            try:
                # 执行捕获和处理
                rgb_mean, rgb_array = self._capture_with_retry("周期截图捕获")

                if rgb_mean:
                    # 记录 RGB 到 CSV
                    timestamp_ns = time.time_ns()
                    self._write_csv_row(timestamp_ns, *rgb_mean)

                    logger.debug(f"[ScreenCaptureWorker] RGB均值: R={rgb_mean[0]:.2f}, G={rgb_mean[1]:.2f}, B={rgb_mean[2]:.2f}")

                    # 检查是否需要保存定期截图（每分钟）
                    current_time = time.time()
                    if current_time - self._last_screenshot_time >= self.screenshot_interval_s:
                        self._screenshot_count += 1
                        screenshot_name = f"screen_{self._screenshot_count}min"
                        if rgb_array is not None:
                            self._save_screenshot(rgb_array, screenshot_name)
                            self._last_screenshot_time = current_time
                            logger.info(f"[ScreenCaptureWorker] 定期截图已保存: {screenshot_name}")

            except Exception as e:
                logger.error(f"[ScreenCaptureWorker] 捕获循环出错: {e}")

            # 计算剩余等待时间
            elapsed = time.time() - loop_start_time
            wait_time = max(0, self.capture_interval_s - elapsed)

            if wait_time > 0 and self._is_running:
                time.sleep(wait_time)

    def is_running(self) -> bool:
        """检查工作器是否正在运行"""
        return self._is_running
