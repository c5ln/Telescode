"""try/except fallbacks: two project candidates, and an import with a variable."""
try:
    from .util import helper as impl
except ImportError:
    from .sub.helpers import cached as impl

try:
    from zarr.storage import WrapperStore
except ImportError:
    WrapperStore = object


def call():
    return impl()


class Store(WrapperStore):
    pass
