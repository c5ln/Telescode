"""Fallback imports and repeated bindings."""
try:
    from .util import helper
except ImportError:
    helper = None

import xml.dom
import xml.sax

a = a = 1


def use():
    return helper(xml.dom)
