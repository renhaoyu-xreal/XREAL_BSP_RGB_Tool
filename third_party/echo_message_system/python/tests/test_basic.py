#!/usr/bin/env python3
"""
Basic tests for the message system.
"""
import time
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import Publisher, Subscriber, Service, ServiceClient


def test_pub_sub():
    """Test publisher and subscriber."""
    print("Testing Publisher/Subscriber...")
    
    received_messages = []
    
    def callback(data):
        received_messages.append(data)
    
    # Create publisher and subscriber
    pub = Publisher(name='test_pub', topic='test', port=5557)
    sub = Subscriber(name='test_sub', topic='test', callback=callback, port=5557)
    
    pub.start()
    sub.start()
    
    # Publish messages
    time.sleep(0.5)  # Wait for connection
    pub.publish({'msg': 'test1'})
    pub.publish({'msg': 'test2'})
    
    time.sleep(0.5)  # Wait for messages
    
    # Check results
    assert len(received_messages) >= 2
    print(f"✅ Received {len(received_messages)} messages")
    
    pub.close()
    sub.close()


def test_service():
    """Test service and client."""
    print("Testing Service/Client...")
    
    def add_callback(request):
        return {'sum': request['a'] + request['b']}
    
    # Create service and client
    service = Service(
        name='test_service',
        service_name='add',
        callback=add_callback,
        port=5558
    )
    
    client = ServiceClient(
        name='test_client',
        service_name='add',
        port=5558
    )
    
    service.start()
    time.sleep(0.5)  # Wait for service to start
    
    # Call service
    response = client.call({'a': 10, 'b': 20})
    
    # Check result
    assert response['sum'] == 30
    print(f"✅ Service returned correct result: {response['sum']}")
    
    service.close()
    client.close()


if __name__ == '__main__':
    print("Running tests...\n")
    
    try:
        test_pub_sub()
        print()
        test_service()
        print("\n✅ All tests passed!")
    except Exception as e:
        print(f"\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
