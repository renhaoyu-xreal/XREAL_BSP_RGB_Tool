#!/usr/bin/env python3
"""
Example action server that performs a long-running task (fibonacci calculation).
Demonstrates how to send feedback and results using ActionServer.
"""
import sys
import os
import time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import ActionServer, GoalStatus


def fibonacci_callback(goal_id, goal_data, server):
    """
    Action callback to compute fibonacci number with progress feedback.
    
    Args:
        goal_id: Unique goal identifier
        goal_data: dict with 'n' key for fibonacci index
        server: ActionServer instance to send feedback/results
    """
    n = goal_data.get('n', 10)
    
    print(f"🚀 Starting fibonacci calculation for n={n} (goal_id: {goal_id})")
    
    # Validate input
    if n < 0:
        server.send_result(goal_id, {'error': 'n must be non-negative'}, GoalStatus.FAILED)
        return
    
    if n > 50:
        server.send_result(goal_id, {'error': 'n must be <= 50'}, GoalStatus.FAILED)
        return
    
    # Compute fibonacci with feedback
    a, b = 0, 1
    for i in range(n):
        a, b = b, a + b
        
        # Send feedback every iteration
        feedback = {
            'current': i + 1,
            'total': n,
            'percentage': int((i + 1) / n * 100) if n > 0 else 100
        }
        server.send_feedback(goal_id, feedback)
        
        # Simulate some work
        time.sleep(0.1)
    
    # Send final result
    result = {'fibonacci': a, 'n': n}
    print(f"✅ Fibonacci({n}) = {a}")
    server.send_result(goal_id, result, GoalStatus.SUCCEEDED)


def main():
    # Create action server
    action_server = ActionServer(
        name='fibonacci_action_server',
        action_name='fibonacci_action',
        callback=fibonacci_callback,
        goal_port=5557,
        feedback_port=5558,
        encoding='json'
    )
    
    action_server.start()
    
    try:
        # Keep server running
        action_server.spin()
    except KeyboardInterrupt:
        action_server.logger.info("Shutting down...")
    finally:
        action_server.close()


if __name__ == '__main__':
    main()
