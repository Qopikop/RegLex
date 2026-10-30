// Basic: typed declarations, precedence, integer vs floating-point division,
// string concatenation and Java's fixed-width integer wrap-around.
public class Basics {
    static final int WIDTH = 12;
    static final int HEIGHT = 5;

    public static void main(String[] args) {
        int a = 7, b = 3;
        int result = a + b * 2;
        int grouped = (a + b) * 2;
        int area = WIDTH * HEIGHT;
        int unused = area * 100;
        double ratio = (double) area / (2 * (WIDTH + HEIGHT));
        int max = Integer.MAX_VALUE;
        int wrapped = max + 1;                 // defined in Java: wraps to MIN_VALUE
        long wide = (long) max + 1;
        char grade = 'A';
        grade += 2;
        boolean ok = result > grouped || area == 60;
        System.out.println("result=" + result + " grouped=" + grouped);
        System.out.println("area=" + area + " ratio=" + ratio);
        System.out.println("-17 / 5 = " + (-17 / 5) + ", -17 % 5 = " + (-17 % 5));
        System.out.println("wrapped=" + wrapped + " wide=" + wide);
        System.out.println("grade=" + grade + " ok=" + ok + " 1/3=" + (1.0 / 3));
    }
}
