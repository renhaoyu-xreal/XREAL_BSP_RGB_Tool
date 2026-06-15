#!/usr/bin/env python3
"""
Example publisher that sends messages periodically.
"""
import time
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import Publisher


def main():
    # Create publisher
    pub = Publisher(
        name='example_publisher',
        topic='chatter',
        port=5555
    )
    
    pub.start()
    
    counter = 0
    try:
        while True:
            # Create message
            message = {
                'text': f'Hello World! Message #{counter}',
                'timestamp': time.time(),
                'counter': counter
            }
            
            # Publish message
            pub.publish(message)
            pub.logger.info(f"Published: {message['text']}")
            
            counter += 1
            time.sleep(1)
            
    except KeyboardInterrupt:
        pub.logger.info("Shutting down...")
    finally:
        pub.close()


if __name__ == '__main__':
    main()
