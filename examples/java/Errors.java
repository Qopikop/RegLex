// Error demo: syntax and type errors with locations and suggested fixes.
public class Errors {
    static int half(int x) {
        return x / 2;
    }

    public static void main(String[] args) {
        int count = 3.5;
        String name = 42;
        boolean flag = 1;
        int total = half(4, 5);
        if (count) {
            System.out.println(missing);
        }
        int value = 10
        System.out.println(value);
    }
}
