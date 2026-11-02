"""Two bases defining the same member: the left one wins."""


class Left:
    def get_axis_num(self):
        return 0


class Right:
    def get_axis_num(self):
        return 1


class Both(Left, Right):
    def use(self):
        return self.get_axis_num()
