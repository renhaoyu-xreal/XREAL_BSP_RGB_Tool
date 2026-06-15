#!/usr/bin/env python3
"""
Publisher module - publishes messages to a topic using ZMQ.
"""
import zmq
from typing import Any
from .node import Node
from .message import Message


class Publisher(Node):
    """Publisher that sends messages to a topic."""
    
    def __init__(self, name: str, topic: str, port: int = 5555, encoding: str = 'json'):
        """
        Initialize a publisher.
        
        Args:
            name: Publisher name
            topic: Topic name to publish on
            port: Port to publish on
            encoding: Message encoding ('json' or 'pickle')
        """
        super().__init__(name)
        self.topic = topic
        self.port = port
        self.encoding = encoding
        
        # Setup ZMQ publisher
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.PUB)
        
        # Configure socket to prevent blocking:
        # 1. Set send high water mark (buffer size) to 10000 messages
        self.socket.setsockopt(zmq.SNDHWM, 10000)
        # 2. Set send timeout to 100ms - drop message if can't send within timeout
        self.socket.setsockopt(zmq.SNDTIMEO, 100)
        # 3. Don't linger on close
        self.socket.setsockopt(zmq.LINGER, 0)
        
        self.socket.bind(f"tcp://*:{port}")
        
        # Give time for socket to bind
        import time
        time.sleep(0.5)
        
        self.logger.info(f"Publisher ready on topic '{topic}' at port {port}")
    
    def publish(self, data: Any):
        """
        Publish a message.
        
        Args:
            data: Data to publish
        """
        msg = Message(data=data, encoding=self.encoding)
        serialized = msg.serialize()
        
        try:
            # Send topic and message (non-blocking with timeout)
            self.socket.send_multipart([
                self.topic.encode('utf-8'),
                serialized
            ])
            self.logger.debug(f"Published to '{self.topic}': {data}")
        except zmq.Again:
            # Send timeout - drop message to avoid blocking
            self.logger.warning(f"Publisher send timeout on '{self.topic}' - message dropped (no subscribers or slow consumer)")
        except Exception as e:
            self.logger.error(f"Publisher send error on '{self.topic}': {e}")
    
    def close(self):
        """Close the publisher."""
        self.socket.close()
        self.context.term()
        self.logger.info("Publisher closed")
