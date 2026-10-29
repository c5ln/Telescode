"""Resolver cases the other fixture files leave out."""
from resapp import helper
from .sub import helpers
from .sub.helpers import B
from .sub import deco


def len(x):
    return 0


def hook():
    return 0


class Child(B):
    def __init__(self):
        self.list()
        self.own()

    def own(self):
        return len(helper(1))


class Grand(Child):
    def go(self):
        return self.build()


class Plain(helpers.B):
    pass


class Outer:
    class Inner:
        def deep(self):
            return 0


def install():
    global hook
    hook = None
    hook()


def local_import():
    from .util import bump
    return bump()


@deco.tag
def tagged():
    return Child()


def walk():
    Outer.Inner.deep(None)
    Child.own(None)
    helpers.cached()
    helper.cache_clear()
    return missing()


class Typed(Child[int]):
    def go(self):
        return self.own()


class Closure:
    def outer(self):
        def inner():
            return self.target()
        return inner

    def target(self):
        return 0


def unbound(self):
    return self.own()
