#!/usr/bin/env python3
"""
Example showing multiple publishers and subscribers on different topics.
"""
import time
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import Publisher, Subscriber
import threading


def sensor_callback(data):
    """Handle sensor messages."""
    print(f"🌡️  Sensor: temp={data['temperature']}°C, humidity={data['humidity']}%")


def status_callback(data):
    """Handle status messages."""
    print(f"📊 Status: {data['status']} (uptime: {data['uptime']}s)")


def run_publishers():
    """Run multiple publishers."""
    sensor_pub = Publisher(name='sensor_publisher', topic='sensors', port=5555)
    status_pub = Publisher(name='status_publisher', topic='status', port=5555)
    
    sensor_pub.start()
    status_pub.start()
    
    counter = 0
    try:
        while True:
            # Publish sensor data
            sensor_pub.publish({
                'temperature': 20 + (counter % 10),
                'humidity': 50 + (counter % 20)
            })
            
            # Publish status data
            status_pub.publish({
                'status': 'running',
                'uptime': counter
            })
            
            counter += 1
            time.sleep(2)
            
    except KeyboardInterrupt:
        pass
    finally:
        sensor_pub.close()
        status_pub.close()


def run_subscribers():
    """Run multiple subscribers."""
    sensor_sub = Subscriber(
        name='sensor_subscriber',
        topic='sensors',
        callback=sensor_callback,
        port=5555
    )
    
    status_sub = Subscriber(
        name='status_subscriber',
        topic='status',
        callback=status_callback,
        port=5555
    )
    
    sensor_sub.start()
    status_sub.start()
    
    try:
        while True:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        sensor_sub.close()
        status_sub.close()


def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python multi_topic_example.py pub    # Run publishers")
        print("  python multi_topic_example.py sub    # Run subscribers")
        return
    
    mode = sys.argv[1]
    
    if mode == 'pub':
        print("🚀 Starting publishers...")
        run_publishers()
    elif mode == 'sub':
        print("🚀 Starting subscribers...")
        run_subscribers()
    else:
        print(f"Unknown mode: {mode}")


if __name__ == '__main__':
    main()
