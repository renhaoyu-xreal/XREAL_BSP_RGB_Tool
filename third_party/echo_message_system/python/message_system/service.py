#!/usr/bin/env python3
"""
Service module - provides request/response pattern using ZMQ.
"""
import zmq
import threading
from typing import Callable, Any, Optional
from .node import Node
from .message import Message


class Service(Node):
    """Service that handles requests and sends responses."""
    
    def __init__(self, name: str, service_name: str, callback: Callable,
                 port: int = 5556, encoding: str = 'json'):
        """
        Initialize a service.
        
        Args:
            name: Service node name
            service_name: Service name
            callback: Callback function(request) -> response
            port: Port to listen on
            encoding: Message encoding
        """
        super().__init__(name)
        self.service_name = service_name
        self.callback = callback
        self.port = port
        self.encoding = encoding
        
        # Setup ZMQ REP socket
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.REP)
        self.socket.bind(f"tcp://*:{port}")
        
        self._thread: Optional[threading.Thread] = None
        self.logger.info(f"Service '{service_name}' ready on port {port}")
    
    def start(self):
        """Start the service in a background thread."""
        super().start()
        self._thread = threading.Thread(target=self._service_loop, daemon=True)
        self._thread.start()
    
    def _service_loop(self):
        """Internal loop to handle service requests."""
        while self._running:
            try:
                # Wait for request with timeout
                if self.socket.poll(100):  # 100ms timeout
                    request_bytes = self.socket.recv()
                    
                    # Deserialize request
                    request_msg = Message.deserialize(request_bytes, self.encoding)
                    self.logger.info(f"Received request: {request_msg.data}")
                    
                    # Process request
                    response_data = self.callback(request_msg.data)
                    
                    # Send response
                    response_msg = Message(data=response_data, encoding=self.encoding)
                    self.socket.send(response_msg.serialize())
                    self.logger.info(f"Sent response: {response_data}")
                    
            except Exception as e:
                self.logger.error(f"Error handling request: {e}")
                # Send error response
                error_msg = Message(data={'error': str(e)}, encoding=self.encoding)
                self.socket.send(error_msg.serialize())
    
    def close(self):
        """Close the service."""
        self.stop()
        if self._thread:
            self._thread.join(timeout=1.0)
        self.socket.close()
        self.context.term()
        self.logger.info("Service closed")


class ServiceClient(Node):
    """Client that sends requests to a service."""
    
    def __init__(self, name: str, service_name: str,
                 host: str = 'localhost', port: int = 5556, 
                 encoding: str = 'json', timeout: int = 5000):
        """
        Initialize a service client.
        
        Args:
            name: Client node name
            service_name: Service name to connect to
            host: Service host
            port: Service port
            encoding: Message encoding
            timeout: Request timeout in milliseconds
        """
        super().__init__(name)
        self.service_name = service_name
        self.host = host
        self.port = port
        self.encoding = encoding
        self.timeout = timeout
        
        # Setup ZMQ REQ socket
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.REQ)
        self.socket.connect(f"tcp://{host}:{port}")
        self.socket.setsockopt(zmq.RCVTIMEO, timeout)
        
        self.logger.info(f"Service client for '{service_name}' connected to {host}:{port}")
    
    def call(self, request_data: Any) -> Any:
        """
        Call the service with request data.
        
        Args:
            request_data: Data to send in request
            
        Returns:
            Response data from service
        """
        # Send request
        request_msg = Message(data=request_data, encoding=self.encoding)
        self.socket.send(request_msg.serialize())
        self.logger.info(f"Sent request: {request_data}")
        
        try:
            # Wait for response
            response_bytes = self.socket.recv()
            response_msg = Message.deserialize(response_bytes, self.encoding)
            self.logger.info(f"Received response: {response_msg.data}")
            return response_msg.data
            
        except zmq.Again:
            self.logger.error(f"Request timeout after {self.timeout}ms")
            raise TimeoutError(f"Service call timed out after {self.timeout}ms")
    
    def close(self):
        """Close the service client."""
        self.socket.close()
        self.context.term()
        self.logger.info("Service client closed")
