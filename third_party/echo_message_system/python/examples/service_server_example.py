#!/usr/bin/env python3
"""
Example service server that provides addition service.
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import Service


def add_two_numbers(request):
    """
    Service callback to add two numbers.
    
    Args:
        request: dict with 'a' and 'b' keys
        
    Returns:
        dict with 'sum' key
    """
    a = request.get('a', 0)
    b = request.get('b', 0)
    result = a + b
    
    print(f"🔢 Computing: {a} + {b} = {result}")
    
    return {
        'sum': result,
        'operation': 'addition'
    }


def main():
    # Create service
    service = Service(
        name='add_service',
        service_name='add_two_ints',
        callback=add_two_numbers,
        port=5556
    )
    
    service.start()
    
    try:
        # Keep service running
        service.spin()
    except KeyboardInterrupt:
        service.logger.info("Shutting down...")
    finally:
        service.close()


if __name__ == '__main__':
    main()
