#!/usr/bin/env python3
"""
Subscriber module - subscribes to messages from a topic using ZMQ.
"""
import zmq
import threading
import logging
from typing import Callable, Optional
from .node import Node
from .message import Message


class Subscriber(Node):
    """Subscriber that receives messages from a topic."""
    
    def __init__(self, name: str, topic: str, callback: Callable, 
                 host: str = 'localhost', port: int = 5555, encoding: str = 'json'):
        """
        Initialize a subscriber.
        
        Args:
            name: Subscriber name
            topic: Topic name to subscribe to
            callback: Callback function to handle received messages
            host: Host to connect to
            port: Port to connect to
            encoding: Message encoding ('json' or 'pickle')
        """
        super().__init__(name, log_level=logging.WARNING)
        self.topic = topic
        self.callback = callback
        self.host = host
        self.port = port
        self.encoding = encoding
        
        # Setup ZMQ subscriber
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.SUB)
        self.socket.connect(f"tcp://{host}:{port}")
        self.socket.setsockopt_string(zmq.SUBSCRIBE, topic)
        
        self._thread: Optional[threading.Thread] = None
        self.logger.info(f"Subscriber ready for topic '{topic}' on {host}:{port}")
    
    def start(self):
        """Start receiving messages in a background thread."""
        super().start()
        self._thread = threading.Thread(target=self._receive_loop, daemon=True)
        self._thread.start()
    
    def _receive_loop(self):
        """Internal loop to receive messages."""
        while self._running:
            try:
                # Receive topic and message with timeout
                if self.socket.poll(100):  # 100ms timeout
                    topic_bytes, message_bytes = self.socket.recv_multipart()
                    
                    # Decode topic
                    topic = topic_bytes.decode('utf-8')

                    # Deserialize message
                    msg = Message.deserialize(message_bytes, self.encoding)
                    
                    # Call user callback with topic and data
                    self.logger.debug(f"Received on '{topic}': {msg.data}")
                    self.callback(topic, msg.data)

            except Exception as e:
                self.logger.error(f"Error receiving message: {e}")
    
    def close(self):
        """Close the subscriber."""
        self.stop()
        if self._thread:
            self._thread.join(timeout=1.0)
        self.socket.close()
        self.context.term()
        self.logger.info("Subscriber closed")
