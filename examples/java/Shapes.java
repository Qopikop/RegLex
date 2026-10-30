// Partial support demo: classes with fields, objects, interfaces and
// exceptions are lexed and parsed; field access, object creation and
// exception handlers are kept as opaque TAC operations.
interface Shape {
    double area();
}

class Circle implements Shape {
    private final double radius;

    Circle(double radius) {
        this.radius = radius;
    }

    public double area() {
        return Math.PI * radius * radius;
    }
}

public class Shapes {
    static double total(Shape[] shapes) {
        double sum = 0;
        for (int i = 0; i < shapes.length; i++) sum += shapes[i].area();
        return sum;
    }

    public static void main(String[] args) {
        Shape[] shapes = { new Circle(1.0), new Circle(2.0) };
        try {
            System.out.printf("total area = %.2f%n", total(shapes));
        } catch (RuntimeException e) {
            System.out.println("failed: " + e.getMessage());
        }
    }
}
