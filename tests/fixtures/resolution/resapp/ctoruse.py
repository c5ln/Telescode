from . import ctors as M


def use():
    M.Derived()
    return M.Derived.bm(None)
