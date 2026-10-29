"""No __all__: underscore names stay out of `import *`."""
import os as _os


def _priv():
    return 0


def pub():
    return 0
