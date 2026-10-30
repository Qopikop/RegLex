# Partial support demo: dictionaries, classes with attributes, list
# comprehensions and exception handlers are lexed and parsed; the TAC
# translator keeps them as opaque operations and reports each one.


class Item:
    def __init__(self, name, price, qty):
        self.name = name
        self.price = price
        self.qty = qty

    def value(self):
        return self.price * self.qty


def total_value(items):
    return sum([item.value() for item in items])


stock = {"pen": Item("pen", 1.5, 40), "book": Item("book", 12.0, 3)}
try:
    print("total:", total_value(list(stock.values())))
except KeyError as error:
    print("missing", error)
