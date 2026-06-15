#!/usr/bin/env python3
"""
Example subscriber that receives messages.
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import Subscriber


def message_callback(data):
    """Callback function to handle received messages."""
    print(f"📨 Received: {data['text']}")
    print(f"   Timestamp: {data['timestamp']}")
    print(f"   Counter: {data['counter']}")


def main():
    # Create subscriber
    sub = Subscriber(
        name='example_subscriber',
        topic='chatter',
        callback=message_callback,
        host='localhost',
        port=5555
    )
    
    sub.start()
    
    try:
        # Keep running
        sub.spin()
    except KeyboardInterrupt:
        sub.logger.info("Shutting down...")
    finally:
        sub.close()


if __name__ == '__main__':
    main()
