#!/usr/bin/env python3
"""
Example action client that sends goals to fibonacci action server.
Demonstrates how to send goals and monitor feedback/results.
"""
import sys
import os
import time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import ActionClient


def on_feedback(goal_id, feedback):
    """Callback when feedback is received."""
    current = feedback.get('current', 0)
    total = feedback.get('total', 1)
    percentage = feedback.get('percentage', 0)
    print(f"📊 Progress: {current}/{total} ({percentage}%)")


def on_result(goal_id, result, status):
    """Callback when result is received."""
    if result.get('error'):
        print(f"❌ Error: {result['error']}")
    else:
        fibonacci = result.get('fibonacci')
        n = result.get('n')
        print(f"✅ Result: fibonacci({n}) = {fibonacci}")


def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='Fibonacci action client')
    parser.add_argument('n', type=int, nargs='?', default=10,
                        help='Fibonacci index to compute (default: 10)')
    args = parser.parse_args()
    
    # Create action client
    client = ActionClient(
        name='fibonacci_action_client',
        action_name='fibonacci_action', #register param
        goal_host='localhost',
        goal_port=5557,#register param
        feedback_host='localhost',
        feedback_port=5558,#register param
        timeout=5000,
        encoding='json'
    )

    #register param
    #dict_param = {
    #   'n': args.n,
    #   'param1': 'value1',
    #   'param2': 42
    #}
    
    # Set callbacks
    client.set_feedback_callback(on_feedback)
    client.set_result_callback(on_result)
    
    # Start listening for feedback and results
    client.start_listening()

    if not client.wait_for_server(timeout=5000):
        raise Exception("未能连接到 action_server")
    else:
        print("已成功连接到 action_server")
    
    try:
        # Send goal
        print(f"📤 Sending goal: compute fibonacci({args.n})")
        goal_id = client.send_goal({'n': args.n})
        
        # Wait for result
        print("⏳ Waiting for result...")
        result, status = client.wait_for_result(goal_id, timeout=60000)
        print(f"🎉 Goal completed with status: {status.value}")
        
    except TimeoutError as e:
        print(f"⏱️  Error: {e}")
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        client.close()


if __name__ == '__main__':
    main()
