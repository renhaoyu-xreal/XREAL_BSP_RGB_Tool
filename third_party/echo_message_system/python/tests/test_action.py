#!/usr/bin/env python3
"""
Unit tests for Action module.
"""
import sys
import os
import time
import unittest
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import ActionServer, ActionClient, GoalStatus


class TestActionServer(unittest.TestCase):
    """Test cases for ActionServer."""
    
    @classmethod
    def setUpClass(cls):
        """Set up test fixtures."""
        cls.server = None
        cls.client = None
    
    @classmethod
    def tearDownClass(cls):
        """Clean up after tests."""
        if cls.server:
            cls.server.close()
        if cls.client:
            cls.client.close()
    
    def test_server_creation(self):
        """Test that server can be created."""
        def dummy_callback(goal_id, goal_data, server):
            server.send_result(goal_id, {'status': 'done'}, GoalStatus.SUCCEEDED)
        
        server = ActionServer(
            name='test_server',
            action_name='test_action',
            callback=dummy_callback,
            goal_port=5559,
            feedback_port=5560
        )
        self.assertIsNotNone(server)
        self.assertEqual(server.action_name, 'test_action')
        server.close()
    
    def test_simple_goal_and_result(self):
        """Test sending a goal and receiving result."""
        
        received_goals = []
        
        def goal_callback(goal_id, goal_data, server):
            received_goals.append((goal_id, goal_data))
            result = {'n': goal_data.get('n', 0), 'square': goal_data.get('n', 0) ** 2}
            server.send_result(goal_id, result, GoalStatus.SUCCEEDED)
        
        # Create server
        server = ActionServer(
            name='square_server',
            action_name='square_action',
            callback=goal_callback,
            goal_port=5561,
            feedback_port=5562,
            encoding='json'
        )
        server.start()
        
        # Give server time to start
        time.sleep(0.5)
        
        # Create client
        client = ActionClient(
            name='square_client',
            action_name='square_action',
            goal_port=5561,
            feedback_port=5562,
            encoding='json',
            timeout=5000
        )
        
        client.set_result_callback(lambda gid, r, s: None)
        client.start_listening()
        
        try:
            # Send goal
            goal_id = client.send_goal({'n': 5})
            
            # Wait for result
            result, status = client.wait_for_result(goal_id, timeout=10000)
            
            # Verify
            self.assertEqual(status, GoalStatus.SUCCEEDED)
            self.assertEqual(result['square'], 25)
            self.assertEqual(result['n'], 5)
            self.assertEqual(len(received_goals), 1)
            
        finally:
            server.close()
            client.close()
    
    def test_goal_with_feedback(self):
        """Test sending goal and receiving feedback."""
        
        def counting_callback(goal_id, goal_data, server):
            count = goal_data.get('count', 5)
            for i in range(count):
                server.send_feedback(goal_id, {'current': i + 1, 'total': count})
                time.sleep(0.05)
            server.send_result(goal_id, {'total_count': count}, GoalStatus.SUCCEEDED)
        
        # Create server
        server = ActionServer(
            name='counter_server',
            action_name='counter_action',
            callback=counting_callback,
            goal_port=5563,
            feedback_port=5564,
            encoding='json'
        )
        server.start()
        
        # Give server time to start
        time.sleep(0.5)
        
        # Create client
        client = ActionClient(
            name='counter_client',
            action_name='counter_action',
            goal_port=5563,
            feedback_port=5564,
            encoding='json'
        )
        
        feedback_list = []
        
        def on_feedback(goal_id, feedback):
            feedback_list.append(feedback)
        
        def on_result(goal_id, result, status):
            pass
        
        client.set_feedback_callback(on_feedback)
        client.set_result_callback(on_result)
        client.start_listening()
        
        try:
            # Send goal
            goal_id = client.send_goal({'count': 3})
            
            # Wait for result
            result, status = client.wait_for_result(goal_id, timeout=10000)
            
            # Verify
            self.assertEqual(status, GoalStatus.SUCCEEDED)
            self.assertGreater(len(feedback_list), 0)
            self.assertEqual(result['total_count'], 3)
            
        finally:
            server.close()
            client.close()
    
    def test_failed_goal(self):
        """Test handling of failed goals."""
        
        def failing_callback(goal_id, goal_data, server):
            n = goal_data.get('n', 0)
            if n < 0:
                server.send_result(goal_id, {'error': 'n must be non-negative'}, GoalStatus.FAILED)
            else:
                server.send_result(goal_id, {'result': n * 2}, GoalStatus.SUCCEEDED)
        
        # Create server
        server = ActionServer(
            name='validator_server',
            action_name='validator_action',
            callback=failing_callback,
            goal_port=5565,
            feedback_port=5566,
            encoding='json'
        )
        server.start()
        
        # Give server time to start
        time.sleep(0.5)
        
        # Create client
        client = ActionClient(
            name='validator_client',
            action_name='validator_action',
            goal_port=5565,
            feedback_port=5566,
            encoding='json'
        )
        
        def on_result(goal_id, result, status):
            pass
        
        client.set_result_callback(on_result)
        client.start_listening()
        
        try:
            # Send invalid goal
            goal_id = client.send_goal({'n': -5})
            
            # Wait for result
            result, status = client.wait_for_result(goal_id, timeout=10000)
            
            # Verify failure
            self.assertEqual(status, GoalStatus.FAILED)
            self.assertIn('error', result)
            
        finally:
            server.close()
            client.close()


if __name__ == '__main__':
    unittest.main()
