import os
from pathlib import Path

def greet(name, greeting="Hello"):
    return f"{greeting}, {name}"

def process_files(directory):
    files = os.listdir(directory)
    result = greet("world")

    def inner_helper(x):
        return x * 2

    return result
