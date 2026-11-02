"""A class named like the base it imports, and a cycle of bases."""
from .models import B


class B(B):
    def again(self):
        return self.list()


class P(Q):
    def ping(self):
        return self.missing()


class Q(P):
    pass
