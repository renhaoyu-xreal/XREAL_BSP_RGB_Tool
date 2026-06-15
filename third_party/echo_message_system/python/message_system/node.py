#!/usr/bin/env python3
"""
Node module - base class for all messaging entities.
"""
import logging
import threading
import time
from typing import Optional


class Node:
    """Base node class with logging and lifecycle management."""
    
    def __init__(self, name: str, log_level: int = logging.INFO):
        """
        Initialize a node.
        
        Args:
            name: Node name
            log_level: Logging level
        """
        self.name = name
        self.logger = self._setup_logger(log_level)
        self._running = False
        self._thread: Optional[threading.Thread] = None
        
    def _setup_logger(self, log_level: int) -> logging.Logger:
        """Setup logger for this node."""
        logger = logging.getLogger(self.name)
        logger.setLevel(log_level)
        
        if not logger.handlers:
            handler = logging.StreamHandler()
            formatter = logging.Formatter(
                f'[%(levelname)s] [%(asctime)s] [{self.name}]: %(message)s',
                datefmt='%Y-%m-%d %H:%M:%S'
            )
            handler.setFormatter(formatter)
            logger.addHandler(handler)
        
        return logger
    
    def start(self):
        """Start the node."""
        self._running = True
        self.logger.info(f"Node '{self.name}' started")
    
    def stop(self):
        """Stop the node."""
        self._running = False
        self.logger.info(f"Node '{self.name}' stopped")
    
    def is_running(self) -> bool:
        """Check if node is running."""
        return self._running
    
    def spin(self):
        """Keep node alive (blocking call)."""
        try:
            while self._running:
                time.sleep(0.1)
        except KeyboardInterrupt:
            self.logger.info("Interrupted by user")
            self.stop()
