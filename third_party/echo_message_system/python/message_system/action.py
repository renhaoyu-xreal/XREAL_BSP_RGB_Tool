#!/usr/bin/env python3
"""
Action module - provides asynchronous goal-feedback-result pattern.
Inspired by ROS2 Action mechanism.
"""
import zmq
import asyncio
import threading
import uuid
import logging
from typing import Callable, Any, Optional, Dict
from enum import Enum
from .node import Node
from .message import Message


class GoalStatus(Enum):
    """Goal status enumeration."""
    ACCEPTED = "ACCEPTED"
    EXECUTING = "EXECUTING"
    SUCCEEDED = "SUCCEEDED"
    FAILED = "FAILED"
    CANCELED = "CANCELED"


class ActionServer(Node):
    """Action server that handles goals, provides feedback, and returns results."""
    
    def __init__(self, name: str, action_name: str, callback: Callable,
                 goal_port: int = 5557, feedback_port: int = 5558,
                 encoding: str = 'json'):
        """
        Initialize an action server.
        
        Args:
            name: Server node name
            action_name: Action name
            callback: Callback function(goal_id, goal_data) that handles goal execution
                     The callback receives (goal_id, goal_data) and should use
                     server.send_feedback(goal_id, feedback) to send feedback
                     and server.send_result(goal_id, result, status) to send result
            goal_port: Port for receiving goals
            feedback_port: Port for publishing feedback
            encoding: Message encoding ('json' or 'pickle')
        """
        super().__init__(name, log_level=logging.WARNING)
        self.action_name = action_name
        self.callback = callback
        self.goal_port = goal_port
        self.feedback_port = feedback_port
        self.encoding = encoding
        
        # Setup ZMQ sockets
        self.context = zmq.Context()
        
        # REP socket for goal requests
        self.goal_socket = self.context.socket(zmq.REP)
        self.goal_socket.setsockopt(zmq.LINGER, 0)  # Close immediately
        self.goal_socket.bind(f"tcp://*:{goal_port}")
        
        # PUB socket for feedback and result
        self.feedback_socket = self.context.socket(zmq.PUB)
        self.feedback_socket.setsockopt(zmq.LINGER, 0)  # Close immediately
        self.feedback_socket.bind(f"tcp://*:{feedback_port}")
        
        # Track active goals
        self.active_goals: Dict[str, dict] = {}
        self._goal_lock = threading.Lock()
        self._thread: Optional[threading.Thread] = None
        
        self.logger.info(f"Action server '{action_name}' ready on goal_port={goal_port}, feedback_port={feedback_port}")
    
    def start(self):
        """Start the action server in a background thread."""
        super().start()
        self._thread = threading.Thread(target=self._server_loop, daemon=True)
        self._thread.start()
    
    def _server_loop(self):
        """Internal loop to handle goal requests."""
        while self._running:
            try:
                # Wait for goal request with timeout
                if self.goal_socket.poll(100):  # 100ms timeout
                    goal_bytes = self.goal_socket.recv()
                    
                    # Deserialize goal
                    goal_msg = Message.deserialize(goal_bytes, self.encoding)
                    goal_id = goal_msg.data.get('goal_id', str(uuid.uuid4()))
                    goal_data = goal_msg.data.get('goal', {})
                    
                    self.logger.info(f"Received goal '{goal_id}': {goal_data}")
                    
                    # Track the goal
                    with self._goal_lock:
                        self.active_goals[goal_id] = {
                            'status': GoalStatus.ACCEPTED,
                            'data': goal_data
                        }
                    
                    # Send acceptance
                    response = {
                        'goal_id': goal_id,
                        'status': GoalStatus.ACCEPTED.value
                    }
                    response_msg = Message(data=response, encoding=self.encoding)
                    self.goal_socket.send(response_msg.serialize())
                    
                    # Execute goal in a separate thread
                    executor_thread = threading.Thread(
                        target=self._execute_goal,
                        args=(goal_id, goal_data),
                        daemon=True
                    )
                    executor_thread.start()
                    
            except Exception as e:
                self.logger.error(f"Error in server loop: {e}")
    
    def _execute_goal(self, goal_id: str, goal_data: Any):
        """Execute a goal by calling the user's callback."""
        try:
            # 自动处理 ping
            if isinstance(goal_data, dict) and goal_data.get("ping"):
                self.send_result(goal_id, {"pong": True}, status=GoalStatus.SUCCEEDED)
                return

            with self._goal_lock:
                if goal_id in self.active_goals:
                    self.active_goals[goal_id]['status'] = GoalStatus.EXECUTING
            
            # Call user's callback - they should use send_feedback and send_result
            self.callback(goal_id, goal_data, self)
            
        except Exception as e:
            self.logger.error(f"Error executing goal '{goal_id}': {e}")
            # Send error result only if server is still running
            if self._running:
                try:
                    self.send_result(goal_id, {'error': str(e)}, GoalStatus.FAILED)
                except Exception as send_err:
                    self.logger.error(f"Could not send error result: {send_err}")
    
    def send_feedback(self, goal_id: str, feedback: Any):
        """
        Send feedback for a goal.
        
        Args:
            goal_id: Goal identifier
            feedback: Feedback data to send
        """
        msg_data = {
            'type': 'feedback',
            'goal_id': goal_id,
            'feedback': feedback
        }
        msg = Message(data=msg_data, encoding=self.encoding)
        # 发送时带上 goal_id 作为 topic 前缀，避免所有客户端都收到
        self.feedback_socket.send_multipart([goal_id.encode('utf-8'), msg.serialize()])
        self.logger.debug(f"Sent feedback for goal '{goal_id}': {feedback}")
    
    def send_result(self, goal_id: str, result: Any, status: GoalStatus = GoalStatus.SUCCEEDED):
        """
        Send result for a goal.
        
        Args:
            goal_id: Goal identifier
            result: Result data to send
            status: Final status of the goal
        """
        msg_data = {
            'type': 'result',
            'goal_id': goal_id,
            'result': result,
            'status': status.value
        }
        msg = Message(data=msg_data, encoding=self.encoding)
        # 发送时带上 goal_id 作为 topic 前缀，避免所有客户端都收到
        parts = [goal_id.encode('utf-8'), msg.serialize()]
        self.feedback_socket.send_multipart(parts)
        
        # Update goal status
        with self._goal_lock:
            if goal_id in self.active_goals:
                self.active_goals[goal_id]['status'] = status
                # Remove after a delay to maintain history briefly
                threading.Timer(5.0, lambda: self._remove_goal(goal_id)).start()
        
        self.logger.info(f"Sent result for goal '{goal_id}': status={status.value}, result={result}")
    
    def _remove_goal(self, goal_id: str):
        """Remove goal from tracking."""
        with self._goal_lock:
            self.active_goals.pop(goal_id, None)
    
    def close(self):
        """Close the action server."""
        self.stop()
        if self._thread:
            self._thread.join(timeout=1.0)
        self.goal_socket.close()
        self.feedback_socket.close()
        self.context.term()
        self.logger.info("Action server closed")


class ActionClient(Node):
    """Client that sends goals to an action server and receives feedback/results."""
    
    def __init__(self, name: str, action_name: str,
                 goal_host: str = 'localhost', goal_port: int = 5557,
                 feedback_host: str = 'localhost', feedback_port: int = 5558,
                 encoding: str = 'json', timeout: int = 5000):
        """
        Initialize an action client.
        
        Args:
            name: Client node name
            action_name: Action name to connect to
            goal_host: Goal server host
            goal_port: Goal server port
            feedback_host: Feedback server host
            feedback_port: Feedback server port
            encoding: Message encoding ('json' or 'pickle')
            timeout: Goal request timeout in milliseconds
        """
        super().__init__(name, log_level=logging.WARNING)
        self.action_name = action_name
        self.goal_host = goal_host
        self.goal_port = goal_port
        self.feedback_host = feedback_host
        self.feedback_port = feedback_port
        self.encoding = encoding
        self.timeout = timeout
        
        # Setup ZMQ sockets
        self.context = zmq.Context()
        
        # REQ socket for sending goals
        self.goal_socket = self.context.socket(zmq.REQ)
        self.goal_socket.setsockopt(zmq.LINGER, 0)  # Close immediately
        self.goal_socket.connect(f"tcp://{goal_host}:{goal_port}")
        self.goal_socket.setsockopt(zmq.RCVTIMEO, timeout)
        
        # SUB socket for receiving feedback/result
        self.feedback_socket = self.context.socket(zmq.SUB)
        self.feedback_socket.setsockopt(zmq.LINGER, 0)  # Close immediately
        self.feedback_socket.connect(f"tcp://{feedback_host}:{feedback_port}")
        # Subscribe to all messages globally - simpler and avoids "slow joiner" issues
        self.feedback_socket.setsockopt(zmq.SUBSCRIBE, b"")
        
        # Callbacks for feedback and result
        self.feedback_callback: Optional[Callable] = None
        self.result_callback: Optional[Callable] = None
        
        # Track pending goals
        self.pending_goals: Dict[str, dict] = {}
        self._pending_lock = threading.Lock()
        
        # Track subscribed goal_ids for cleanup
        self.subscribed_goal_ids: set = set()
        self._subscribe_lock = threading.Lock()
        
        # Feedback listener thread
        self._listener_thread: Optional[threading.Thread] = None
        
        self.logger.info(f"Action client for '{action_name}' connected to goal={goal_host}:{goal_port}, feedback={feedback_host}:{feedback_port}")
    

    #timeout unit is ms
    def wait_for_server(self, timeout: int = 5000) -> bool:
        """
        等待 action_server 可用，超时返回 False。
        """
        import time
        start = time.time()
        test_goal = {"ping": True}
        while True:
            try:
                # 发送一个测试 goal，不影响正常业务
                goal_id = str(uuid.uuid4())
                request = {
                    'goal_id': goal_id,
                    'goal': test_goal
                }
                request_msg = Message(data=request, encoding=self.encoding)
                self.goal_socket.send(request_msg.serialize())
                response_bytes = self.goal_socket.recv()
                response_msg = Message.deserialize(response_bytes, self.encoding)
                # 收到响应说明 server 可用（REQ-REP channel working）
                # Note: This doesn't verify PUB-SUB channel, caller should add small delay
                return True
            except Exception:
                if (time.time() - start) * 1000 > timeout:
                    return False
                time.sleep(0.1)

    def set_feedback_callback(self, callback: Callable):
        """
        Set callback for feedback messages.
        
        Args:
            callback: Function(goal_id, feedback) called when feedback is received
        """
        self.feedback_callback = callback
    
    def set_result_callback(self, callback: Callable):
        """
        Set callback for result messages.
        
        Args:
            callback: Function(goal_id, result, status) called when result is received
        """
        self.result_callback = callback
    
    def send_goal(self, goal_data: Any) -> str:
        """
        Send a goal to the action server.
        
        Args:
            goal_data: Goal data to send
            
        Returns:
            Goal ID assigned by the server
        """
        goal_id = str(uuid.uuid4())
        
        # Track this goal (init status before sending)
        with self._pending_lock:
            self.pending_goals[goal_id] = {
                'status': GoalStatus.ACCEPTED,
                'data': goal_data
            }
        
        # Send goal request
        request = {
            'goal_id': goal_id,
            'goal': goal_data
        }
        request_msg = Message(data=request, encoding=self.encoding)
        self.goal_socket.send(request_msg.serialize())
        self.logger.info(f"Sent goal '{goal_id}': {goal_data}")
        
        try:
            # Wait for acceptance
            response_bytes = self.goal_socket.recv()
            response_msg = Message.deserialize(response_bytes, self.encoding)
            
            if response_msg.data.get('status') == GoalStatus.ACCEPTED.value:
                # Track this goal
                with self._pending_lock:
                    self.pending_goals[goal_id] = {
                        'status': GoalStatus.ACCEPTED,
                        'data': goal_data
                    }
                
                # 订阅该 goal_id 的 feedback 和 result（避免收到其他 goal 的消息）
                with self._subscribe_lock:
                    if goal_id not in self.subscribed_goal_ids:
                        self.feedback_socket.setsockopt(zmq.SUBSCRIBE, goal_id.encode('utf-8'))
                        self.subscribed_goal_ids.add(goal_id)
                        self.logger.debug(f"Subscribed to goal_id: {goal_id}")
                
                self.logger.info(f"Goal '{goal_id}' accepted by server")
                return goal_id
            else:
                self.logger.error(f"Goal rejected: {response_msg.data}")
                # Clean up on rejection
                with self._pending_lock:
                    self.pending_goals.pop(goal_id, None)
                raise RuntimeError("Goal rejected by server")
                
        except zmq.Again:
            self.logger.error(f"Goal request timeout after {self.timeout}ms")
            raise TimeoutError(f"Goal request timed out after {self.timeout}ms")
    
    def start_listening(self):
        """Start listening for feedback and results in background thread."""
        if not self._running:
            self.start()  # Start the client if not already started
        if self._listener_thread is None:
            self._listener_thread = threading.Thread(target=self._listen_loop, daemon=True)
            self._listener_thread.start()
            self.logger.info("Started listening for feedback and results")
    
    def _listen_loop(self):
        """Internal loop to listen for feedback and results."""
        while self._running:
            try:
                if self.feedback_socket.poll(100):  # 100ms timeout
                    # 接收 multipart 消息：PUB/SUB 模式下第一个 part 是 topic
                    parts = self.feedback_socket.recv_multipart()
                    
                    # PUB/SUB 模式：第一个 part 是 topic (goal_id)，第二个 part 是消息
                    # 但如果只有 1 个 part，说明 topic 被过滤掉了，直接是消息
                    if len(parts) == 1:
                        # 没有 topic，直接是消息（可能是旧版本或配置问题）
                        self.logger.debug(f"Received 1-part message (no topic)")
                        msg_bytes = parts[0]
                    elif len(parts) == 2:
                        # 标准 PUB/SUB 格式：[topic, message]
                        topic, msg_bytes = parts
                        self.logger.debug(f"Received 2-part message")
                    else:
                        # 3+ parts：可能是服务器发送了 [goal_id, goal_id, message] 的错误格式
                        # 尝试使用最后一个 part 作为消息
                        self.logger.warning(f"Received unusual {len(parts)}-part message, using last part")
                        msg_bytes = parts[-1]
                    
                    msg = Message.deserialize(msg_bytes, self.encoding)
                    
                    msg_data = msg.data
                    msg_type = msg_data.get('type')
                    goal_id = msg_data.get('goal_id')
                    
                    if msg_type == 'feedback':
                        feedback = msg_data.get('feedback')
                        self.logger.debug(f"Received feedback for goal '{goal_id}': {feedback}")
                        if self.feedback_callback:
                            self.feedback_callback(goal_id, feedback)
                    
                    elif msg_type == 'result':
                        result = msg_data.get('result')
                        status = GoalStatus(msg_data.get('status', GoalStatus.SUCCEEDED.value))
                        self.logger.info(f"Received result for goal '{goal_id}': status={status.value}")
                        
                        # Update goal status and result
                        with self._pending_lock:
                            if goal_id in self.pending_goals:
                                self.pending_goals[goal_id]['status'] = status
                                self.pending_goals[goal_id]['result'] = result
                        
                        if self.result_callback:
                            self.result_callback(goal_id, result, status)
                    
            except Exception as e:
                self.logger.error(f"Error in listen loop: {e}")
    
    def wait_for_result(self, goal_id: str, timeout: Optional[int] = None, sleep_ms: int = 10) -> tuple:
        """
        Wait for a goal to complete (blocking).

        Args:
            goal_id: Goal ID to wait for
            timeout: Timeout in milliseconds (None for infinite)
            sleep_ms: Sleep interval in milliseconds between checks (default 10)

        Returns:
            Tuple of (result, status)
        """
        import time

        start_time = time.time()
        timeout_sec = timeout / 1000.0 if timeout else None

        while True:
            with self._pending_lock:
                if goal_id in self.pending_goals:
                    goal = self.pending_goals[goal_id]
                    status = goal['status']

                    # Check if goal is complete
                    if status in (GoalStatus.SUCCEEDED, GoalStatus.FAILED, GoalStatus.CANCELED):
                        return goal.get('result'), status

            if timeout_sec and (time.time() - start_time) > timeout_sec:
                raise TimeoutError(f"Waiting for goal '{goal_id}' timed out after {timeout}ms")

            time.sleep(sleep_ms / 1000.0)

    async def wait_for_result_async(self, goal_id: str, timeout: Optional[int] = None, sleep_ms: int = 10) -> tuple:
        """
        Wait for a goal to complete (异步阻塞版本)。

        Args:
            goal_id: Goal ID to wait for
            timeout: Timeout in milliseconds (None for infinite)
            sleep_ms: Sleep interval in milliseconds between checks (default 10)

        Returns:
            Tuple of (result, status)
        """
        start_time = asyncio.get_event_loop().time()
        timeout_sec = timeout / 1000.0 if timeout else None
        
        while True:
            with self._pending_lock:
                if goal_id in self.pending_goals:
                    goal = self.pending_goals[goal_id]
                    status = goal['status']
                    
                    # Check if goal is complete
                    if status in (GoalStatus.SUCCEEDED, GoalStatus.FAILED, GoalStatus.CANCELED):
                        return goal.get('result'), status
            
            if timeout_sec and (asyncio.get_event_loop().time() - start_time) > timeout_sec:
                raise TimeoutError(f"Waiting for goal '{goal_id}' timed out after {timeout}ms")
            
            await asyncio.sleep(sleep_ms / 1000.0)

    def close(self):
        """Close the action client."""
        # 取消所有订阅，避免此 client 继续收到消息
        with self._subscribe_lock:
            for goal_id in self.subscribed_goal_ids:
                try:
                    self.feedback_socket.setsockopt(zmq.UNSUBSCRIBE, goal_id.encode('utf-8'))
                    self.logger.debug(f"Unsubscribed from goal_id: {goal_id}")
                except Exception as e:
                    self.logger.warning(f"Failed to unsubscribe from {goal_id}: {e}")
            self.subscribed_goal_ids.clear()
        
        self.stop()
        if self._listener_thread:
            self._listener_thread.join(timeout=1.0)
        self.goal_socket.close()
        self.feedback_socket.close()
        self.context.term()
        self.logger.info("Action client closed")
