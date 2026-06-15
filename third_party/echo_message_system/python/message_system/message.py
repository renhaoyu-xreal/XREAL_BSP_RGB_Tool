#!/usr/bin/env python3
"""
Message module for serializing and deserializing data.
"""
import json
import pickle
from typing import Any, Dict


class Message:
    """Base class for messages with serialization support."""
    
    def __init__(self, data: Any = None, encoding: str = 'json'):
        """
        Initialize a message.
        
        Args:
            data: The message data (dict, str, bytes, or any serializable object)
            encoding: Encoding method ('json' or 'pickle')
        """
        self.data = data
        self.encoding = encoding
    
    def serialize(self) -> bytes:
        """Serialize message data to bytes."""
        if self.encoding == 'json':
            if isinstance(self.data, (dict, list, str, int, float, bool, type(None))):
                return json.dumps(self.data).encode('utf-8')
            else:
                raise ValueError(f"Data type {type(self.data)} not JSON serializable")
        elif self.encoding == 'pickle':
            return pickle.dumps(self.data)
        else:
            raise ValueError(f"Unknown encoding: {self.encoding}")
    
    @staticmethod
    def deserialize(data: bytes, encoding: str = 'json') -> 'Message':
        """Deserialize bytes to a Message object."""
        if encoding == 'json':
            decoded_data = json.loads(data.decode('utf-8'))
        elif encoding == 'pickle':
            decoded_data = pickle.loads(data)
        else:
            raise ValueError(f"Unknown encoding: {encoding}")
        
        return Message(data=decoded_data, encoding=encoding)
    
    def __repr__(self):
        return f"Message(data={self.data}, encoding={self.encoding})"
