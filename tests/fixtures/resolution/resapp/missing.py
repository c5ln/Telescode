"""A module of a project package that does not exist, and an outside one."""
import numpy
import resapp.gone
from resapp.gone import thing


def use():
    numpy.zeros(1)
    resapp.gone.f()
    return thing()
