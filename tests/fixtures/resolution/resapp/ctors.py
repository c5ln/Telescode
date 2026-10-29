"""Constructors and class attributes found on a base."""
from zarr.storage import WrapperStore


class Own:
    def __init__(self):
        pass

    def bm(self):
        return 0


class Derived(Own):
    pass


class Bare:
    pass


class Ext(WrapperStore):
    pass


def make():
    Own()
    Derived()
    Bare()
    Ext()
    return Derived.bm(None)
