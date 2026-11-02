list = [1, 2]
counter = 0


def helper(x):
    return len(x)


def bump():
    global counter
    counter = counter + 1
    return counter


def local_len(items):
    len = lambda s: 0
    return len(items)


def _private():
    return list


__all__ = ["list", "counter", "helper", "bump", "local_len"]
