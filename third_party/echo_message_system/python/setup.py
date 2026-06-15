#!/usr/bin/env python3
"""
Setup script for message_system package
"""

from setuptools import setup, find_packages

setup(
    name="echo-message-system",
    version="0.1.0",
    description="Echo Message System - A lightweight message passing framework",
    author="Echo Message System Team",
    packages=find_packages(),
    install_requires=[
        "pyzmq>=25.0.0",
    ],
    python_requires=">=3.7",
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
    ],
)
