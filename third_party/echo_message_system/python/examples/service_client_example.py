#!/usr/bin/env python3
"""
Example service client that calls addition service.
"""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from message_system import ServiceClient


def main():
    # Get numbers from command line or use defaults
    if len(sys.argv) >= 3:
        try:
            a = int(sys.argv[1])
            b = int(sys.argv[2])
        except ValueError:
            print("Error: Arguments must be integers")
            print("Usage: python service_client_example.py <number1> <number2>")
            return
    else:
        a = 10
        b = 20
        print(f"Using default values: a={a}, b={b}")
        print("Usage: python service_client_example.py <number1> <number2>")
    
    # Create service client
    client = ServiceClient(
        name='add_client',
        service_name='add_two_ints',
        host='localhost',
        port=5556,
        timeout=5000
    )
    
    try:
        # Call service
        print(f"\n📤 Sending request: a={a}, b={b}")
        response = client.call({'a': a, 'b': b})
        
        # Display result
        print(f"📥 Received response:")
        print(f"   Sum: {response['sum']}")
        print(f"   Operation: {response['operation']}")
        print(f"\n✅ Result: {a} + {b} = {response['sum']}")
        
    except TimeoutError as e:
        print(f"❌ Error: {e}")
        print("Make sure the service server is running!")
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        client.close()


if __name__ == '__main__':
    main()
