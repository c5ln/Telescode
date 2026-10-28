import functools


class B:
    """Unrelated to resapp.models.B."""

    def list(self):
        return list(range(3))

    @staticmethod
    def build():
        return B()


@functools.lru_cache(maxsize=None)
def cached():
    return B().list()
