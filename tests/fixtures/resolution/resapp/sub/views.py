from .. import util
from ..models import B as C
from . import helpers
from ..util import *
import resapp.sub.helpers
import resapp.models as m
from resapp import PublicB


def route(path):
    def wrap(fn):
        return fn
    return wrap


@route("/")
def index():
    from json import dumps
    return dumps(helper(1))


class View(C, m.E, metaclass=type):
    def get(self):
        super().get()
        return util.helper(self)

    def render(self):
        return resapp.sub.helpers.cached()


def make_public():
    return PublicB()
