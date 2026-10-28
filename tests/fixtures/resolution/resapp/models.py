import os.path
from collections import OrderedDict as OD


class B:
    def list(self):
        return []

    def run(self):
        return self.list()


class E(Exception):
    pass


def make():
    return B()


def first():
    return make()[0].run()
