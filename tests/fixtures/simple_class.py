class Animal(Base):
    def speak(self, sound):
        pass

    async def eat(self, food, quantity):
        pass

@dataclass
class Dog(Animal, Serializable):
    def __init__(self, name: str):
        self.name = name

    def bark(self):
        speak("woof")
